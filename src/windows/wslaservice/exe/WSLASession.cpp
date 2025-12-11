/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLASession.cpp

Abstract:

    This file contains the implementation of the WSLASession COM class.

--*/

#include "precomp.h"
#include "WSLASession.h"
#include "WSLAUserSession.h"
#include "WSLAContainer.h"
#include "ServiceProcessLauncher.h"
#include "WslCoreFilesystem.h"

using wsl::windows::service::wsla::WSLASession;
using wsl::windows::service::wsla::WSLAVirtualMachine;

WSLASession::WSLASession(ULONG id, const WSLA_SESSION_SETTINGS& Settings, WSLAUserSessionImpl& userSessionImpl) :

    m_id(id), m_sessionSettings(Settings), m_userSession(&userSessionImpl), m_displayName(Settings.DisplayName)
{
    WSL_LOG("SessionCreated", TraceLoggingValue(m_displayName.c_str(), "DisplayName"));

    m_virtualMachine = wil::MakeOrThrow<WSLAVirtualMachine>(CreateVmSettings(Settings), userSessionImpl.GetUserSid());

    if (Settings.TerminationCallback != nullptr)
    {
        m_virtualMachine->RegisterCallback(Settings.TerminationCallback);
    }

    m_virtualMachine->Start();

    ConfigureStorage(Settings);

    // Launch the init script.
    // TODO: Replace with something more robust once the final VHD is ready.
    try
    {
        ServiceProcessLauncher launcher{"/bin/sh", {"/bin/sh", "-c", "/etc/lsw-init.sh"}};
        auto result = launcher.Launch(*m_virtualMachine.Get()).WaitAndCaptureOutput();

        THROW_HR_IF_MSG(E_FAIL, result.Code != 0, "Init script failed: %hs", launcher.FormatResult(result).c_str());
    }
    catch (...)
    {
        // Ignore issues launching the init script with custom root VHD's, for convenience.
        // TODO: Remove once the final VHD is ready.
        if (Settings.RootVhdOverride == nullptr)
        {
            throw;
        }
    }

    // Start the event tracker.
    m_eventTracker.emplace(*m_virtualMachine.Get());
}

WSLAVirtualMachine::Settings WSLASession::CreateVmSettings(const WSLA_SESSION_SETTINGS& Settings)
{
    WSLAVirtualMachine::Settings vmSettings{};
    vmSettings.CpuCount = Settings.CpuCount;
    vmSettings.MemoryMb = Settings.MemoryMb;
    vmSettings.NetworkingMode = Settings.NetworkingMode;
    vmSettings.BootTimeoutMs = Settings.BootTimeoutMs;
    vmSettings.FeatureFlags = static_cast<WSLAFeatureFlags>(Settings.FeatureFlags);
    vmSettings.DisplayName = Settings.DisplayName;

    if (Settings.RootVhdOverride != nullptr)
    {
        THROW_HR_IF(E_INVALIDARG, Settings.RootVhdTypeOverride == nullptr);

        vmSettings.RootVhd = Settings.RootVhdOverride;
        vmSettings.RootVhdType = Settings.RootVhdTypeOverride;
    }
    else
    {

#ifdef WSLA_TEST_DISTRO_PATH

        vmSettings.RootVhd = TEXT(WSLA_TEST_DISTRO_PATH);

#else
        vmSettings.RootVhd = std::filesystem::path(common::wslutil::GetMsiPackagePath().value()) / L"wslarootfs.vhd";

#endif

        vmSettings.RootVhdType = "ext4";
    }

    if (Settings.DmesgOutput != 0)
    {
        vmSettings.DmesgHandle.reset(wsl::windows::common::wslutil::DuplicateHandleFromCallingProcess(ULongToHandle(Settings.DmesgOutput)));
    }

    return vmSettings;
}

WSLASession::~WSLASession()
{
    WSL_LOG("SessionTerminated", TraceLoggingValue(m_displayName.c_str(), "DisplayName"));

    std::lock_guard lock{m_lock};

    // Stop the event tracker
    if (m_eventTracker.has_value())
    {
        m_eventTracker->Stop();
    }

    m_containers.clear();

    if (m_virtualMachine)
    {
        m_virtualMachine->OnSessionTerminated();

        // TODO: Signal containerd to exit before unmounting /root.
        LOG_IF_FAILED(m_virtualMachine->Unmount("/root"));

        m_virtualMachine.Reset();
    }

    if (m_userSession != nullptr)
    {
        m_userSession->OnSessionTerminated(this);
    }
}

