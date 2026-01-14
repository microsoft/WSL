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

using namespace wsl::windows::common;
using wsl::windows::service::wsla::WSLASession;
using wsl::windows::service::wsla::WSLAVirtualMachine;

constexpr auto c_containerdStorage = "/var/lib/docker";

namespace {

std::pair<std::string, std::string> ParseImage(const std::string& Input)
{
    size_t separator = Input.find(':');
    THROW_HR_IF_MSG(
        E_INVALIDARG,
        separator == std::string::npos || separator >= Input.size() - 1 || separator == 0,
        "Invalid image: %hs",
        Input.c_str());

    return {Input.substr(0, separator), Input.substr(separator + 1)};
}
} // namespace

WSLASession::WSLASession(ULONG id, const WSLA_SESSION_SETTINGS& Settings, WSLAUserSessionImpl& userSessionImpl) :

    m_id(id), m_sessionSettings(Settings), m_displayName(Settings.DisplayName)
{
    WSL_LOG("SessionCreated", TraceLoggingValue(m_displayName.c_str(), "DisplayName"));

    m_virtualMachine.emplace(CreateVmSettings(Settings), userSessionImpl.GetUserSid());

    if (Settings.TerminationCallback != nullptr)
    {
        m_virtualMachine->RegisterCallback(Settings.TerminationCallback);
    }

    m_virtualMachine->Start();

    ConfigureStorage(Settings, userSessionImpl.GetUserSid());

    // Make sure that everything is destroyed correctly if an exception is thrown.
    auto errorCleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
        m_sessionTerminatingEvent.SetEvent();

        if (m_containerdThread.joinable())
        {
            m_containerdThread.join();
        }
    });

    // Launch containerd
    // TODO: Rework the daemon logic so we can have only one thread watching all daemons.
    ServiceProcessLauncher launcher{
        "/usr/bin/dockerd",
        {"/usr/bin/dockerd" /*, "--debug"*/}, // TODO: Flag for --debug.
        {{"PATH=/bin:/usr/local/sbin:/usr/bin:/usr/sbin:/sbin"}},
        common::ProcessFlags::Stdout | common::ProcessFlags::Stderr};
    m_containerdThread = std::thread(&WSLASession::MonitorContainerd, this, launcher.Launch(*m_virtualMachine));

    // Wait for containerd to be ready before starting the event tracker.
    // TODO: Configurable timeout.
    THROW_WIN32_IF_MSG(ERROR_TIMEOUT, !m_containerdReadyEvent.wait(10 * 1000), "Timed out waiting for containerd to start");

    auto [_, __, channel] = m_virtualMachine->Fork(WSLA_FORK::Thread);

    m_dockerClient.emplace(std::move(channel), m_virtualMachine->ExitingEvent(), m_virtualMachine->VmId(), 10 * 1000);

    //  Start the event tracker.
    m_eventTracker.emplace(m_dockerClient.value());

    errorCleanup.release();
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

    Terminate();
}

