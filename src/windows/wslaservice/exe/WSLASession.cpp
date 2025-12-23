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
        {"/usr/bin/dockerd"},
        {{"PATH=/bin:/usr/local/sbin:/usr/bin:/usr/sbin:/sbin"}},
        common::ProcessFlags::Stdout | common::ProcessFlags::Stderr};
    m_containerdThread = std::thread(&WSLASession::MonitorContainerd, this, launcher.Launch(*m_virtualMachine.Get()));

    // Wait for containerd to be ready before starting the event tracker.
    // TODO: Configurable timeout.
    THROW_WIN32_IF_MSG(ERROR_TIMEOUT, !m_containerdReadyEvent.wait(10 * 1000), "Timed out waiting for containerd to start");

    auto [_, __, channel] = m_virtualMachine->Fork(WSLA_FORK::Thread);

    m_dockerClient.emplace(std::move(channel), m_virtualMachine->ExitingEvent(), m_virtualMachine->VmId(), 10 * 1000);

    // WSL_LOG("Info", TraceLoggingValue(response.c_str(), "DockerInfo"));
    //  Start the event tracker.
    //  m_eventTracker.emplace(*m_virtualMachine.Get());

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

    std::lock_guard lock{m_lock};

    // Stop the event tracker
    if (m_eventTracker.has_value())
    {
        m_eventTracker->Stop();
    }

    // This will delete all containers. Needs to be done before the VM is terminated.
    // TODO: If callers still have references to containers, the instances won't actually be deleted.
    m_containers.clear();

    m_sessionTerminatingEvent.SetEvent();

    // N.B. The containerd thread can only run if the VM is running.
    if (m_containerdThread.joinable())
    {
        m_containerdThread.join();
    }

    if (m_virtualMachine)
    {
        // N.B. containerd has exited by this point, so unmounting the VHD is safe since no container can be running.
        LOG_IF_FAILED(m_virtualMachine->Unmount(c_containerdStorage));
        m_virtualMachine->OnSessionTerminated();

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
        wsl::core::filesystem::CreateVhd(
            m_storageVhdPath.c_str(), Settings.MaximumStorageSizeMb * _1MB, m_userSession->GetUserSid(), false, false);
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

    // auto parsed = nlohmann::json::parse(entry);

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
        process.GetStdHandle(1), [&](const auto& data) { OnContainerdLog(data); }));

    io.AddHandle(std::make_unique<windows::common::relay::LineBasedReadHandle>(
        process.GetStdHandle(2), [&](const auto& data) { OnContainerdLog(data); }));

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
        process.Get().Signal(15); // SIGTERM

        process.Wait(30 * 1000); // TODO: Configurable timeout.
    }
}
CATCH_LOG();

HRESULT WSLASession::PullImage(LPCSTR ImageUri, const WSLA_REGISTRY_AUTHENTICATION_INFORMATION* RegistryAuthenticationInformation, IProgressCallback* ProgressCallback)
try
{
    UNREFERENCED_PARAMETER(RegistryAuthenticationInformation);
    UNREFERENCED_PARAMETER(ProgressCallback);

    RETURN_HR_IF_NULL(E_POINTER, ImageUri);

    std::lock_guard lock{m_lock};

    std::string image{ImageUri};
    size_t separator = image.find(':');
    THROW_HR_IF_MSG(E_INVALIDARG, separator == std::string::npos || separator >= image.size() - 1, "Invalid image: %hs", ImageUri);

    auto callback = [&](const std::string &content)
    {
        WSL_LOG("ImagePullProgress", TraceLoggingValue(ImageUri, "Image"), TraceLoggingValue(content.c_str(), "Content"));
    };

    auto code = m_dockerClient->PullImage(image.substr(0, separator).c_str(), image.substr(separator + 1).c_str(), callback);

    THROW_HR_IF_MSG(E_FAIL, code != 200, "Failed to pull image: %hs", ImageUri);

    return S_OK;
}
CATCH_RETURN();