void WSLASession::ConfigureStorage(const WSLA_SESSION_SETTINGS& Settings)
{
    if (Settings.StoragePath == nullptr)
    {
        // If no storage path is specified, use a tmpfs for convenience.
        m_virtualMachine->Mount("", "/root", "tmpfs", "", 0);
        return;
    }

    std::filesystem::path storagePath{Settings.StoragePath};
    THROW_HR_IF_MSG(E_INVALIDARG, !storagePath.is_absolute(), "Storage path is not absolute: %ls", storagePath.c_str());

    m_storageVhdPath = storagePath / "storage.vhdx";

    std::string diskDevice;
    std::optional<ULONG> diskLun{};
    bool vhdCreated = false;

    auto deleteVhdOnFailure = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
        if (vhdCreated)
        {
            if (diskLun.has_value())
            {
                m_virtualMachine->DetachDisk(diskLun.value());
            }

            auto runAsUser = wil::CoImpersonateClient();
            LOG_IF_WIN32_BOOL_FALSE(DeleteFileW(m_storageVhdPath.c_str()));
        }
    });

    auto result =
        wil::ResultFromException([&]() { diskDevice = m_virtualMachine->AttachDisk(m_storageVhdPath.c_str(), false).second; });

    if (FAILED(result))
    {
        THROW_HR_IF_MSG(
            result,
            result != HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND) && result != HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND),
            "Failed to attach vhd: %ls",
            m_storageVhdPath.c_str());

        // If the VHD wasn't found, create it.
        WSL_LOG("CreateStorageVhd", TraceLoggingValue(m_storageVhdPath.c_str(), "StorageVhdPath"));

        auto runAsUser = wil::CoImpersonateClient();

        std::filesystem::create_directories(storagePath);
        wsl::core::filesystem::CreateVhd(
            m_storageVhdPath.c_str(), Settings.MaximumStorageSizeMb * _1MB, m_userSession->GetUserSid(), false, false);
        vhdCreated = true;

        // Then attach the new disk.
        std::tie(diskLun, diskDevice) = m_virtualMachine->AttachDisk(m_storageVhdPath.c_str(), false);

        // Then format it.
        Ext4Format(diskDevice);
    }

    // Mount the device to /root.
    m_virtualMachine->Mount(diskDevice.c_str(), "/root", "ext4", "", 0);

    deleteVhdOnFailure.release();
}

const std::wstring& WSLASession::DisplayName() const
{
    return m_displayName;
}

ULONG WSLASession::GetId() const noexcept
{
    return m_id;
}

void WSLASession::CopyDisplayName(_Out_writes_z_(bufferLength) PWSTR buffer, size_t bufferLength) const
{
    THROW_HR_IF(E_BOUNDS, m_displayName.size() + 1 > bufferLength);
    wcscpy_s(buffer, bufferLength, m_displayName.c_str());
}

HRESULT WSLASession::PullImage(LPCWSTR Image, const WSLA_REGISTRY_AUTHENTICATION_INFORMATION* RegistryInformation, IProgressCallback* ProgressCallback)
{
    return E_NOTIMPL;
}

HRESULT WSLASession::ImportImage(ULONG Handle, LPCWSTR Image, IProgressCallback* ProgressCallback)
{
    return E_NOTIMPL;
}

HRESULT WSLASession::ListImages(WSLA_IMAGE_INFORMATION** Images, ULONG* Count)
{
    return E_NOTIMPL;
}

HRESULT WSLASession::DeleteImage(LPCWSTR Image)
{
    return E_NOTIMPL;
}

HRESULT WSLASession::CreateContainer(const WSLA_CONTAINER_OPTIONS* containerOptions, IWSLAContainer** Container)
try
{
    RETURN_HR_IF_NULL(E_POINTER, containerOptions);

    std::lock_guard lock{m_lock};
    RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_virtualMachine);

    ClearDeletedContainers();

    // Validate that no container with the same name already exists.
    auto it = m_containers.find(containerOptions->Name);
    RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), it != m_containers.end());

    // Validate that name & images are within length limits.
    RETURN_HR_IF(E_INVALIDARG, strlen(containerOptions->Name) > WSLA_MAX_CONTAINER_NAME_LENGTH);
    RETURN_HR_IF(E_INVALIDARG, strlen(containerOptions->Image) > WSLA_MAX_IMAGE_NAME_LENGTH);

    // TODO: Log entrance into the function.
    auto container = WSLAContainer::Create(*containerOptions, *m_virtualMachine.Get(), *m_eventTracker);

    RETURN_IF_FAILED(container.CopyTo(__uuidof(IWSLAContainer), (void**)Container));

    auto [newElement, inserted] = m_containers.emplace(containerOptions->Name, std::move(container));
    WI_ASSERT(inserted);

    newElement->second->Start(*containerOptions);

    return S_OK;
}
CATCH_RETURN();