void WSLASession::ConfigureStorage(const WSLA_SESSION_SETTINGS& Settings, PSID UserSid)
{
    if (Settings.StoragePath == nullptr)
    {
        // If no storage path is specified, use a tmpfs for convenience.
        m_virtualMachine->Mount("", c_containerdStorage, "tmpfs", "", 0);
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
        wsl::core::filesystem::CreateVhd(m_storageVhdPath.c_str(), Settings.MaximumStorageSizeMb * _1MB, UserSid, false, false);
        vhdCreated = true;

        // Then attach the new disk.
        std::tie(diskLun, diskDevice) = m_virtualMachine->AttachDisk(m_storageVhdPath.c_str(), false);

        // Then format it.
        Ext4Format(diskDevice);
    }

    // Mount the device to /root.
    m_virtualMachine->Mount(diskDevice.c_str(), c_containerdStorage, "ext4", "", 0);

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

void WSLASession::OnContainerdLog(const gsl::span<char>& buffer)
try
{
    if (buffer.empty())
    {
        return;
    }

    constexpr auto c_containerdReadyLogLine = "API listen on /var/run/docker.sock";

    std::string entry = {buffer.begin(), buffer.end()};
    WSL_LOG("ContainerdLog", TraceLoggingValue(entry.c_str(), "Content"), TraceLoggingValue(m_displayName.c_str(), "Name"));

    if (!m_containerdReadyEvent.is_signaled())
    {
        if (entry.find(c_containerdReadyLogLine) != std::string::npos)
        {
            m_containerdReadyEvent.SetEvent();
        }
    }
}
CATCH_LOG();

void WSLASession::MonitorContainerd(ServiceRunningProcess&& process)
try
{
    windows::common::relay::MultiHandleWait io;

    // Read stdout & stderr.
    io.AddHandle(std::make_unique<windows::common::relay::LineBasedReadHandle>(
        process.GetStdHandle(1), [&](const auto& data) { OnContainerdLog(data); }, false));

    io.AddHandle(std::make_unique<windows::common::relay::LineBasedReadHandle>(
        process.GetStdHandle(2), [&](const auto& data) { OnContainerdLog(data); }, false));

    // Exit if either the VM terminates or containerd exits.
    io.AddHandle(std::make_unique<windows::common::relay::EventHandle>(process.GetExitEvent(), [&]() { io.Cancel(); }));
    io.AddHandle(std::make_unique<windows::common::relay::EventHandle>(m_sessionTerminatingEvent.get(), [&]() { io.Cancel(); }));

    io.Run({});

    if (!m_sessionTerminatingEvent.is_signaled())
    {
        // If containerd exited before the VM starts terminating, then it exited unexpectedly.
        WSL_LOG("UnexpectedContainerdExit", TraceLoggingValue(m_displayName.c_str(), "SessionDisplayName"));
    }
    else
    {
        // Otherwise, the session is shutting down; terminate containerd before exiting.
        LOG_IF_FAILED(process.Get().Signal(15)); // SIGTERM

        auto code = process.Wait(30 * 1000); // TODO: Configurable timeout.

        WSL_LOG("DockerdExit", TraceLoggingValue(code, "code"));
    }
}
CATCH_LOG();

HRESULT WSLASession::PullImage(LPCSTR ImageUri, const WSLA_REGISTRY_AUTHENTICATION_INFORMATION* RegistryAuthenticationInformation, IProgressCallback* ProgressCallback)
try
{
    UNREFERENCED_PARAMETER(RegistryAuthenticationInformation);
    UNREFERENCED_PARAMETER(ProgressCallback);

    RETURN_HR_IF_NULL(E_POINTER, ImageUri);

    auto [repo, tag] = ParseImage(ImageUri);

    std::lock_guard lock{m_lock};

    auto callback = [&](const std::string& content) {
        WSL_LOG("ImagePullProgress", TraceLoggingValue(ImageUri, "Image"), TraceLoggingValue(content.c_str(), "Content"));
    };

    auto code = m_dockerClient->PullImage(repo.c_str(), tag.c_str(), callback);

    THROW_HR_IF_MSG(E_FAIL, code != 200, "Failed to pull image: %hs", ImageUri);

    return S_OK;
}
CATCH_RETURN();

HRESULT WSLASession::LoadImage(ULONG ImageHandle, IProgressCallback* ProgressCallback, ULONGLONG ContentSize)
try
{
    UNREFERENCED_PARAMETER(ProgressCallback);

    std::lock_guard lock{m_lock};

    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_dockerClient.has_value());

    auto requestContext = m_dockerClient->LoadImage(ContentSize);

    ImportImageImpl(*requestContext, ImageHandle);
    return S_OK;
}
CATCH_RETURN();

HRESULT WSLASession::ImportImage(ULONG ImageHandle, LPCSTR ImageName, IProgressCallback* ProgressCallback, ULONGLONG ContentSize)
try
{
    UNREFERENCED_PARAMETER(ProgressCallback);
    RETURN_HR_IF_NULL(E_POINTER, ImageName);

    auto [repo, tag] = ParseImage(ImageName);

    std::lock_guard lock{m_lock};

    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_dockerClient.has_value());

    auto requestContext = m_dockerClient->ImportImage(repo, tag, ContentSize);

    ImportImageImpl(*requestContext, ImageHandle);
    return S_OK;
}
CATCH_RETURN();

