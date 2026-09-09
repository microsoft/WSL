// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "OpenVmmWslCoreVm.h"

#include "Dmesg.h"
#include "VirtioFsShareRequest.h"
#include "WslCoreInstance.h"
#include "WslCoreVmDiskState.h"

#include <afunix.h>

using namespace wsl::windows::common;
using wsl::core::NetworkingMode;

namespace {
constexpr size_t c_bootEntropy = 0x1000;

wil::srwlock g_openVmmProcessLock;
std::map<GUID, wil::shared_handle, wsl::windows::common::helpers::GuidLess> g_openVmmProcesses;

void RegisterOpenVmmProcess(_In_ const GUID& VmId, _In_ HANDLE Process)
{
    wil::shared_handle process{wsl::windows::common::wslutil::DuplicateHandle(Process)};
    auto lock = g_openVmmProcessLock.lock_exclusive();
    const auto [entry, inserted] = g_openVmmProcesses.emplace(VmId, std::move(process));
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), !inserted);
}

wil::shared_handle GetOpenVmmProcess(_In_ const GUID& VmId)
{
    auto lock = g_openVmmProcessLock.lock_shared();
    const auto process = g_openVmmProcesses.find(VmId);
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), process == g_openVmmProcesses.end());
    return process->second;
}

wil::unique_socket ConnectToOpenVmmGuest(
    _In_ const std::filesystem::path& VsockPath,
    _In_ const GUID& VmId,
    _In_ ULONG Port,
    _In_opt_ HANDLE ExitHandle,
    _In_ DWORD Timeout)
{
    WSL_LOG(
        "OpenVmmConnectToGuest",
        TraceLoggingValue(Port, "Port"),
        TraceLoggingValue(VsockPath.c_str(), "BridgePath"),
        TraceLoggingValue(VmId, "VmId"));

    wil::unique_socket socket{::socket(AF_UNIX, SOCK_STREAM, 0)};
    THROW_WIN32_IF(static_cast<DWORD>(WSAGetLastError()), !socket);

    SOCKADDR_UN address{};
    address.sun_family = AF_UNIX;
    const auto narrowPath = wsl::shared::string::WideToMultiByte(VsockPath.wstring());
    THROW_HR_IF_MSG(E_INVALIDARG, narrowPath.size() >= sizeof(address.sun_path), "vsock bridge path too long: %hs", narrowPath.c_str());
    std::copy(narrowPath.cbegin(), narrowPath.cend(), address.sun_path);
    address.sun_path[narrowPath.size()] = '\0';

    THROW_WIN32_IF(
        static_cast<DWORD>(WSAGetLastError()),
        connect(socket.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR);

    const auto request = std::format("CONNECT {}\n", Port);
    wsl::windows::common::socket::Send(
        socket.get(), gsl::make_span(reinterpret_cast<const gsl::byte*>(request.data()), request.size()), ExitHandle);

    std::array<char, 64> response{};
    size_t responseLength = 0;
    for (; responseLength < response.size() - 1; ++responseLength)
    {
        const auto bytesRead = wsl::windows::common::socket::Receive(
            socket.get(),
            gsl::make_span(reinterpret_cast<gsl::byte*>(&response[responseLength]), 1),
            ExitHandle,
            MSG_WAITALL,
            Timeout);
        THROW_HR_IF_MSG(HRESULT_FROM_WIN32(ERROR_CONNECTION_ABORTED), bytesRead == 0, "vsock bridge closed during CONNECT handshake");

        if (response[responseLength] == '\n')
        {
            ++responseLength;
            break;
        }
    }

    THROW_HR_IF_MSG(E_FAIL, responseLength == response.size() - 1 && response[responseLength - 1] != '\n', "vsock bridge response too long");
    const std::string_view responseView{response.data(), responseLength};
    THROW_HR_IF_MSG(E_FAIL, responseView.find("OK ") != 0, "vsock bridge CONNECT failed: %hs", response.data());

    return socket;
}

LxssCreateProcess::ConnectToGuestCallback CreateOpenVmmConnector(
    _In_ const std::filesystem::path& VsockPath, _In_ const GUID& VmId, _In_ DWORD Timeout)
{
    return [VsockPath, VmId, Timeout](ULONG Port, HANDLE ExitHandle) {
        return ConnectToOpenVmmGuest(VsockPath, VmId, Port, ExitHandle, Timeout);
    };
}
}

OpenVmmWslCoreVm::OpenVmmWslCoreVm(
    _In_ const wil::shared_handle& UserToken,
    _In_ wsl::core::Config&& VmConfig,
    _In_ const GUID& VmId,
    _In_ InitializeDrvFsCallback InitializeDrvFs) :
    m_userToken(UserToken),
    m_vmConfig(std::move(VmConfig)),
    m_vmId(VmId),
    m_initializeDrvFs(std::move(InitializeDrvFs))
{
    m_processJobObject = wsl::windows::common::helpers::CreateKillOnCloseJob();
}

OpenVmmWslCoreVm::~OpenVmmWslCoreVm() noexcept
{
    WSL_LOG("OpenVmmTerminateVmStart", TraceLoggingValue(m_vmId, "VmId"));
    UnregisterProcess();

    {
        auto exitLock = m_exitCallbackLock.lock_exclusive();
        m_terminationCallback = {};
        m_terminatingEvent.SetEvent();
    }

    m_portTracker.reset();
    m_gnsSocket.reset();
    m_notifyChannel.reset();
    m_miniInitChannel.Close();

    if (m_distroExitThread.joinable())
    {
        m_distroExitThread.join();
    }

    if (m_virtioFsThread.joinable())
    {
        m_virtioFsThread.join();
    }

    if (m_vm)
    {
        if (m_processHandle && !m_vmExitEvent.wait(c_shutdownTimeoutMs))
        {
            LOG_IF_FAILED(WslOpenVmmVmTeardown(m_vm.get()));
        }

        LOG_IF_FAILED(WslOpenVmmVmQuit(m_vm.get()));
        m_vm.reset();
    }

    if (m_processHandle)
    {
        const auto waitResult = WaitForSingleObject(m_processHandle.get(), c_processTerminationTimeoutMs);
        if (waitResult == WAIT_TIMEOUT)
        {
            WSL_LOG("OpenVmmForceTerminate", TraceLoggingValue(m_vmId, "VmId"));
            LOG_LAST_ERROR_IF(!TerminateProcess(m_processHandle.get(), 1));
            LOG_LAST_ERROR_IF(WaitForSingleObject(m_processHandle.get(), c_processTerminationTimeoutMs) == WAIT_FAILED);
        }
        else
        {
            LOG_LAST_ERROR_IF(waitResult == WAIT_FAILED);
        }
    }

    if (m_processWait)
    {
        SetThreadpoolWait(m_processWait.get(), nullptr, nullptr);
        m_processWait.reset();
    }

    m_dmesgCollector.reset();
    m_listenSocket.reset();
    m_virtioFsListenSocket.reset();
    m_processHandle.reset();
    m_processJobObject.reset();

    DeleteFileW(m_listenPath.c_str());
    DeleteFileW(m_virtioFsListenPath.c_str());
    DeleteFileW(m_vsockPath.c_str());

    WSL_LOG("OpenVmmTerminateVmStop", TraceLoggingValue(m_vmId, "VmId"));
}

std::unique_ptr<OpenVmmWslCoreVm> OpenVmmWslCoreVm::Create(
    _In_ const wil::shared_handle& UserToken,
    _In_ wsl::core::Config&& VmConfig,
    _In_ const GUID& VmId,
    _In_ InitializeDrvFsCallback InitializeDrvFs)
{
    THROW_HR_IF(E_INVALIDARG, !InitializeDrvFs);

    auto newInstance = std::unique_ptr<OpenVmmWslCoreVm>{
        new OpenVmmWslCoreVm{UserToken, std::move(VmConfig), VmId, std::move(InitializeDrvFs)}};

    newInstance->Initialize();

    return newInstance;
}