HRESULT WSLASession::LoadImage(ULONG ImageHandle, IProgressCallback* ProgressCallback)
try
{
    UNREFERENCED_PARAMETER(ProgressCallback);

    wil::unique_handle imageFileHandle{wsl::windows::common::wslutil::DuplicateHandleFromCallingProcess(ULongToHandle(ImageHandle))};
    RETURN_HR_IF(E_INVALIDARG, INVALID_HANDLE_VALUE == imageFileHandle.get());

    std::lock_guard lock{m_lock};

    // Directly invoking "nerdctl load" will immediately return with failure
    // "stdin is empty and input flag is not specified".
    // TODO: Change the workaround when nerdctl has a fix.
    ServiceProcessLauncher launcher{
        "/bin/sh", {"/bin/sh", "-c", "cat | /usr/bin/nerdctl load"}, {}, ProcessFlags::Stdin | ProcessFlags::Stdout | ProcessFlags::Stderr};
    auto loadProcess = launcher.Launch(*m_virtualMachine.Get());

    auto loadProcessStdin = loadProcess.GetStdHandle(0);
    // TODO: Create a new OverlappedIOHandle that relays a handle to process's stdin.
    wsl::windows::common::relay::InterruptableRelay(
        imageFileHandle.get(), loadProcessStdin.get(), m_sessionTerminatingEvent.get(), 4 * 1024 * 1024 /* 4MB buffer */);
    loadProcessStdin.reset();

    auto result = loadProcess.WaitAndCaptureOutput();

    RETURN_HR_IF_MSG(E_FAIL, result.Code != 0, "Load image failed: %hs", launcher.FormatResult(result).c_str());

    return S_OK;
}
CATCH_RETURN();

HRESULT WSLASession::ImportImage(ULONG ImageHandle, LPCSTR ImageName, IProgressCallback* ProgressCallback)
try
{
    UNREFERENCED_PARAMETER(ProgressCallback);

    wil::unique_handle imageFileHandle{wsl::windows::common::wslutil::DuplicateHandleFromCallingProcess(ULongToHandle(ImageHandle))};
    RETURN_HR_IF(E_INVALIDARG, INVALID_HANDLE_VALUE == imageFileHandle.get());
    RETURN_HR_IF_NULL(E_POINTER, ImageName);

    std::lock_guard lock{m_lock};

    ServiceProcessLauncher launcher{
        nerdctlPath, {nerdctlPath, "import", "-", ImageName}, {}, ProcessFlags::Stdin | ProcessFlags::Stdout | ProcessFlags::Stderr};
    auto importProcess = launcher.Launch(*m_virtualMachine.Get());

    auto importProcessStdin = importProcess.GetStdHandle(0);
    // TODO: Create a new OverlappedIOHandle that relays a handle to process's stdin.
    wsl::windows::common::relay::InterruptableRelay(
        imageFileHandle.get(), importProcessStdin.get(), m_sessionTerminatingEvent.get(), 4 * 1024 * 1024 /* 4MB buffer */);
    importProcessStdin.reset();

    auto result = importProcess.WaitAndCaptureOutput();

    RETURN_HR_IF_MSG(E_FAIL, result.Code != 0, "Import image failed: %hs", launcher.FormatResult(result).c_str());

    return S_OK;
}
CATCH_RETURN();

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
            *containerOptions, *m_virtualMachine.Get(), *m_eventTracker, std::bind(&WSLASession::OnContainerDeleted, this, std::placeholders::_1)));
    WI_ASSERT(inserted);

    container->second->Start(*containerOptions);

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

void WSLASession::OnContainerDeleted(const WSLAContainerImpl* Container)
{
    std::lock_guard lock{m_lock};
    WI_VERIFY(std::erase_if(m_containers, [Container](const auto& e) { return e.second.get() == Container; }) == 1);
}

std::string WSLASession::DockerRequest(const std::string& Url)
{
    wil::unique_socket socket;
    {
        std::lock_guard lock{m_lock};
        socket = m_virtualMachine->ConnectUnixSocket("/var/run/docker.sock");
    }

    namespace http = boost::beast::http;

    boost::asio::io_context context;

    boost::asio::generic::stream_protocol hv_proto(AF_HYPERV, SOCK_STREAM);

    boost::asio::generic::stream_protocol::stream_protocol::socket stream(context);
    stream.assign(hv_proto, socket.release());

    http::request<http::string_body> req{http::verb::get, Url, 11};
    req.set(http::field::host, "hvsocket"); // label only; AF_HYPERV doesn't do DNS
    req.prepare_payload();

    WSL_LOG("Written", TraceLoggingValue(http::write(stream, req), "Bytes"));

    boost::beast::flat_buffer buffer; // for header/body framing
    http::response<http::string_body> res;
    http::read(stream, buffer, res);

    WSL_LOG("HTTPREsult", TraceLoggingValue(res.result_int(), "Status"));

    return res.body();
}