void WSLASession::ImportImageImpl(DockerHTTPClient::HTTPRequestContext& Request, ULONG InputHandle)
{
    wil::unique_handle imageFileHandle{wsl::windows::common::wslutil::DuplicateHandleFromCallingProcess(ULongToHandle(InputHandle))};

    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_dockerClient.has_value());

    relay::MultiHandleWait io;

    std::optional<boost::beast::http::status> importResult;

    auto onHttpResponse = [&](const boost::beast::http::message<false, boost::beast::http::buffer_body>& response) {
        WSL_LOG("ImageImportHttpResponse", TraceLoggingValue(static_cast<int>(response.result()), "StatusCode"));

        importResult = response.result();
    };

    std::string errorJson;
    auto onProgress = [&](const gsl::span<char>& buffer) {
        WI_ASSERT(importResult.has_value());

        if (importResult.value() != boost::beast::http::status::ok)
        {
            // If the import failed, accumulate the error message.
            errorJson.append(buffer.data(), buffer.size());
        }
        else
        {
            // TODO: report progress to caller.
            std::string entry = {buffer.begin(), buffer.end()};
            WSL_LOG("ImageImportProgress", TraceLoggingValue(entry.c_str(), "Content"));
        }
    };

    auto onCompleted = [&]() { io.Cancel(); };

    io.AddHandle(std::make_unique<relay::RelayHandle>(
        common::relay::HandleWrapper{std::move(imageFileHandle)}, common::relay::HandleWrapper{Request.stream.native_handle()}));

    io.AddHandle(std::make_unique<relay::EventHandle>(m_sessionTerminatingEvent.get(), [&]() { THROW_HR(E_ABORT); }));

    io.AddHandle(std::make_unique<DockerHTTPClient::DockerHttpResponseHandle>(
        Request, std::move(onHttpResponse), std::move(onProgress), std::move(onCompleted)));

    io.Run({});

    THROW_HR_IF(E_UNEXPECTED, !importResult.has_value());

    if (importResult.value() != boost::beast::http::status::ok)
    {
        // Import failed, parse the error message.
        auto error = wsl::shared::FromJson<docker_schema::ErrorResponse>(errorJson.c_str());

        // TODO: Return error message to client.
        THROW_HR_MSG(E_FAIL, "Image import failed: %hs", error.message.c_str());
    }
}

HRESULT WSLASession::ListImages(WSLA_IMAGE_INFORMATION** Images, ULONG* Count)
try
{
    *Count = 0;
    *Images = nullptr;

    std::lock_guard lock{m_lock};

    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_dockerClient.has_value());

    auto images = m_dockerClient->ListImages();

    auto output = wil::make_unique_cotaskmem<WSLA_IMAGE_INFORMATION[]>(images.size());

    size_t index = 0;
    for (const auto& e : images)
    {
        // TODO: Find a better way to encode tags;
        // TODO: download_timestamp
        if (!e.RepoTags.empty())
        {
            THROW_HR_IF(E_UNEXPECTED, strcpy_s(output[index].Image, e.RepoTags[0].c_str()) != 0);
        }

        THROW_HR_IF(E_UNEXPECTED, strcpy_s(output[index].Hash, e.Id.c_str()) != 0);
        output[index].Size = e.Size;

        index++;
    }

    *Count = static_cast<ULONG>(images.size());
    *Images = output.release();
    return S_OK;
}
CATCH_RETURN();

HRESULT WSLASession::DeleteImage(LPCWSTR Image)
{
    return E_NOTIMPL;
}

HRESULT WSLASession::CreateContainer(const WSLA_CONTAINER_OPTIONS* containerOptions, IWSLAContainer** Container)
try
{
    RETURN_HR_IF_NULL(E_POINTER, containerOptions);

    // Validate that Image and Name are not null.
    RETURN_HR_IF(E_INVALIDARG, containerOptions->Image == nullptr);
    RETURN_HR_IF(E_INVALIDARG, containerOptions->Name == nullptr);

    std::lock_guard lock{m_lock};
    RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_virtualMachine);

    // Validate that no container with the same name already exists.
    auto it = m_containers.find(containerOptions->Name);
    RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), it != m_containers.end());

    // Validate that name & images are within length limits.
    RETURN_HR_IF(E_INVALIDARG, strlen(containerOptions->Name) > WSLA_MAX_CONTAINER_NAME_LENGTH);
    RETURN_HR_IF(E_INVALIDARG, strlen(containerOptions->Image) > WSLA_MAX_IMAGE_NAME_LENGTH);

    // TODO: Log entrance into the function.
    auto [container, inserted] = m_containers.emplace(
        containerOptions->Name,
        WSLAContainerImpl::Create(
            *containerOptions,
            *m_virtualMachine,
            std::bind(&WSLASession::OnContainerDeleted, this, std::placeholders::_1),
            m_eventTracker.value(),
            m_dockerClient.value()));

    WI_ASSERT(inserted);

    THROW_IF_FAILED(container->second->ComWrapper().QueryInterface(__uuidof(IWSLAContainer), (void**)Container));

    return S_OK;
}
CATCH_RETURN();

HRESULT WSLASession::OpenContainer(LPCSTR Name, IWSLAContainer** Container)
try
{
    std::lock_guard lock{m_lock};
    auto it = m_containers.find(Name);
    RETURN_HR_IF_MSG(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), it == m_containers.end(), "Container not found: '%hs'", Name);

    THROW_IF_FAILED(it->second->ComWrapper().QueryInterface(__uuidof(IWSLAContainer), (void**)Container));

    return S_OK;
}
CATCH_RETURN();