void OpenVmmWslCoreVm::Initialize()
{
    auto signalEarlyTermination = wil::scope_exit([&] {
        m_terminatingEvent.SetEvent();
        m_vmExitEvent.SetEvent();

        UnregisterProcess();
        m_processWait.reset();
        m_portTracker.reset();
        m_gnsSocket.reset();
        m_notifyChannel.reset();
        m_miniInitChannel.Close();
        m_listenSocket.reset();
        m_vm.reset();
        m_processHandle.reset();
        m_processJobObject.reset();

        DeleteFileW(m_listenPath.c_str());
        DeleteFileW(m_vsockPath.c_str());
    });

    InitializeConfiguration();

    m_systemDistroDeviceId = ReserveLun();
    m_attachedDisks.emplace(
        m_systemDistroDeviceId, AttachedDisk{DiskType::VHD, m_vmConfig.SystemDistroPath, true, false, {}, {}});

    if (!m_vmConfig.KernelModulesPath.empty())
    {
        m_kernelModulesDeviceId = ReserveLun();
        m_attachedDisks.emplace(
            m_kernelModulesDeviceId, AttachedDisk{DiskType::VHD, m_vmConfig.KernelModulesPath, true, false, {}, {}});
    }

    std::tie(m_listenSocket, m_listenPath) = CreateVsockListener(LX_INIT_UTILITY_VM_INIT_PORT);
    LaunchOpenVmm();

    {
        SlowOperationWatcher slowOperation{"WaitForMiniInitConnect"};
        m_miniInitChannel =
            wsl::shared::SocketChannel{AcceptConnection(m_vmConfig.KernelBootTimeout), "mini_init", {m_terminatingEvent.get()}};
    }

    m_notifyChannel = AcceptConnection(m_vmConfig.KernelBootTimeout);
    ReadGuestCapabilities();
    InitializeGuest();

    signalEarlyTermination.release();
}

void OpenVmmWslCoreVm::InitializeConfiguration()
{
    m_restrictedToken = wsl::windows::common::security::CreateRestrictedToken(m_userToken.get());
    m_creatorElevated = wsl::windows::common::security::IsTokenElevated(m_userToken.get());

    const auto tokenUser = wil::get_token_information<TOKEN_USER>(m_userToken.get());
    THROW_IF_WIN32_BOOL_FALSE(::CopySid(sizeof(m_userSid), &m_userSid.Sid, tokenUser->User.Sid));

    m_machineId = wsl::shared::string::GuidToString<wchar_t>(m_vmId, wsl::shared::string::GuidToStringFlags::Uppercase);
    m_installPath = wsl::windows::common::wslutil::GetBasePath();
    m_rootFsPath = m_installPath / LXSS_TOOLS_DIRECTORY;
    m_initrdPath = m_rootFsPath / LXSS_VM_MODE_INITRD_NAME;
    m_userProfile = wsl::windows::common::helpers::GetUserProfilePath(m_userToken.get());

    m_defaultKernel = m_vmConfig.KernelPath.empty();
    if (m_defaultKernel)
    {
#ifdef WSL_KERNEL_PATH
        m_vmConfig.KernelPath = TEXT(WSL_KERNEL_PATH);
#else
        m_vmConfig.KernelPath = m_rootFsPath / LXSS_VM_MODE_KERNEL_NAME;
#endif
    }
    else if (!wsl::windows::common::filesystem::FileExists(m_vmConfig.KernelPath.c_str()))
    {
        THROW_HR_WITH_USER_ERROR(
            WSL_E_CUSTOM_KERNEL_NOT_FOUND,
            wsl::shared::Localization::MessageCustomKernelNotFound(
                wsl::windows::common::helpers::GetWslConfigPath(m_userToken.get()), m_vmConfig.KernelPath.c_str()));
    }

    const bool privateKernelModules = !m_vmConfig.KernelModulesPath.empty();
    if (m_vmConfig.KernelModulesPath.empty())
    {
        if (m_defaultKernel)
        {
#ifdef WSL_KERNEL_MODULES_PATH
            m_vmConfig.KernelModulesPath = TEXT(WSL_KERNEL_MODULES_PATH);
#else
            m_vmConfig.KernelModulesPath = m_rootFsPath / L"artifacts.vhd";
#endif
        }
    }
    else if (!wsl::windows::common::filesystem::FileExists(m_vmConfig.KernelModulesPath.c_str()))
    {
        THROW_HR_WITH_USER_ERROR(
            WSL_E_CUSTOM_KERNEL_NOT_FOUND,
            wsl::shared::Localization::MessageCustomKernelModulesNotFound(
                wsl::windows::common::helpers::GetWslConfigPath(m_userToken.get()), m_vmConfig.KernelModulesPath.c_str()));
    }

    if (m_defaultKernel && privateKernelModules)
    {
        THROW_HR_WITH_USER_ERROR(WSL_E_CUSTOM_KERNEL_NOT_FOUND, wsl::shared::Localization::MessageMismatchedKernelModulesError());
    }

    if (m_vmConfig.SystemDistroPath.empty())
    {
#ifdef WSL_SYSTEM_DISTRO_PATH
        m_vmConfig.SystemDistroPath = TEXT(WSL_SYSTEM_DISTRO_PATH);
#else
        m_vmConfig.SystemDistroPath = m_installPath / L"system.vhd";
#endif
    }

    THROW_HR_IF(
        WSL_E_CUSTOM_SYSTEM_DISTRO_ERROR,
        !wsl::windows::common::string::IsPathComponentEqual(m_vmConfig.SystemDistroPath.extension().native(), L".vhd") ||
            !wsl::windows::common::filesystem::FileExists(m_vmConfig.SystemDistroPath.c_str()));

    m_openVmmPath = m_installPath / L"openvmm.exe";
    THROW_HR_IF_MSG(
        HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND),
        !wsl::windows::common::filesystem::FileExists(m_openVmmPath.c_str()),
        "openvmm.exe not found at: %ls",
        m_openVmmPath.c_str());

    m_vmConfig.EnableDebugShell = false;
    m_vmConfig.EnableDnsTunneling = false;
    m_vmConfig.EnableGpuSupport = false;
    m_vmConfig.EnableGuiApps = false;
    m_vmConfig.EnableVirtio = true;
    m_vmConfig.EnableVirtio9p = false;
    m_vmConfig.EnableVirtioFs = m_vmConfig.EnableHostFileSystemAccess;
    m_vmConfig.MaxCrashDumpCount = -1;
    m_vmConfig.NetworkingMode = NetworkingMode::Consomme;

    constexpr UINT64 c_maxMemorySizeBytes = 4ULL * _1GB;
    m_vmConfig.MemorySizeBytes = (std::min(m_vmConfig.MemorySizeBytes, c_maxMemorySizeBytes) / (2 * _1MB)) * (2 * _1MB);
    if (m_vmConfig.SwiotlbSizeBytes == 0)
    {
        m_vmConfig.SwiotlbSizeBytes =
            wsl::windows::common::helpers::ComputeDefaultSwiotlbConfig(m_vmConfig.MemorySizeBytes);
    }

    const auto vmId = wsl::shared::string::GuidToString<wchar_t>(m_vmId, wsl::shared::string::GuidToStringFlags::None);
    const auto socketDirectory = wsl::windows::common::filesystem::GetTempFolderPath(m_userToken.get());
    {
        const auto runAsUser = wil::impersonate_token(m_userToken.get());
        wil::CreateDirectoryDeep(socketDirectory.c_str());
    }

    const auto shortId = vmId.substr(0, 8);
    m_rpcPipeName = wsl::windows::common::helpers::GetUniquePipeName();
    m_vsockPath = socketDirectory / std::format(L"wsl-{}.v", shortId);
    DeleteFileW(m_vsockPath.c_str());

    if (m_vmConfig.EnableDebugConsole || !m_vmConfig.DebugConsoleLogFile.empty())
    {
        m_vmConfig.EnableDebugConsole = true;
        m_comPipe0 = wsl::windows::common::helpers::GetUniquePipeName();
    }

    try
    {
        const bool enableTelemetry = TraceLoggingProviderEnabled(g_hTraceLoggingProvider, WINEVENT_LEVEL_INFO, 0);
        const auto userSid = wsl::windows::common::wslutil::SidToString(&m_userSid.Sid);
        const auto pipeSddl = std::format(L"D:P(A;;GA;;;SY)(A;;GA;;;{})", userSid.get());
        wil::unique_hlocal_security_descriptor pipeSecurityDescriptor;
        THROW_IF_WIN32_BOOL_FALSE(ConvertStringSecurityDescriptorToSecurityDescriptorW(
            pipeSddl.c_str(), SDDL_REVISION_1, &pipeSecurityDescriptor, nullptr));
        m_dmesgCollector = DmesgCollector::Create(
            m_vmId,
            m_vmExitEvent.get(),
            enableTelemetry,
            m_vmConfig.EnableDebugConsole,
            m_comPipe0,
            m_vmConfig.EnableEarlyBootLogging,
            {},
            pipeSecurityDescriptor.get());
    }
    CATCH_LOG()

    if (m_vmConfig.EnableDebugConsole)
    {
        try
        {
            wil::unique_hfile logFile;
            if (!m_vmConfig.DebugConsoleLogFile.empty())
            {
                const auto runAsUser = wil::impersonate_token(m_userToken.get());
                logFile.reset(CreateFileW(
                    m_vmConfig.DebugConsoleLogFile.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, 0, nullptr));
                LOG_LAST_ERROR_IF(!logFile);
            }

            wsl::windows::common::helpers::LaunchDebugConsole(
                m_comPipe0.c_str(),
                !!m_dmesgCollector,
                m_restrictedToken.get(),
                logFile ? logFile.get() : nullptr,
                !m_vmConfig.EnableTelemetry,
                m_processJobObject.get());
        }
        CATCH_LOG()
    }
}