HRESULT WSLASession::OpenContainer(LPCSTR Name, IWSLAContainer** Container)
try
{
    std::lock_guard lock{m_lock};
    auto it = m_containers.find(Name);
    RETURN_HR_IF_MSG(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), it == m_containers.end(), "Container not found: '%hs'", Name);

    THROW_IF_FAILED(it->second.CopyTo(__uuidof(IWSLAContainer), (void**)Container));
    return S_OK;
}
CATCH_RETURN();

HRESULT WSLASession::ListContainers(WSLA_CONTAINER** Containers, ULONG* Count)
try
{
    *Count = 0;
    *Containers = nullptr;

    std::lock_guard lock{m_lock};
    ClearDeletedContainers();

    auto output = wil::make_unique_cotaskmem<WSLA_CONTAINER[]>(m_containers.size());

    size_t index = 0;
    for (const auto& [name, container] : m_containers)
    {
        THROW_HR_IF(E_UNEXPECTED, strcpy_s(output[index].Image, container->Image().c_str()) != 0);
        THROW_HR_IF(E_UNEXPECTED, strcpy_s(output[index].Name, name.c_str()) != 0);
        THROW_IF_FAILED(container->GetState(&output[index].State));
        index++;
    }

    *Count = static_cast<ULONG>(m_containers.size());
    *Containers = output.release();
    return S_OK;
}
CATCH_RETURN();

HRESULT WSLASession::GetVirtualMachine(IWSLAVirtualMachine** VirtualMachine)
{
    std::lock_guard lock{m_lock};
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_virtualMachine);

    THROW_IF_FAILED(m_virtualMachine->QueryInterface(__uuidof(IWSLAVirtualMachine), (void**)VirtualMachine));
    return S_OK;
}

HRESULT WSLASession::CreateRootNamespaceProcess(const WSLA_PROCESS_OPTIONS* Options, IWSLAProcess** Process, int* Errno)
try
{
    if (Errno != nullptr)
    {
        *Errno = -1; // Make sure not to return 0 if something fails.
    }

    std::lock_guard lock{m_lock};
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_virtualMachine);

    return m_virtualMachine->CreateLinuxProcess(Options, Process, Errno);
}
CATCH_RETURN();

void WSLASession::Ext4Format(const std::string& Device)
{
    constexpr auto mkfsPath = "/usr/sbin/mkfs.ext4";
    ServiceProcessLauncher launcher(mkfsPath, {mkfsPath, Device});
    auto result = launcher.Launch(*m_virtualMachine.Get()).WaitAndCaptureOutput();

    THROW_HR_IF_MSG(E_FAIL, result.Code != 0, "%hs", launcher.FormatResult(result).c_str());
}

HRESULT WSLASession::FormatVirtualDisk(LPCWSTR Path)
try
{
    THROW_HR_IF_MSG(E_INVALIDARG, !std::filesystem::path(Path).is_absolute(), "FormatVirtualDisk called with a relative path: %ls", Path);

    std::lock_guard lock{m_lock};
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_virtualMachine);

    // Attach the disk to the VM (AttachDisk() performs the access check for the VHD file).
    auto [lun, device] = m_virtualMachine->AttachDisk(Path, false);

    // N.B. DetachDisk calls sync() before detaching.
    auto detachDisk = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [this, lun]() { m_virtualMachine->DetachDisk(lun); });

    // Format it to ext4.
    Ext4Format(device);

    return S_OK;
}
CATCH_RETURN();

void WSLASession::OnUserSessionTerminating()
{
    std::lock_guard lock{m_lock};
    WI_ASSERT(m_userSession != nullptr);

    m_userSession = nullptr;
    m_virtualMachine.Reset();
}

HRESULT WSLASession::Shutdown(ULONG Timeout)
try
{
    std::lock_guard lock{m_lock};
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_virtualMachine);

    THROW_IF_FAILED(m_virtualMachine->Shutdown(Timeout));

    m_virtualMachine.Reset();
    return S_OK;
}
CATCH_RETURN();

void WSLASession::ClearDeletedContainers()
{
    std::lock_guard lock{m_lock};
    auto deleted = std::erase_if(m_containers, [](const auto e) { return e.second->State() == WslaContainerStateDeleted; });

    if (deleted > 0)
    {
        WSL_LOG("ClearedDeletedContainers", TraceLoggingValue(deleted, "Count"));
    }
}