HRESULT WSLASession::ListContainers(WSLA_CONTAINER** Containers, ULONG* Count)
try
{
    *Count = 0;
    *Containers = nullptr;

    std::lock_guard lock{m_lock};

    auto output = wil::make_unique_cotaskmem<WSLA_CONTAINER[]>(m_containers.size());

    size_t index = 0;
    for (const auto& [name, container] : m_containers)
    {
        THROW_HR_IF(E_UNEXPECTED, strcpy_s(output[index].Image, container->Image().c_str()) != 0);
        THROW_HR_IF(E_UNEXPECTED, strcpy_s(output[index].Name, name.c_str()) != 0);
        container->GetState(&output[index].State);
        index++;
    }

    *Count = static_cast<ULONG>(m_containers.size());
    *Containers = output.release();
    return S_OK;
}
CATCH_RETURN();

HRESULT WSLASession::CreateRootNamespaceProcess(const WSLA_PROCESS_OPTIONS* Options, IWSLAProcess** Process, int* Errno)
try
{
    if (Errno != nullptr)
    {
        *Errno = -1; // Make sure not to return 0 if something fails.
    }

    std::lock_guard lock{m_lock};
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_virtualMachine);

    auto process = m_virtualMachine->CreateLinuxProcess(*Options, Errno);
    THROW_IF_FAILED(process.CopyTo(Process));

    return S_OK;
}
CATCH_RETURN();

void WSLASession::Ext4Format(const std::string& Device)
{
    constexpr auto mkfsPath = "/usr/sbin/mkfs.ext4";
    ServiceProcessLauncher launcher(mkfsPath, {mkfsPath, Device});
    auto result = launcher.Launch(*m_virtualMachine).WaitAndCaptureOutput();

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
    LOG_IF_FAILED(Terminate());
}

HRESULT WSLASession::Terminate()
try
{
    // m_sessionTerminatingEvent is always valid, so it can be signalled with the lock.
    // This allows a session to be unblocked if a stuck operation is holding the lock.
    m_sessionTerminatingEvent.SetEvent();

    std::lock_guard lock{m_lock};

    // Stop the event tracker
    if (m_eventTracker.has_value())
    {
        m_eventTracker->Stop();
    }

    // This will delete all containers. Needs to be done before the VM is terminated.
    m_containers.clear();

    m_dockerClient.reset();

    // N.B. The containerd thread can only run if the VM is running.
    if (m_containerdThread.joinable())
    {
        m_containerdThread.join();
    }

    if (m_virtualMachine)
    {
        // N.B. containerd has exited by this point, so unmounting the VHD is safe since no container can be running.
        try
        {
            m_virtualMachine->Unmount(c_containerdStorage);
        }
        CATCH_LOG();

        m_virtualMachine->OnSessionTerminated();
        m_virtualMachine.reset();
    }

    return S_OK;
}
CATCH_RETURN();

HRESULT WSLASession::MountWindowsFolder(LPCWSTR WindowsPath, LPCSTR LinuxPath, BOOL ReadOnly)
try
{
    std::lock_guard lock{m_lock};
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_virtualMachine);

    return m_virtualMachine->MountWindowsFolder(WindowsPath, LinuxPath, ReadOnly);
}
CATCH_RETURN();

HRESULT WSLASession::UnmountWindowsFolder(LPCSTR LinuxPath)
try
{
    std::lock_guard lock{m_lock};
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_virtualMachine);

    return m_virtualMachine->UnmountWindowsFolder(LinuxPath);
}
CATCH_RETURN();

HRESULT WSLASession::MapVmPort(int Family, short WindowsPort, short LinuxPort)
try
{
    std::lock_guard lock{m_lock};
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_virtualMachine);

    m_virtualMachine->MapPort(Family, WindowsPort, LinuxPort);
    return S_OK;
}
CATCH_RETURN();

HRESULT WSLASession::UnmapVmPort(int Family, short WindowsPort, short LinuxPort)
try
{
    std::lock_guard lock{m_lock};
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_virtualMachine);

    m_virtualMachine->UnmapPort(Family, WindowsPort, LinuxPort);
    return S_OK;
}
CATCH_RETURN();

void WSLASession::OnContainerDeleted(const WSLAContainerImpl* Container)
{
    std::lock_guard lock{m_lock};
    WI_VERIFY(std::erase_if(m_containers, [Container](const auto& e) { return e.second.get() == Container; }) == 1);
}

HRESULT WSLASession::GetImplNoRef(_Out_ WSLASession** Session)
{
    // N.B. This returns a raw pointer to the implementation without calling AddRef.
    // The caller must hold a separate strong reference to the owning WSLASession
    // object for at least as long as this pointer is used, and must not store it
    // beyond that lifetime.
    *Session = this;
    return S_OK;
}

bool WSLASession::Terminated()
{
    std::lock_guard lock{m_lock};
    return !m_virtualMachine;
}