std::pair<wil::unique_socket, std::filesystem::path> OpenVmmWslCoreVm::CreateVsockListener(_In_ ULONG Port) const
{
    const auto runAsUser = wil::impersonate_token(m_userToken.get());
    auto listenPath = std::filesystem::path{
        std::format(L"{}_{:08x}-facb-11e6-bd58-64006a7986d3", m_vsockPath.wstring(), Port)};
    DeleteFileW(listenPath.c_str());

    wil::unique_socket listenSocket{::socket(AF_UNIX, SOCK_STREAM, 0)};
    THROW_WIN32_IF(static_cast<DWORD>(WSAGetLastError()), !listenSocket);

    SOCKADDR_UN address{};
    address.sun_family = AF_UNIX;
    const auto narrowPath = wsl::shared::string::WideToMultiByte(listenPath.wstring());
    THROW_HR_IF_MSG(E_INVALIDARG, narrowPath.size() >= sizeof(address.sun_path), "vsock bridge path too long: %hs", narrowPath.c_str());
    std::copy(narrowPath.cbegin(), narrowPath.cend(), address.sun_path);
    address.sun_path[narrowPath.size()] = '\0';

    THROW_WIN32_IF(
        static_cast<DWORD>(WSAGetLastError()),
        bind(listenSocket.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR);
    THROW_WIN32_IF(static_cast<DWORD>(WSAGetLastError()), listen(listenSocket.get(), SOMAXCONN) == SOCKET_ERROR);

    return {std::move(listenSocket), std::move(listenPath)};
}

std::wstring OpenVmmWslCoreVm::BuildCommandLine() const
{
    return std::format(L"\"{}\" --rpc \"path={},transport=grpc,allow-sid=S-1-5-18\"", m_openVmmPath.wstring(), m_rpcPipeName);
}

std::wstring OpenVmmWslCoreVm::BuildKernelCommandLine() const
{
    std::wstring commandLine = L"initrd=\\" LXSS_VM_MODE_INITRD_NAME L" " TEXT(WSL_ROOT_INIT_ENV) L"=1 panic=-1";
    wsl::windows::common::helpers::AppendCommonKernelCommandLine(
        commandLine, c_pageReportingOrder, m_vmConfig.SwiotlbSizeBytes, m_vmConfig.ProcessorCount);

    if (m_dmesgCollector)
    {
        if (m_vmConfig.EnableEarlyBootLogging)
        {
            if constexpr (wsl::shared::Arm64)
            {
                commandLine += L" earlycon=pl011,0xeffec000,115200";
            }
            else
            {
                commandLine += L" earlycon=uart8250,io,0x3f8,115200";
            }
        }

        commandLine += L" console=hvc0 debug";
    }
    else if (m_vmConfig.EnableDebugConsole)
    {
        commandLine += wsl::shared::Arm64 ? L" console=ttyAMA0 debug" : L" console=ttyS0,115200 debug";
    }

    commandLine += L" pty.legacy_count=0";
    if (!m_vmConfig.KernelCommandLine.empty())
    {
        commandLine += L" ";
        commandLine += m_vmConfig.KernelCommandLine;
    }

    return commandLine;
}

void OpenVmmWslCoreVm::ConfigureVm(_In_ WslOpenVmmConfig* Config) const
{
    THROW_IF_FAILED(WslOpenVmmConfigSetKernelPath(Config, m_vmConfig.KernelPath.c_str()));
    THROW_IF_FAILED(WslOpenVmmConfigSetInitrdPath(Config, m_initrdPath.c_str()));
    const auto commandLine = BuildKernelCommandLine();
    THROW_IF_FAILED(WslOpenVmmConfigSetKernelCmdLine(Config, commandLine.c_str()));
    THROW_IF_FAILED(WslOpenVmmConfigSetMemoryMb(Config, m_vmConfig.MemorySizeBytes / _1MB));
    THROW_IF_FAILED(WslOpenVmmConfigSetProcessorCount(Config, gsl::narrow_cast<UINT32>(m_vmConfig.ProcessorCount)));
    THROW_IF_FAILED(WslOpenVmmConfigSetHvSocketPath(Config, m_vsockPath.c_str()));

    for (const auto& [lun, disk] : m_attachedDisks)
    {
        THROW_IF_FAILED(WslOpenVmmConfigAddBootDisk(Config, 0, lun, disk.Path.c_str(), disk.ReadOnly));
    }

    GUID nicGuid = m_vmId;
    nicGuid.Data1 ^= c_nicGuidXorMask;
    const auto nicId = wsl::shared::string::GuidToString<wchar_t>(nicGuid, wsl::shared::string::GuidToStringFlags::None);
    const auto macAddress = std::ranges::all_of(m_vmConfig.MacAddress, [](const auto octet) { return octet == 0; })
                                ? std::wstring{c_defaultConsommeMacAddress}
                                : wsl::shared::string::FormatMacAddress<wchar_t>(m_vmConfig.MacAddress, L'-');
    THROW_IF_FAILED(WslOpenVmmConfigSetConsommeNic(Config, nicId.c_str(), macAddress.c_str()));

    if (m_dmesgCollector)
    {
        if (const auto earlyConsole = m_dmesgCollector->EarlyConsoleName(); !earlyConsole.empty())
        {
            THROW_IF_FAILED(WslOpenVmmConfigAddSerialPort(Config, 0, earlyConsole.c_str()));
        }

        const auto console = m_dmesgCollector->VirtioConsoleName();
        THROW_IF_FAILED(WslOpenVmmConfigSetVirtioConsolePath(Config, console.c_str()));
    }
    else if (!m_comPipe0.empty())
    {
        THROW_IF_FAILED(WslOpenVmmConfigAddSerialPort(Config, 0, m_comPipe0.c_str()));
    }
}

void OpenVmmWslCoreVm::LaunchOpenVmm()
{
    wil::unique_any<WslOpenVmmConfig*, decltype(&WslOpenVmmDestroyConfig), WslOpenVmmDestroyConfig> config;
    THROW_IF_FAILED(WslOpenVmmCreateConfig(config.put()));
    ConfigureVm(config.get());

    const auto commandLine = BuildCommandLine();
    WSL_LOG("LaunchOpenVmm", TraceLoggingValue(commandLine.c_str(), "CommandLine"));

    wil::unique_handle primaryToken;
    THROW_IF_WIN32_BOOL_FALSE(DuplicateTokenEx(
        m_userToken.get(), MAXIMUM_ALLOWED, nullptr, SecurityImpersonation, TokenPrimary, &primaryToken));

    wsl::windows::common::helpers::unique_environment_block environment{nullptr};
    THROW_LAST_ERROR_IF(!CreateEnvironmentBlock(&environment, primaryToken.get(), false));

    SubProcess process{m_openVmmPath.c_str(), commandLine.c_str()};
    process.SetFlags(CREATE_NO_WINDOW);
    process.SetToken(primaryToken.get());
    process.SetEnvironment(environment.get());
    process.SetJobObject(m_processJobObject.get());

    SECURITY_ATTRIBUTES securityAttributes{sizeof(securityAttributes), nullptr, TRUE};
    const auto logPath = m_vsockPath.wstring() + L".log";
    wil::unique_hfile logFile;
    {
        const auto runAsUser = wil::impersonate_token(m_userToken.get());
        logFile.reset(CreateFileW(
            logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &securityAttributes, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    }
    THROW_LAST_ERROR_IF(!logFile);

    wil::unique_hfile errorLogFile;
    THROW_IF_WIN32_BOOL_FALSE(DuplicateHandle(
        GetCurrentProcess(), logFile.get(), GetCurrentProcess(), errorLogFile.put(), 0, TRUE, DUPLICATE_SAME_ACCESS));
    process.SetStdHandles(nullptr, logFile.get(), errorLogFile.get());

    m_processHandle = process.Start();
    RegisterOpenVmmProcess(m_vmId, m_processHandle.get());
    m_processRegistered = true;
    m_processWait.reset(CreateThreadpoolWait(&OpenVmmWslCoreVm::OnProcessExit, this, nullptr));
    THROW_LAST_ERROR_IF(!m_processWait);
    SetThreadpoolWait(m_processWait.get(), m_processHandle.get(), nullptr);
    THROW_IF_FAILED_MSG(
        WslOpenVmmCreateVm(config.addressof(), m_rpcPipeName.c_str(), m_vmConfig.KernelBootTimeout, m_vm.put()),
        "Failed to create OpenVMM VM");
    THROW_IF_FAILED_MSG(WslOpenVmmVmResume(m_vm.get()), "Failed to resume OpenVMM VM");
}

wil::unique_socket OpenVmmWslCoreVm::AcceptConnection(_In_ DWORD ReceiveTimeout) const
{
    return AcceptConnection(m_listenSocket.get(), m_vmConfig.KernelBootTimeout, ReceiveTimeout);
}

wil::unique_socket OpenVmmWslCoreVm::AcceptConnection(
    _In_ SOCKET ListenSocket, _In_ DWORD AcceptTimeout, _In_ DWORD ReceiveTimeout) const
{
    wil::unique_event acceptEvent{wil::EventOptions::ManualReset};
    THROW_WIN32_IF(
        static_cast<DWORD>(WSAGetLastError()),
        WSAEventSelect(ListenSocket, acceptEvent.get(), FD_ACCEPT) == SOCKET_ERROR);

    auto restoreListener = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&] {
        if (WSAEventSelect(ListenSocket, nullptr, 0) == SOCKET_ERROR)
        {
            LOG_HR_MSG(HRESULT_FROM_WIN32(WSAGetLastError()), "Failed to clear the OpenVMM listener event");
        }

        u_long blocking = 0;
        if (ioctlsocket(ListenSocket, FIONBIO, &blocking) == SOCKET_ERROR)
        {
            LOG_HR_MSG(HRESULT_FROM_WIN32(WSAGetLastError()), "Failed to restore blocking mode on the OpenVMM listener");
        }
    });

    const HANDLE waitHandles[]{acceptEvent.get(), m_terminatingEvent.get()};
    const auto waitResult = WaitForMultipleObjects(ARRAYSIZE(waitHandles), waitHandles, FALSE, AcceptTimeout);
    THROW_HR_IF(E_ABORT, waitResult == WAIT_OBJECT_0 + 1);
    THROW_HR_IF(HCS_E_CONNECTION_TIMEOUT, waitResult == WAIT_TIMEOUT);
    THROW_LAST_ERROR_IF(waitResult == WAIT_FAILED);
    THROW_HR_IF(E_UNEXPECTED, waitResult != WAIT_OBJECT_0);

    WSANETWORKEVENTS networkEvents{};
    THROW_WIN32_IF(
        static_cast<DWORD>(WSAGetLastError()),
        WSAEnumNetworkEvents(ListenSocket, acceptEvent.get(), &networkEvents) == SOCKET_ERROR);
    THROW_WIN32_IF(
        static_cast<DWORD>(networkEvents.iErrorCode[FD_ACCEPT_BIT]),
        networkEvents.iErrorCode[FD_ACCEPT_BIT] != 0);

    wil::unique_socket socket{accept(ListenSocket, nullptr, nullptr)};
    THROW_WIN32_IF(static_cast<DWORD>(WSAGetLastError()), !socket);

    THROW_WIN32_IF(
        static_cast<DWORD>(WSAGetLastError()),
        WSAEventSelect(socket.get(), nullptr, 0) == SOCKET_ERROR);

    u_long blocking = 0;
    THROW_WIN32_IF(
        static_cast<DWORD>(WSAGetLastError()),
        ioctlsocket(socket.get(), FIONBIO, &blocking) == SOCKET_ERROR);

    if (ReceiveTimeout != 0)
    {
        THROW_WIN32_IF(
            static_cast<DWORD>(WSAGetLastError()),
            setsockopt(socket.get(), SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ReceiveTimeout), sizeof(ReceiveTimeout)) == SOCKET_ERROR);
    }

    return socket;
}

_Requires_lock_held_(m_guestDeviceLock)
std::tuple<std::wstring, std::wstring, std::wstring> OpenVmmWslCoreVm::AddVirtioFsShare(
    _In_ bool Admin, _In_ const std::wstring& Path, _In_ const std::wstring& Options)
{
    THROW_HR_IF(E_ACCESSDENIED, Admin != m_creatorElevated);

    std::filesystem::path sharePath{Path};
    if (!sharePath.native().ends_with(L'\\') && !sharePath.native().ends_with(L'/'))
    {
        sharePath += L'\\';
    }

    {
        const auto runAsUser = wil::impersonate_token(m_userToken.get());
        sharePath = wsl::windows::common::filesystem::GetCanonicalPath(sharePath);
    }

    const auto existing = std::ranges::find_if(m_virtioFsShares, [&](const auto& Share) {
        return Share.Admin == Admin && Share.Options == Options &&
               wsl::windows::common::string::IsPathComponentEqual(Share.Path.native(), sharePath.native());
    });
    if (existing != m_virtioFsShares.end())
    {
        return {existing->Tag, {}, existing->Path.wstring()};
    }

    bool readOnly = false;
    for (const auto option : Options | std::views::split(L','))
    {
        if (std::ranges::equal(option, std::wstring_view{L"ro"}))
        {
            readOnly = true;
        }
        else if (std::ranges::equal(option, std::wstring_view{L"rw"}))
        {
            readOnly = false;
        }
    }

    GUID tagGuid{};
    THROW_IF_FAILED(CoCreateGuid(&tagGuid));
    const auto tag = wsl::shared::string::GuidToString<wchar_t>(tagGuid, wsl::shared::string::GuidToStringFlags::None);
    THROW_IF_FAILED(WslOpenVmmVmAddShare(m_vm.get(), tag.c_str(), sharePath.c_str(), readOnly));
    auto removeOnFailure =
        wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&] { LOG_IF_FAILED(WslOpenVmmVmRemoveShare(m_vm.get(), tag.c_str())); });

    m_virtioFsShares.emplace_back(VirtioFsShare{std::move(sharePath), Options, Admin, tag});
    removeOnFailure.release();

    const auto& share = m_virtioFsShares.back();
    WSL_LOG(
        "OpenVmmAddVirtioFsShare",
        TraceLoggingValue(share.Path.c_str(), "Path"),
        TraceLoggingValue(share.Options.c_str(), "Options"),
        TraceLoggingValue(share.Admin, "Admin"),
        TraceLoggingValue(share.Tag.c_str(), "Tag"));
    return {share.Tag, {}, share.Path.wstring()};
}

std::vector<char> OpenVmmWslCoreVm::ProcessVirtioFsRequest(_In_ gsl::span<gsl::byte> Request)
{
    return wsl::windows::service::ProcessVirtioFsShareRequest(
        Request,
        [this](bool Admin, const std::wstring& Path, const std::wstring& Options) {
            auto lock = m_guestDeviceLock.lock_exclusive();
            auto [tag, childName, source] = AddVirtioFsShare(Admin, Path, Options);
            return wsl::windows::service::VirtioFsShareResult{
                std::move(tag), std::move(childName), std::move(source)};
        },
        [this](const std::wstring& Tag, bool Admin) {
            auto lock = m_guestDeviceLock.lock_exclusive();
            const auto share = std::ranges::find_if(m_virtioFsShares, [&](const auto& Share) {
                return wsl::shared::string::IsEqual(Share.Tag, Tag, false);
            });
            THROW_HR_IF_MSG(E_UNEXPECTED, share == m_virtioFsShares.end(), "Unknown tag %ls", Tag.c_str());

            const auto path = share->Path.wstring();
            const auto options = share->Options;
            auto [newTag, childName, source] = AddVirtioFsShare(Admin, path, options);
            return wsl::windows::service::VirtioFsShareResult{
                std::move(newTag), std::move(childName), std::move(source)};
        });
}

void OpenVmmWslCoreVm::VirtioFsWorker() noexcept
{
    wsl::windows::common::wslutil::SetThreadDescription(L"OpenVMM VirtioFs");

    while (!m_terminatingEvent.is_signaled())
    {
        try
        {
            auto socket = AcceptConnection(
                m_virtioFsListenSocket.get(), INFINITE, m_vmConfig.KernelBootTimeout);
            std::vector<gsl::byte> buffer;
            const auto request = wsl::shared::socket::RecvMessage(
                socket.get(), buffer, m_terminatingEvent.get(), m_vmConfig.KernelBootTimeout);
            if (request.empty())
            {
                continue;
            }

            const auto response = ProcessVirtioFsRequest(request);
            wsl::windows::common::socket::Send(
                socket.get(),
                gsl::make_span(
                    reinterpret_cast<const gsl::byte*>(response.data()), response.size()),
                m_terminatingEvent.get());
        }
        catch (...)
        {
            if (!m_terminatingEvent.is_signaled())
            {
                LOG_CAUGHT_EXCEPTION();
            }
        }
    }
}

void OpenVmmWslCoreVm::ReadGuestCapabilities()
{
    const auto& info = m_miniInitChannel.ReceiveMessage<LX_INIT_GUEST_CAPABILITIES>();
    m_kernelVersionString = wsl::shared::string::MultiByteToWide(info.Buffer);

    const std::regex pattern{"(\\d+)\\.(\\d+)\\.(\\d+).*"};
    std::smatch match;
    const std::string input = info.Buffer;
    THROW_HR_IF_MSG(
        E_UNEXPECTED,
        !std::regex_match(input, match, pattern) || match.size() != 4,
        "Failed to parse kernel version: '%hs'",
        input.c_str());

    try
    {
        m_kernelVersion = {std::stoul(match.str(1)), std::stoul(match.str(2)), std::stoul(match.str(3))};
    }
    catch (const std::exception& exception)
    {
        THROW_HR_MSG(E_UNEXPECTED, "Failed to parse kernel version: '%hs', %hs", info.Buffer, exception.what());
    }

    m_seccompAvailable = info.SeccompAvailable;
    m_hvPciSwiotlbBase = info.HvPciSwiotlbBase;
    m_hvPciSwiotlbSize = info.HvPciSwiotlbSize;
    WSL_LOG(
        "GuestKernelInfo",
        TraceLoggingValue(m_seccompAvailable, "SeccompAvailable"),
        TraceLoggingValue(m_hvPciSwiotlbBase, "HvPciSwiotlbBase"),
        TraceLoggingValue(m_hvPciSwiotlbSize, "HvPciSwiotlbSize"),
        TraceLoggingValue(std::get<0>(m_kernelVersion), "Version"),
        TraceLoggingValue(std::get<1>(m_kernelVersion), "Revision"),
        TraceLoggingValue(std::get<2>(m_kernelVersion), "Minor"));
}

void OpenVmmWslCoreVm::InitializeGuest()
{
    wsl::shared::MessageWriter<LX_MINI_INIT_EARLY_CONFIG_MESSAGE> earlyConfig{LxMiniInitMessageEarlyConfig};
    earlyConfig->SwapLun = ULONG_MAX;
    earlyConfig->SystemDistroDeviceType = LxMiniInitMountDeviceTypeLun;
    earlyConfig->SystemDistroDeviceId = m_systemDistroDeviceId;
    earlyConfig->MemoryReclaimMode = static_cast<LX_MINI_INIT_MEMORY_RECLAIM_MODE>(m_vmConfig.MemoryReclaim);
    earlyConfig->EnableDebugShell = false;
    earlyConfig->EnableSafeMode = m_vmConfig.EnableSafeMode;
    earlyConfig->EnableDnsTunneling = false;
    earlyConfig->DefaultKernel = m_defaultKernel;
    earlyConfig->IsolateDistroCgroup = m_vmConfig.IsolateDistroCgroup;
    earlyConfig->KernelModulesDeviceId = m_kernelModulesDeviceId;
    earlyConfig.WriteString(earlyConfig->HostnameOffset, wsl::windows::common::filesystem::GetLinuxHostName());
    earlyConfig.WriteString(earlyConfig->KernelModulesListOffset, m_vmConfig.KernelModulesList);

    auto earlyConfigTransaction = m_miniInitChannel.StartTransaction();
    earlyConfigTransaction.Send<LX_MINI_INIT_EARLY_CONFIG_MESSAGE>(earlyConfig.Span());

    m_gnsSocket = AcceptConnection(m_vmConfig.KernelBootTimeout);

    wsl::shared::MessageWriter<LX_MINI_INIT_CONFIG_MESSAGE> config{LxMiniInitMessageInitialConfig};
    config->EntropySize = c_bootEntropy;
    config->EnableGuiApps = false;
    config->MountGpuShares = false;
    config->NetworkingConfiguration.NetworkingMode = LxMiniInitNetworkingModeConsomme;
    config->NetworkingConfiguration.DisableIpv6 = false;
    config->NetworkingConfiguration.EnableDhcpClient = true;
    config->NetworkingConfiguration.DhcpTimeout = m_vmConfig.DhcpTimeout;
    config->NetworkingConfiguration.PortTrackerType =
        m_vmConfig.EnableLocalhostRelay ? LxMiniInitPortTrackerTypeMirrored : LxMiniInitPortTrackerTypeNone;

    THROW_IF_NTSTATUS_FAILED(BCryptGenRandom(
        nullptr,
        reinterpret_cast<PUCHAR>(config.InsertBuffer(config->EntropyOffset, config->EntropySize).data()),
        config->EntropySize,
        BCRYPT_USE_SYSTEM_PREFERRED_RNG));

    auto configTransaction = m_miniInitChannel.StartTransaction();
    configTransaction.Send<LX_MINI_INIT_CONFIG_MESSAGE>(config.Span());

    if (m_vmConfig.EnableLocalhostRelay)
    {
        m_portTracker.emplace(
            AcceptConnection(m_vmConfig.KernelBootTimeout),
            [this](const SOCKADDR_INET& address, int protocol, bool allocate) -> HRESULT {
                RETURN_HR_IF(E_INVALIDARG, address.si_family != AF_INET && address.si_family != AF_INET6);
                RETURN_HR_IF(E_INVALIDARG, protocol != IPPROTO_TCP && protocol != IPPROTO_UDP);
                const auto* ipAddress = address.si_family == AF_INET ? reinterpret_cast<const void*>(&address.Ipv4.sin_addr)
                                                                     : reinterpret_cast<const void*>(&address.Ipv6.sin6_addr);
                if (!INET_IS_ADDR_UNSPECIFIED(address.si_family, ipAddress) && !INET_IS_ADDR_LOOPBACK(address.si_family, ipAddress))
                {
                    return S_OK;
                }

                if (address.si_family == AF_INET && INET_IS_ADDR_LOOPBACK(AF_INET, ipAddress) &&
                    address.Ipv4.sin_addr.s_addr != htonl(INADDR_LOOPBACK))
                {
                    return S_OK;
                }

                const auto port = INETADDR_PORT(reinterpret_cast<const SOCKADDR*>(&address));
                return allocate ? WslOpenVmmVmBindPort(m_vm.get(), port, port, protocol == IPPROTO_TCP, address.si_family)
                                : WslOpenVmmVmUnbindPort(m_vm.get(), port, port, protocol == IPPROTO_TCP, address.si_family);
            },
            [](const std::string&, bool) {});
    }
}

std::string OpenVmmWslCoreVm::GetMountTargetName(_In_ PCWSTR Disk, _In_opt_ PCWSTR Name, _In_ int PartitionIndex)
{
    if (ARGUMENT_PRESENT(Name))
    {
        auto mountName = wsl::shared::string::WideToMultiByte(Name);
        THROW_HR_IF(
            WSL_E_VM_MODE_INVALID_MOUNT_NAME,
            mountName.empty() || mountName == "." || mountName == ".." || mountName.find('/') != std::string::npos);
        return mountName;
    }

    std::string target;
    const auto diskName = wsl::shared::string::WideToMultiByte(Disk);
    std::copy_if(diskName.begin(), diskName.end(), std::back_inserter(target), &isalnum);
    if (PartitionIndex != 0)
    {
        target += std::format("p{}", PartitionIndex);
    }

    return target;
}

std::pair<int, LX_MINI_MOUNT_STEP> OpenVmmWslCoreVm::GetMountResult(_In_ wsl::shared::SocketChannel& Channel)
{
    const auto& message = Channel.ReceiveMessage<LX_MINI_INIT_MOUNT_RESULT_MESSAGE>();
    return {message.Result, message.FailureStep};
}

_Requires_lock_held_(m_lock)
void OpenVmmWslCoreVm::FreeLun(_In_ ULONG Lun)
{
    THROW_HR_IF(E_BOUNDS, Lun >= m_lunBitmap.size());
    THROW_HR_IF(E_INVALIDARG, !m_lunBitmap[Lun]);
    m_lunBitmap[Lun] = false;
}

_Requires_lock_held_(m_lock)
ULONG OpenVmmWslCoreVm::ReserveLun(_In_ std::optional<ULONG> Lun)
{
    THROW_HR_IF(E_BOUNDS, Lun.has_value() && Lun.value() >= m_lunBitmap.size());
    if (Lun.has_value() && !m_lunBitmap[Lun.value()])
    {
        m_lunBitmap[Lun.value()] = true;
        return Lun.value();
    }

    for (ULONG index = 0; index < gsl::narrow_cast<ULONG>(m_lunBitmap.size()); ++index)
    {
        if (!m_lunBitmap[index])
        {
            m_lunBitmap[index] = true;
            return index;
        }
    }

    THROW_HR(WSL_E_TOO_MANY_DISKS_ATTACHED);
}

_Requires_lock_held_(m_lock)
std::pair<int, LX_MINI_MOUNT_STEP> OpenVmmWslCoreVm::UnmountDisk(_In_ ULONG Lun, _Inout_ AttachedDisk& Disk)
{
    for (auto mount = Disk.Mounts.begin(); mount != Disk.Mounts.end();)
    {
        const auto result = UnmountVolume(mount->second.Name.c_str());
        if (result.first != 0)
        {
            return result;
        }

        mount = Disk.Mounts.erase(mount);
    }

    LX_MINI_INIT_DETACH_MESSAGE message{};
    message.Header.MessageType = LxMiniInitMessageDetach;
    message.Header.MessageSize = sizeof(message);
    message.ScsiLun = Lun;

    auto transaction = m_miniInitChannel.StartTransaction();
    transaction.Send(message);

    wsl::shared::SocketChannel resultChannel{
        AcceptConnection(m_vmConfig.KernelBootTimeout), "MountResult", {m_terminatingEvent.get()}};
    return GetMountResult(resultChannel);
}

std::pair<int, LX_MINI_MOUNT_STEP> OpenVmmWslCoreVm::UnmountVolume(_In_ PCWSTR Name)
{
    wsl::shared::MessageWriter<LX_MINI_INIT_UNMOUNT_MESSAGE> message{LxMiniInitMessageUnmount};
    message.WriteString(Name);

    auto transaction = m_miniInitChannel.StartTransaction();
    transaction.Send<LX_MINI_INIT_UNMOUNT_MESSAGE>(message.Span());

    wsl::shared::SocketChannel resultChannel{
        AcceptConnection(m_vmConfig.KernelBootTimeout), "MountResult", {m_terminatingEvent.get()}};
    return GetMountResult(resultChannel);
}

void OpenVmmWslCoreVm::UnregisterProcess() noexcept
{
    if (m_processRegistered)
    {
        auto lock = g_openVmmProcessLock.lock_exclusive();
        g_openVmmProcesses.erase(m_vmId);
        m_processRegistered = false;
    }
}

void CALLBACK OpenVmmWslCoreVm::OnProcessExit(
    _Inout_ PTP_CALLBACK_INSTANCE, _In_opt_ void* Context, _Inout_ PTP_WAIT, _In_ TP_WAIT_RESULT) noexcept
{
    auto* vm = static_cast<OpenVmmWslCoreVm*>(Context);
    DWORD exitCode{};
    LOG_IF_WIN32_BOOL_FALSE(GetExitCodeProcess(vm->m_processHandle.get(), &exitCode));

    WSL_LOG(
        "OpenVmmProcessExited",
        TraceLoggingValue(exitCode, "ExitCode"),
        TraceLoggingValue(vm->m_vmId, "VmId"));

    std::function<void(GUID)> terminationCallback;
    {
        auto exitLock = vm->m_exitCallbackLock.lock_exclusive();
        vm->m_terminatingEvent.SetEvent();
        vm->m_vmExitEvent.SetEvent();
        terminationCallback = std::move(vm->m_terminationCallback);
    }

    if (terminationCallback)
    {
        try
        {
            terminationCallback(vm->m_vmId);
        }
        CATCH_LOG()
    }
}

void OpenVmmWslCoreVm::ForceTerminate(_In_ const GUID& VmId)
{
    const auto process = GetOpenVmmProcess(VmId);
    const auto waitResult = WaitForSingleObject(process.get(), 0);
    if (waitResult == WAIT_OBJECT_0)
    {
        return;
    }

    THROW_LAST_ERROR_IF(waitResult == WAIT_FAILED);
    THROW_IF_WIN32_BOOL_FALSE(TerminateProcess(process.get(), 1));
}

ULONG OpenVmmWslCoreVm::AttachDisk(
    _In_ PCWSTR Disk, _In_ DiskType Type, _In_ std::optional<ULONG> Lun, _In_ bool IsUserDisk, _In_ HANDLE)
{
    auto lock = m_lock.lock_exclusive();
    return AttachDiskLockHeld(Disk, Type, Lun, IsUserDisk, false);
}

_Requires_lock_held_(m_lock)
ULONG OpenVmmWslCoreVm::AttachDiskLockHeld(_In_ PCWSTR Disk, _In_ DiskType Type, _In_ std::optional<ULONG> Lun, _In_ bool IsUserDisk, _In_ bool ReadOnly)
{
    ExecutionContext context{Context::MountDisk};
    THROW_HR_IF(E_INVALIDARG, !ARGUMENT_PRESENT(Disk));
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED), Type != DiskType::VHD);

    for (const auto& [attachedLun, attachedDisk] : m_attachedDisks)
    {
        if (attachedDisk.Type == Type && wsl::windows::common::string::IsPathComponentEqual(attachedDisk.Path.native(), Disk))
        {
            THROW_HR_IF(WSL_E_USER_VHD_ALREADY_ATTACHED, attachedDisk.User);
            return attachedLun;
        }
    }

    const auto allocatedLun = ReserveLun(Lun);
    bool attached = false;
    auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&] {
        if (attached)
        {
            LOG_IF_FAILED(WslOpenVmmVmDetachScsiDisk(m_vm.get(), 0, allocatedLun));
        }

        FreeLun(allocatedLun);
    });

    try
    {
        wil::unique_hfile backingFile{CreateFileW(
            Disk, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
        THROW_LAST_ERROR_IF(!backingFile);

        THROW_IF_FAILED(WslOpenVmmVmAttachScsiDisk(m_vm.get(), 0, allocatedLun, Disk, ReadOnly));
        attached = true;
        m_attachedDisks.emplace(allocatedLun, AttachedDisk{Type, Disk, ReadOnly, IsUserDisk, {}, std::move(backingFile)});
    }
    catch (...)
    {
        const auto result = wil::ResultFromCaughtException();
        THROW_HR_WITH_USER_ERROR(
            result, wsl::shared::Localization::MessageFailedToAttachDisk(Disk, wsl::windows::common::wslutil::GetSystemErrorString(result)));
    }

    cleanup.release();
    return allocatedLun;
}

wil::unique_socket OpenVmmWslCoreVm::ConnectToGuest(_In_ ULONG Port) const
{
    return ConnectToOpenVmmGuest(m_vsockPath, m_vmId, Port, m_terminatingEvent.get(), m_vmConfig.KernelBootTimeout);
}

std::shared_ptr<LxssRunningInstance> OpenVmmWslCoreVm::CreateInstance(
    _In_ const GUID& InstanceId,
    _In_ const LXSS_DISTRO_CONFIGURATION& Configuration,
    _In_ LX_MESSAGE_TYPE MessageType,
    _In_ DWORD ReceiveTimeout,
    _In_ ULONG DefaultUid,
    _In_ ULONG64 ClientLifetimeId,
    _In_ ULONG ExportFlags,
    _Out_opt_ ULONG* ConnectPort)
{
    auto lock = m_lock.lock_exclusive();

    SlowOperationWatcher slowOperation{"AttachDistroVhd"};
    const auto lun = AttachDiskLockHeld(Configuration.VhdFilePath.c_str(), DiskType::VHD, {}, false, false);
    slowOperation.Reset();

    int flags = LxMiniInitMessageFlagNone;
    WI_SetFlagIf(flags, LxMiniInitMessageFlagExportCompressGzip, WI_IsFlagSet(ExportFlags, LXSS_EXPORT_DISTRO_FLAGS_GZIP));
    WI_SetFlagIf(flags, LxMiniInitMessageFlagExportCompressXzip, WI_IsFlagSet(ExportFlags, LXSS_EXPORT_DISTRO_FLAGS_XZIP));
    WI_SetFlagIf(flags, LxMiniInitMessageFlagVerbose, WI_IsFlagSet(ExportFlags, LXSS_EXPORT_DISTRO_FLAGS_VERBOSE));

#ifdef WSL_DEV_INSTALL_PATH
    const std::wstring installPath = TEXT(WSL_DEV_INSTALL_PATH);
#else
    const std::wstring installPath = m_installPath.wstring();
#endif

    wsl::shared::MessageWriter<LX_MINI_INIT_MESSAGE> message{MessageType};
    message->MountDeviceType = LxMiniInitMountDeviceTypeLun;
    message->DeviceId = lun;
    message->Flags = flags;
    message.WriteString(message->FsTypeOffset, "ext4");
    message.WriteString(message->MountOptionsOffset, "discard,errors=remount-ro,data=ordered");
    message.WriteString(message->VmIdOffset, m_machineId);
    message.WriteString(message->DistributionNameOffset, Configuration.Name);
    message.WriteString(message->SharedMemoryRootOffset, std::wstring{});
    message.WriteString(message->InstallPathOffset, installPath);
    message.WriteString(message->UserProfileOffset, std::wstring{});

    auto transaction = m_miniInitChannel.StartTransaction();
    transaction.Send<LX_MINI_INIT_MESSAGE>(message.Span());

    SlowOperationWatcher waitForInit{"WaitForInitDaemonConnect"};
    auto initSocket = AcceptConnection(ReceiveTimeout);
    waitForInit.Reset();

    LXSS_DISTRO_CONFIGURATION localConfig = Configuration;
    WI_ClearFlagIf(localConfig.Flags, LXSS_DISTRO_FLAGS_ENABLE_DRIVE_MOUNTING, !m_vmConfig.EnableHostFileSystemAccess);

    ULONG featureFlags{};
    WI_SetFlag(featureFlags, LxInitFeatureDisable9pServer);
    WI_SetFlagIf(featureFlags, LxInitFeatureVirtIo9p, m_vmConfig.EnableVirtio9p);
    WI_SetFlagIf(featureFlags, LxInitFeatureVirtIoFs, m_vmConfig.EnableVirtioFs);
    WI_SetFlagIf(featureFlags, LxInitFeatureDnsTunneling, m_vmConfig.EnableDnsTunneling);

    const auto connectToGuest = CreateOpenVmmConnector(m_vsockPath, m_vmId, m_vmConfig.KernelBootTimeout);
    wil::unique_socket systemDistroSocket;
    auto instance = std::make_shared<WslCoreInstance>(
        m_userToken.get(),
        initSocket,
        systemDistroSocket,
        InstanceId,
        m_vmId,
        connectToGuest,
        localConfig,
        DefaultUid,
        ClientLifetimeId,
        m_initializeDrvFs,
        featureFlags,
        m_vmConfig.DistributionStartTimeout,
        m_vmConfig.InstanceIdleTimeout,
        ConnectPort,
        m_processJobObject.get());

    WI_ASSERT(!initSocket && !systemDistroSocket);
    return instance;
}

wil::unique_socket OpenVmmWslCoreVm::CreateRootNamespaceProcess(_In_ LPCSTR Path, _In_ LPCSTR* Arguments)
{
    auto lock = m_lock.lock_exclusive();
    const auto connectToGuest = CreateOpenVmmConnector(m_vsockPath, m_vmId, m_vmConfig.KernelBootTimeout);
    return LxssCreateProcess::CreateLinuxProcess(
        Path, Arguments, connectToGuest, m_miniInitChannel, m_terminatingEvent.get(), m_vmConfig.DistributionStartTimeout);
}

std::pair<int, LX_MINI_MOUNT_STEP> OpenVmmWslCoreVm::DetachDisk(_In_opt_ PCWSTR Disk)
{
    bool detached = !ARGUMENT_PRESENT(Disk);
    auto lock = m_lock.lock_exclusive();
    for (auto disk = m_attachedDisks.begin(); disk != m_attachedDisks.end();)
    {
        if (!disk->second.User)
        {
            ++disk;
            continue;
        }

        std::error_code error;
        const bool matches = !ARGUMENT_PRESENT(Disk) || std::filesystem::equivalent(disk->second.Path, Disk, error);
        if (!matches)
        {
            ++disk;
            continue;
        }

        const auto result = UnmountDisk(disk->first, disk->second);
        if (result.first != 0)
        {
            return result;
        }

        THROW_IF_FAILED(WslOpenVmmVmDetachScsiDisk(m_vm.get(), 0, disk->first));
        FreeLun(disk->first);
        detached = true;
        disk = m_attachedDisks.erase(disk);
    }

    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND), !detached);
    return {0, LxMiniInitMountStepNone};
}

void OpenVmmWslCoreVm::EjectVhd(_In_ PCWSTR VhdPath)
{
    auto lock = m_lock.lock_exclusive();
    const auto disk = std::ranges::find_if(m_attachedDisks, [&](const auto& entry) {
        return entry.second.Type == DiskType::VHD && wsl::windows::common::string::IsPathComponentEqual(entry.second.Path.native(), VhdPath);
    });
    if (disk == m_attachedDisks.end())
    {
        return;
    }

    EJECT_VHD_MESSAGE message{};
    message.Header.MessageSize = sizeof(message);
    message.Header.MessageType = LxMiniInitMessageEjectVhd;
    message.Lun = disk->first;
    const auto& result = m_miniInitChannel.Transaction(message);
    LOG_HR_IF_MSG(E_UNEXPECTED, result.Result != 0, "VHD eject failed: %u", result.Result);

    THROW_IF_FAILED(WslOpenVmmVmDetachScsiDisk(m_vm.get(), 0, disk->first));
    FreeLun(disk->first);
    m_attachedDisks.erase(disk);
}

const wsl::core::Config& OpenVmmWslCoreVm::GetConfig() const noexcept
{
    return m_vmConfig;
}

GUID OpenVmmWslCoreVm::GetRuntimeId() const
{
    return m_vmId;
}

int OpenVmmWslCoreVm::GetVmIdleTimeout() const
{
    return m_vmConfig.VmIdleTimeout;
}

bool OpenVmmWslCoreVm::InitializeDrvFs(_In_ HANDLE UserToken)
{
    THROW_HR_IF(E_INVALIDARG, !ARGUMENT_PRESENT(UserToken));
    THROW_HR_IF(E_UNEXPECTED, !m_vmConfig.EnableHostFileSystemAccess || !m_vmConfig.EnableVirtioFs);

    const auto tokenUser = wil::get_token_information<TOKEN_USER>(UserToken);
    THROW_HR_IF(E_ACCESSDENIED, !EqualSid(&m_userSid.Sid, tokenUser->User.Sid));

    const auto elevated = wsl::windows::common::security::IsTokenElevated(UserToken);
    THROW_HR_IF_MSG(
        E_ACCESSDENIED,
        elevated != m_creatorElevated,
        "OpenVMM DrvFs cannot switch elevation context after VM creation");
    THROW_HR_IF(E_UNEXPECTED, !m_virtioFsThread.joinable());

    return elevated;
}

bool OpenVmmWslCoreVm::IsVhdAttached(_In_ PCWSTR VhdPath)
{
    auto lock = m_lock.lock_shared();
    return std::ranges::any_of(m_attachedDisks, [&](const auto& entry) {
        return entry.second.Type == DiskType::VHD &&
               wsl::windows::common::string::IsPathComponentEqual(entry.second.Path.native(), VhdPath);
    });
}

IWslCoreVm::DiskMountResult OpenVmmWslCoreVm::MountDisk(
    _In_ PCWSTR Disk,
    _In_ DiskType MountDiskType,
    _In_ ULONG PartitionIndex,
    _In_opt_ PCWSTR Name,
    _In_opt_ PCWSTR Type,
    _In_opt_ PCWSTR Options)
{
    auto lock = m_lock.lock_exclusive();
    const auto disk = std::ranges::find_if(m_attachedDisks, [&](const auto& entry) {
        return entry.second.Type == MountDiskType &&
               wsl::windows::common::string::IsPathComponentEqual(entry.second.Path.native(), Disk);
    });
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND), disk == m_attachedDisks.end());
    THROW_HR_IF(WSL_E_DISK_ALREADY_MOUNTED, disk->second.Mounts.contains(PartitionIndex));

    auto targetName = GetMountTargetName(Disk, Name, PartitionIndex);
    auto targetNameWide = wsl::shared::string::MultiByteToWide(targetName);
    const auto nameCollision = std::ranges::any_of(m_attachedDisks, [&](const auto& diskEntry) {
        return std::ranges::any_of(diskEntry.second.Mounts, [&](const auto& mountEntry) {
            return wsl::shared::string::IsEqual(mountEntry.second.Name, targetNameWide, false);
        });
    });
    THROW_HR_IF(WSL_E_VM_MODE_MOUNT_NAME_ALREADY_EXISTS, nameCollision);

    wsl::shared::MessageWriter<LX_MINI_INIT_MOUNT_MESSAGE> message{LxMiniInitMessageMount};
    message->PartitionIndex = PartitionIndex;
    message->ScsiLun = disk->first;
    message.WriteString(message->TypeOffset, Type);
    message.WriteString(message->TargetNameOffset, targetName);
    message.WriteString(message->OptionsOffset, Options);

    auto transaction = m_miniInitChannel.StartTransaction();
    transaction.Send<LX_MINI_INIT_MOUNT_MESSAGE>(message.Span());

    wsl::shared::SocketChannel resultChannel{
        AcceptConnection(m_vmConfig.KernelBootTimeout), "MountResult", {m_terminatingEvent.get()}};
    auto [mountResult, step] = GetMountResult(resultChannel);
    if (mountResult == 0)
    {
        Mount mount{.Name = std::move(targetNameWide)};
        if (Type != nullptr)
        {
            mount.Type = Type;
        }
        if (Options != nullptr)
        {
            mount.Options = Options;
        }

        disk->second.Mounts.emplace(PartitionIndex, std::move(mount));
    }

    return {std::move(targetName), mountResult, step};
}

void OpenVmmWslCoreVm::MountRootNamespaceFolder(_In_ LPCWSTR, _In_ LPCWSTR, _In_ bool, _In_ LPCWSTR)
{
    THROW_HR(E_NOTIMPL);
}

void OpenVmmWslCoreVm::RegisterCallbacks(
    _In_ const std::function<void(ULONG)>& DistroExitCallback,
    _In_ const std::function<void(GUID)>& TerminationCallback)
{
    WSL_LOG(
        "OpenVmmWslCoreVm::RegisterCallbacks",
        TraceLoggingValue(static_cast<bool>(DistroExitCallback), "DistroExitCallback"),
        TraceLoggingValue(static_cast<bool>(TerminationCallback), "TerminationCallback"));

    if (DistroExitCallback)
    {
        auto lock = m_lock.lock_exclusive();
        THROW_HR_IF(E_INVALIDARG, !m_notifyChannel || m_distroExitThread.joinable());
        m_distroExitThread = std::thread([exitCallback = DistroExitCallback,
                                          notifyChannel = std::move(m_notifyChannel),
                                          terminationEvent = m_terminatingEvent.get()]() mutable {
            try
            {
                wsl::windows::common::wslutil::SetThreadDescription(L"DistroExitCallback");

                std::vector<gsl::byte> buffer;
                for (;;)
                {
                    const auto message = wsl::shared::socket::RecvMessage(notifyChannel.get(), buffer, terminationEvent);
                    if (message.empty())
                    {
                        break;
                    }

                    const auto* header = gslhelpers::get_struct<MESSAGE_HEADER>(message);
                    if (header->MessageType == LxMiniInitMessageChildExit)
                    {
                        if (const auto* exitMessage = gslhelpers::try_get_struct<LX_MINI_INIT_CHILD_EXIT_MESSAGE>(message))
                        {
                            WSL_LOG("ProcessExited", TraceLoggingValue(exitMessage->ChildPid, "Pid"));
                            exitCallback(exitMessage->ChildPid);
                        }
                    }
                    else
                    {
                        LOG_HR_MSG(E_UNEXPECTED, "Unexpected MessageType %d", header->MessageType);
                    }
                }
            }
            CATCH_LOG()
        });
    }

    if (TerminationCallback)
    {
        auto exitLock = m_exitCallbackLock.lock_exclusive();
        THROW_HR_IF(E_INVALIDARG, m_terminationCallback);
        if (!m_terminatingEvent.is_signaled())
        {
            m_terminationCallback = TerminationCallback;
        }
        else
        {
            std::thread([terminationCallback = TerminationCallback, vmId = m_vmId] {
                wsl::windows::common::wslutil::SetThreadDescription(L"TerminationCallback");
                terminationCallback(vmId);
            }).detach();
        }
    }

    if (m_vmConfig.EnableHostFileSystemAccess && m_vmConfig.EnableVirtioFs)
    {
        THROW_HR_IF(E_INVALIDARG, m_virtioFsThread.joinable());
        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&] {
            m_virtioFsListenSocket.reset();
            DeleteFileW(m_virtioFsListenPath.c_str());
        });

        std::tie(m_virtioFsListenSocket, m_virtioFsListenPath) =
            CreateVsockListener(LX_INIT_UTILITY_VM_VIRTIOFS_PORT);
        m_virtioFsThread = std::thread(&OpenVmmWslCoreVm::VirtioFsWorker, this);
        cleanup.release();
    }
}

void OpenVmmWslCoreVm::ResizeDistribution(_In_ ULONG Lun, _In_ HANDLE OutputHandle, _In_ ULONG64 NewSize)
{
    auto lock = m_lock.lock_exclusive();

    LX_MINI_INIT_RESIZE_DISTRIBUTION_MESSAGE message{};
    message.Header.MessageSize = sizeof(message);
    message.Header.MessageType = LxMiniInitMessageResizeDistribution;
    message.ScsiLun = Lun;
    message.NewSize = NewSize;

    auto transaction = m_miniInitChannel.StartTransaction();
    transaction.Send(message);

    wsl::shared::SocketChannel responseChannel{
        AcceptConnection(m_vmConfig.KernelBootTimeout), "ResizeDistribution", {m_terminatingEvent.get()}};
    auto outputChannel = AcceptConnection(m_vmConfig.KernelBootTimeout);
    wsl::windows::common::relay::ScopedRelay outputRelay{std::move(outputChannel), OutputHandle};

    const auto& response = responseChannel.ReceiveMessage<LX_MINI_INIT_RESIZE_DISTRIBUTION_RESPONSE>();
    if (response.ResponseCode != 0)
    {
        THROW_HR_WITH_USER_ERROR(E_FAIL, wsl::shared::Localization::MessageFailedToResizeDisk());
    }
}

void OpenVmmWslCoreVm::SaveAttachedDisksState()
try
{
    auto lock = m_lock.lock_exclusive();
    const auto key = wsl::windows::common::registry::OpenOrCreateLxssDiskMountsKey(&m_userSid.Sid);
    for (const auto& [lun, disk] : m_attachedDisks)
    {
        if (!disk.User)
        {
            continue;
        }

        const auto diskKeyPath = std::to_wstring(lun);
        const auto diskKey = wsl::windows::common::registry::CreateKey(
            key.get(), diskKeyPath.c_str(), KEY_ALL_ACCESS, nullptr, REG_OPTION_VOLATILE);
        wsl::windows::common::registry::WriteString(diskKey.get(), nullptr, c_diskValueName, disk.Path.c_str());
        wsl::windows::common::registry::WriteDword(
            diskKey.get(), nullptr, c_disktypeValueName, static_cast<DWORD>(disk.Type));

        for (const auto& [partitionIndex, mount] : disk.Mounts)
        {
            const auto mountKeyPath = std::to_wstring(partitionIndex);
            const auto mountKey = wsl::windows::common::registry::CreateKey(
                diskKey.get(), mountKeyPath.c_str(), KEY_ALL_ACCESS, nullptr, REG_OPTION_VOLATILE);
            wsl::windows::common::registry::WriteString(
                mountKey.get(), nullptr, c_mountNameValueName, mount.Name.c_str());

            if (mount.Options.has_value())
            {
                wsl::windows::common::registry::WriteString(
                    mountKey.get(), nullptr, c_optionsValueName, mount.Options->c_str());
            }
            if (mount.Type.has_value())
            {
                wsl::windows::common::registry::WriteString(
                    mountKey.get(), nullptr, c_typeValueName, mount.Type->c_str());
            }
        }
    }
}
CATCH_LOG()

void OpenVmmWslCoreVm::TrimDistribution(_In_ ULONG Lun)
{
    auto lock = m_lock.lock_exclusive();

    LX_MINI_INIT_TRIM_DISTRIBUTION_MESSAGE message{};
    message.Header.MessageSize = sizeof(message);
    message.Header.MessageType = LxMiniInitMessageTrimDistribution;
    message.ScsiLun = Lun;

    auto transaction = m_miniInitChannel.StartTransaction();
    transaction.Send(message);

    wsl::shared::SocketChannel responseChannel{
        AcceptConnection(m_vmConfig.KernelBootTimeout), "TrimDistribution", {m_terminatingEvent.get()}};
    const auto& response = responseChannel.ReceiveMessage<LX_MINI_INIT_TRIM_DISTRIBUTION_RESPONSE>();
    THROW_HR_IF(E_FAIL, response.ResponseCode != 0);
}