// Copyright (C) Microsoft Corporation. All rights reserved.

/*++

Module Name:

    OpenVmmVirtualMachine.cpp

Abstract:

    Implementation of IWSLCVirtualMachine using OpenVMM as the VMM backend.

    Spawns openvmm.exe in ttrpc orchestration mode and configures the VM via
    vmservice RPCs (CreateVM, ResumeVM, ModifyResource, etc.).

    Current limitations:
    - AddShare/RemoveShare require vmservice.proto extensions.
    - GPU passthrough is not supported.

--*/

#include "precomp.h"

#include "OpenVmmVirtualMachine.h"
#include <format>
#include <filesystem>
#include <afunix.h>
#include "wslutil.h"
#include "lxinitshared.h"
#include "ConsommeNetworking.h"

using namespace wsl::windows::common;
using wsl::windows::service::wslc::OpenVmmVirtualMachine;
using wsl::windows::service::wslc::TtrpcClient;
namespace wslutil = wsl::windows::common::wslutil;

OpenVmmVirtualMachine::OpenVmmVirtualMachine(_In_ const WSLCSessionSettings* Settings)
{
    THROW_HR_IF(E_POINTER, Settings == nullptr);

    std::lock_guard lock(m_lock);

    THROW_IF_FAILED(CoCreateGuid(&m_vmId));
    m_vmIdString = wsl::shared::string::GuidToString<wchar_t>(m_vmId, wsl::shared::string::GuidToStringFlags::Uppercase);
    m_featureFlags = Settings->FeatureFlags;

    // Disable features not yet supported by the OpenVMM backend.
    WI_ClearFlag(m_featureFlags, WslcFeatureFlagsGPU);
    WI_ClearFlag(m_featureFlags, WslcFeatureFlagsVirtioFs);

    m_networkingMode = Settings->NetworkingMode;
    m_bootTimeoutMs = Settings->BootTimeoutMs;
    m_cpuCount = Settings->CpuCount;
    m_memoryMb = Settings->MemoryMb;

    // Resolve paths for kernel, initrd, and root VHD.
    auto basePath = wslutil::GetBasePath();

#ifdef WSL_KERNEL_PATH
    m_kernelPath = std::filesystem::path(WSL_KERNEL_PATH);
#else
    m_kernelPath = basePath / L"tools" / L"vmlinux";
    if (!std::filesystem::exists(m_kernelPath))
    {
        // Fall back to the standard kernel name if vmlinux is not found.
        m_kernelPath = basePath / L"tools" / LXSS_VM_MODE_KERNEL_NAME;
    }
#endif

    m_initrdPath = basePath / L"tools" / LXSS_VM_MODE_INITRD_NAME;

#ifdef WSL_KERNEL_MODULES_PATH
    m_modulesVhdPath = std::filesystem::path(TEXT(WSL_KERNEL_MODULES_PATH));
#else
    m_modulesVhdPath = basePath / L"tools" / L"modules.vhd";
#endif

    if (Settings->RootVhdOverride != nullptr)
    {
        m_rootVhdPath = Settings->RootVhdOverride;
    }
    else
    {
#ifdef WSL_SYSTEM_DISTRO_PATH
        m_rootVhdPath = TEXT(WSL_SYSTEM_DISTRO_PATH);
#else
        m_rootVhdPath = std::filesystem::path(wslutil::GetMsiPackagePath().value()) / L"system.vhd";
#endif
    }

    // Locate openvmm.exe. Expect it alongside the WSL binaries.
    m_openvmmPath = basePath / L"openvmm.exe";
    THROW_HR_IF_MSG(
        HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND),
        !std::filesystem::exists(m_openvmmPath),
        "openvmm.exe not found at: %ls",
        m_openvmmPath.c_str());

    // Pre-create the container storage VHDX so it's ready for hot-attach.
    // WSLCSession::ConfigureStorage will attach, format, and mount it later.
    if (Settings->StoragePath != nullptr)
    {
        std::filesystem::path storagePath{Settings->StoragePath};
        m_storageVhdPath = storagePath / L"storage.vhdx";

        std::filesystem::create_directories(storagePath);
        if (!std::filesystem::exists(m_storageVhdPath))
        {
            VIRTUAL_STORAGE_TYPE storageType{VIRTUAL_STORAGE_TYPE_DEVICE_VHDX, VIRTUAL_STORAGE_TYPE_VENDOR_MICROSOFT};
            CREATE_VIRTUAL_DISK_PARAMETERS createParams{};
            createParams.Version = CREATE_VIRTUAL_DISK_VERSION_2;
            createParams.Version2.MaximumSize = Settings->MaximumStorageSizeMb * 1024ULL * 1024ULL;
            wil::unique_hfile diskHandle;
            THROW_IF_WIN32_ERROR_MSG(CreateVirtualDisk(
                &storageType, m_storageVhdPath.c_str(), VIRTUAL_DISK_ACCESS_NONE,
                nullptr, CREATE_VIRTUAL_DISK_FLAG_NONE, 0, &createParams, nullptr, &diskHandle),
                "Failed to create storage VHDX: %ls", m_storageVhdPath.c_str());
        }
    }

    // Build kernel command line matching HcsVirtualMachine's format.
    m_kernelCmdLine = L"initrd=\\" LXSS_VM_MODE_INITRD_NAME L" " TEXT(WSLC_ROOT_INIT_ENV) L"=1 panic=-1";
    m_kernelCmdLine += std::format(L" nr_cpus={}", Settings->CpuCount);

    // Append common WSL kernel parameters (timesync, printk, page reporting).
    helpers::AppendCommonKernelCommandLine(m_kernelCmdLine, c_pageReportingOrder);

    // Setup dmesg collector with optional DmesgOutput handle, matching HcsVirtualMachine.
    // The DmesgCollector creates named pipes that we pass to OpenVMM via serial and
    // virtio console configs to capture kernel output.
    wil::unique_handle dmesgOutputHandle;
    if (Settings->DmesgOutput.Handle.File != nullptr && Settings->DmesgOutput.Handle.File != INVALID_HANDLE_VALUE)
    {
        dmesgOutputHandle.reset(wslutil::DuplicateHandle(wslutil::FromCOMInputHandle(Settings->DmesgOutput), GENERIC_WRITE | SYNCHRONIZE));
    }

    // REVIEW: Can we always enable earlycon?
    m_dmesgCollector = DmesgCollector::Create(
        m_vmId, m_vmExitEvent, true, false, L"", true /* earlycon */, std::move(dmesgOutputHandle));

    // Earlycon captures kernel output via COM1 before the hvc0 driver loads.
    m_kernelCmdLine += L" earlycon=uart8250,io,0x3f8,115200";

    m_kernelCmdLine += L" console=hvc0 debug";

    // Set up vsock bridge path for HvSocket emulation.
    // OpenVMM uses a Unix domain socket for the hybrid_vsock bridge.
    // The hybrid_vsock bridge appends "_<port>" (e.g. "_50000") to this path,
    // and Unix domain sockets have a 108-byte path limit on Windows.
    // The SYSTEM profile temp path is too long, so use a short fixed directory.
    auto vsockDir = std::filesystem::path(c_vsockBridgeDir);
    std::filesystem::create_directories(vsockDir);
    // Use first 8 chars of the GUID to keep it short but unique.
    m_vsockPath = vsockDir / std::format(L"vm-{:.8}", m_vmIdString);

    // Set up the ttrpc socket path for runtime VM management.
    m_ttrpcSocketPath = vsockDir / std::format(L"vm-{:.8}.ttrpc", m_vmIdString);
    DeleteFileW(m_ttrpcSocketPath.c_str());

    // Setup boot VHDs — use the same pattern as HcsVirtualMachine.
    auto attachBootDisk = [&](PCWSTR path) {
        const ULONG lun = AllocateLun();
        DiskInfo disk{path, true};
        m_attachedDisks.emplace(lun, std::move(disk));
    };

    attachBootDisk(m_rootVhdPath.c_str());
    attachBootDisk(m_modulesVhdPath.c_str());

    auto cleanupOnFailure = wil::scope_exit([this]() {
        m_vmExitEvent.SetEvent();

        if (m_ttrpcClient)
        {
            m_ttrpcClient->Disconnect();
            m_ttrpcClient.reset();
        }

        if (m_processHandle)
        {
            TerminateProcess(m_processHandle.get(), 1);
        }

        if (m_processWatchThread.joinable())
        {
            m_processWatchThread.join();
        }

        if (m_initListenSocket != INVALID_SOCKET)
        {
            closesocket(m_initListenSocket);
            m_initListenSocket = INVALID_SOCKET;
        }
        DeleteFileW(m_initListenPath.c_str());

        if (m_crashDumpListenSocket != INVALID_SOCKET)
        {
            closesocket(m_crashDumpListenSocket);
            m_crashDumpListenSocket = INVALID_SOCKET;
        }
        DeleteFileW(m_crashDumpListenPath.c_str());

        try
        {
            if (!m_ttrpcSocketPath.empty())
            {
                std::filesystem::remove(m_ttrpcSocketPath);
            }
        }
        CATCH_LOG()
    });

    // Create Unix domain socket listeners for the hybrid_vsock bridge BEFORE
    // launching openvmm. The guest connects to vsock ports immediately on boot,
    // and OpenVMM's hybrid_vsock bridge relays connections to the host.
    //
    // The bridge uses the HvSocket GUID template to construct the path:
    // port 50000 (0xC350) becomes GUID 0000c350-facb-11e6-bd58-64006a7986d3,
    // and the bridge looks for <vsock_path>_<guid> on the host.
    std::tie(m_initListenSocket, m_initListenPath) =
        CreateVsockListener(LX_INIT_UTILITY_VM_INIT_PORT);

    std::tie(m_crashDumpListenSocket, m_crashDumpListenPath) =
        CreateVsockListener(LX_INIT_UTILITY_VM_CRASH_DUMP_PORT);

    // Launch the openvmm process.
    LaunchOpenVmm();

    cleanupOnFailure.release();
}

std::pair<SOCKET, std::wstring> OpenVmmVirtualMachine::CreateVsockListener(ULONG port)
{
    auto portHex = std::format(L"{:08x}", port);
    auto listenPath = std::format(L"{}_{}-facb-11e6-bd58-64006a7986d3", m_vsockPath.wstring(), portHex);
    DeleteFileW(listenPath.c_str());

    SOCKET listenSocket = ::socket(AF_UNIX, SOCK_STREAM, 0);
    THROW_LAST_ERROR_IF(listenSocket == INVALID_SOCKET);
    auto closeOnFailure = wil::scope_exit([&] { closesocket(listenSocket); });

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    auto narrowPath = wsl::shared::string::WideToMultiByte(listenPath);
    THROW_HR_IF_MSG(E_INVALIDARG, narrowPath.size() >= sizeof(addr.sun_path),
        "vsock bridge path too long: %hs", narrowPath.c_str());
    memcpy(addr.sun_path, narrowPath.c_str(), narrowPath.size() + 1);

    THROW_LAST_ERROR_IF(bind(listenSocket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR);
    THROW_LAST_ERROR_IF(listen(listenSocket, 1) == SOCKET_ERROR);

    WSL_LOG("OpenVmmVsockListenerReady",
        TraceLoggingValue(listenPath.c_str(), "ListenPath"),
        TraceLoggingValue(port, "Port"));

    closeOnFailure.release();
    return {listenSocket, std::move(listenPath)};
}

std::wstring OpenVmmVirtualMachine::BuildCommandLine() const
{
    std::wstring cmd = std::format(L"\"{}\"", m_openvmmPath.wstring());
    cmd += std::format(L" --ttrpc \"{}\"", m_ttrpcSocketPath.wstring());

    return cmd;
}

TtrpcClient::VmConfig OpenVmmVirtualMachine::BuildVmConfig() const
{
    TtrpcClient::VmConfig config;

    config.KernelPath = wsl::shared::string::WideToMultiByte(m_kernelPath.wstring());
    config.InitrdPath = wsl::shared::string::WideToMultiByte(m_initrdPath.wstring());

    // Kernel command line — the server prepends "panic=-1 debug pci=off console=ttyS0 "
    // automatically via HyperVGen2LinuxDirect chipset type.
    config.KernelCmdLine = wsl::shared::string::WideToMultiByte(m_kernelCmdLine);

    // Ensure 2MB granularity. Cap at 4GB because OpenVMM on WHP allocates guest RAM upfront.
    constexpr ULONG c_maxMemoryMb = 4096;
    config.MemoryMb = std::min(m_memoryMb, c_maxMemoryMb) & ~0x1;

    config.ProcessorCount = m_cpuCount;

    // HvSocket bridge via vsock path (for the guest init connection).
    config.HvSocketPath = wsl::shared::string::WideToMultiByte(m_vsockPath.wstring());

    // Boot disks: root VHD (LUN 0) and modules VHD (LUN 1), both read-only.
    for (const auto& [lun, disk] : m_attachedDisks)
    {
        config.ScsiDisks.push_back({
            .Controller = 0,
            .Lun = lun,
            .HostPath = wsl::shared::string::WideToMultiByte(disk.Path),
            .ReadOnly = disk.ReadOnly,
        });
    }

    if (m_networkingMode == WSLCNetworkingModeConsomme)
    {
        // Generate a deterministic NIC instance ID from the VM ID so it's
        // stable across restarts but unique per VM.
        GUID nicGuid = m_vmId;
        nicGuid.Data1 ^= c_nicGuidXorMask;

        config.Nic = TtrpcClient::VmConfig::ConsommeNic{
            .NicId = wsl::shared::string::GuidToString<char>(nicGuid),
            .MacAddress = c_defaultConsommeMacAddress,
        };
    }

    // COM1 (port 0) — earlycon output before hvc0 loads.
    config.SerialPorts.push_back({
        .Port = 0,
        .SocketPath = wsl::shared::string::WideToMultiByte(m_dmesgCollector->EarlyConsoleName()),
    });

    // Virtio console (/dev/hvc0) — primary console after boot.
    config.VirtioConsolePath = wsl::shared::string::WideToMultiByte(m_dmesgCollector->VirtioConsoleName());

    return config;
}

void OpenVmmVirtualMachine::LaunchOpenVmm()
{
    auto cmd = BuildCommandLine();

    WSL_LOG("LaunchOpenVmm", TraceLoggingValue(cmd.c_str(), "cmd"));

    SubProcess process(m_openvmmPath.c_str(), cmd.c_str());

    // Set OPENVMM_LOG so the openvmm tracing subscriber emits detailed logs.
    // Without this, only INFO-level messages appear (the default), which omits
    // most operational output from VM creation and runtime.
    // The variable is set in the current process environment and inherited by the
    // child; restore it after Start() to avoid polluting the service environment.
    wil::unique_hlocal_string previousLog;
    DWORD prevLen = GetEnvironmentVariableW(L"OPENVMM_LOG", nullptr, 0);
    if (prevLen > 0)
    {
        previousLog.reset(static_cast<PWSTR>(LocalAlloc(LMEM_FIXED, prevLen * sizeof(WCHAR))));
        THROW_IF_NULL_ALLOC(previousLog.get());
        GetEnvironmentVariableW(L"OPENVMM_LOG", previousLog.get(), prevLen);
    }

    SetEnvironmentVariableW(L"OPENVMM_LOG", L"info,openvmm=debug");
    auto restoreEnv = wil::scope_exit([&] {
        SetEnvironmentVariableW(L"OPENVMM_LOG", previousLog.get());
    });

    // Redirect stdout and stderr to a log file for diagnostics.
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    auto logPath = m_vsockPath.wstring() + L".log";
    wil::unique_hfile logFile{CreateFileW(
        logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)};

    // Duplicate the log file handle for stderr so that stdout and stderr are
    // independent.  OpenVMM closes stdout after startup (pal::close_stdout),
    // and if both handles share the same value that also invalidates stderr,
    // silencing all tracing output.
    wil::unique_hfile logFileForStderr;
    THROW_IF_WIN32_BOOL_FALSE(DuplicateHandle(
        GetCurrentProcess(), logFile.get(),
        GetCurrentProcess(), logFileForStderr.put(),
        0, TRUE, DUPLICATE_SAME_ACCESS));

    process.SetStdHandles(nullptr, logFile.get(), logFileForStderr.get());

    // Start the process. The returned handle is the process handle.
    m_processHandle = process.Start();

    // Kill-on-close job object ensures the child is terminated if the service
    // exits without running our destructor.
    m_jobObject.reset(CreateJobObjectW(nullptr, nullptr));
    THROW_LAST_ERROR_IF(!m_jobObject);

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jobLimits{};
    jobLimits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    THROW_IF_WIN32_BOOL_FALSE(SetInformationJobObject(
        m_jobObject.get(), JobObjectExtendedLimitInformation, &jobLimits, sizeof(jobLimits)));
    THROW_IF_WIN32_BOOL_FALSE(AssignProcessToJobObject(m_jobObject.get(), m_processHandle.get()));

    logFile.reset();

    // Monitor the openvmm process and signal m_vmExitEvent on exit.
    m_processWatchThread = std::thread(&OpenVmmVirtualMachine::WatchProcessExit, this);

    m_ttrpcClient = std::make_unique<TtrpcClient>();
    THROW_IF_FAILED_MSG(
        m_ttrpcClient->Connect(m_ttrpcSocketPath.wstring(), TtrpcClient::c_defaultTimeoutMs),
        "Failed to connect to OpenVMM ttrpc server");

    auto vmConfig = BuildVmConfig();
    THROW_IF_FAILED_MSG(
        m_ttrpcClient->CreateVm(vmConfig),
        "Failed to create VM via ttrpc CreateVM");

    THROW_IF_FAILED_MSG(
        m_ttrpcClient->ResumeVm(),
        "Failed to resume VM via ttrpc ResumeVM");

}

void OpenVmmVirtualMachine::WatchProcessExit()
{
    WaitForSingleObject(m_processHandle.get(), INFINITE);

    DWORD exitCode = 0;
    GetExitCodeProcess(m_processHandle.get(), &exitCode);

    WSL_LOG(
        "OpenVmmProcessExited",
        TraceLoggingValue(exitCode, "ExitCode"),
        TraceLoggingValue(m_vmIdString.c_str(), "VmId"));

    m_vmExitEvent.SetEvent();
}

OpenVmmVirtualMachine::~OpenVmmVirtualMachine()
{
    WSL_LOG("OpenVmmTerminateVmStart", TraceLoggingValue(m_vmIdString.c_str(), "VmId"));

    // Signal termination to any pending operations.
    m_vmExitEvent.SetEvent();

    // TeardownVM releases all VM resources and unblocks WaitVM.
    if (m_ttrpcClient)
    {
        LOG_IF_FAILED(m_ttrpcClient->TeardownVm());
        m_ttrpcClient->Disconnect();
        m_ttrpcClient.reset();
    }

    // Wait up to 5 seconds for graceful exit, then force-terminate.
    if (m_processHandle)
    {
        if (WaitForSingleObject(m_processHandle.get(), c_processTerminationTimeoutMs) == WAIT_TIMEOUT)
        {
            WSL_LOG("OpenVmmForceTerminate", TraceLoggingValue(m_vmIdString.c_str(), "VmId"));
            TerminateProcess(m_processHandle.get(), 1);
        }
    }

    if (m_processWatchThread.joinable())
    {
        m_processWatchThread.join();
    }

    if (m_initListenSocket != INVALID_SOCKET)
    {
        closesocket(m_initListenSocket);
        m_initListenSocket = INVALID_SOCKET;
    }
    DeleteFileW(m_initListenPath.c_str());

    if (m_crashDumpListenSocket != INVALID_SOCKET)
    {
        closesocket(m_crashDumpListenSocket);
        m_crashDumpListenSocket = INVALID_SOCKET;
    }
    DeleteFileW(m_crashDumpListenPath.c_str());

    try
    {
        if (std::filesystem::exists(m_vsockPath))
        {
            std::filesystem::remove(m_vsockPath);
        }
        if (std::filesystem::exists(m_ttrpcSocketPath))
        {
            std::filesystem::remove(m_ttrpcSocketPath);
        }
    }
    CATCH_LOG()
}

bool OpenVmmVirtualMachine::FeatureEnabled(WSLCFeatureFlags Value) const
{
    return static_cast<ULONG>(m_featureFlags) & static_cast<ULONG>(Value);
}

HRESULT OpenVmmVirtualMachine::GetId(_Out_ GUID* VmId)
try
{
    *VmId = m_vmId;
    return S_OK;
}
CATCH_RETURN()

HRESULT OpenVmmVirtualMachine::AcceptConnection(_Out_ HANDLE* Socket)
try
{
    THROW_HR_IF(E_UNEXPECTED, m_initListenSocket == INVALID_SOCKET);

    WSL_LOG("OpenVmmAcceptConnection",
        TraceLoggingValue(m_initListenPath.c_str(), "ListenPath"),
        TraceLoggingValue(m_vmIdString.c_str(), "VmId"));

    wil::unique_event acceptEvent(wil::EventOptions::ManualReset);
    WSAEventSelect(m_initListenSocket, acceptEvent.get(), FD_ACCEPT);

    HANDLE waitHandles[] = { acceptEvent.get(), m_vmExitEvent.get() };
    auto waitResult = WaitForMultipleObjects(ARRAYSIZE(waitHandles), waitHandles, FALSE, m_bootTimeoutMs);
    THROW_HR_IF(E_ABORT, waitResult != WAIT_OBJECT_0);

    SOCKET unixSock = accept(m_initListenSocket, nullptr, nullptr);
    THROW_LAST_ERROR_IF(unixSock == INVALID_SOCKET);

    closesocket(m_initListenSocket);
    m_initListenSocket = INVALID_SOCKET;
    DeleteFileW(m_initListenPath.c_str());

    // Return the AF_UNIX socket directly. Callers that wrap it in a
    // SocketChannel should use blocking I/O mode since AF_UNIX on Windows
    // does not support overlapped I/O.
    *Socket = reinterpret_cast<HANDLE>(unixSock);
    return S_OK;
}
CATCH_RETURN()

HRESULT OpenVmmVirtualMachine::ConfigureNetworking(_In_ HANDLE GnsSocket, _In_opt_ HANDLE* DnsSocket)
try
{
    std::lock_guard lock(m_lock);

    // Consomme networking is configured server-side via NICConfig.
    WI_ASSERT(m_networkingMode == WSLCNetworkingModeConsomme);
    THROW_HR_IF(E_INVALIDARG, m_networkingMode != WSLCNetworkingModeConsomme);
    return S_OK;
}
CATCH_RETURN()

HRESULT OpenVmmVirtualMachine::AttachDisk(_In_ LPCWSTR Path, _In_ BOOL ReadOnly, _Out_ ULONG* Lun)
try
{
    RETURN_HR_IF(E_POINTER, Path == nullptr || Lun == nullptr);

    std::lock_guard lock(m_lock);

    THROW_HR_IF_MSG(E_FAIL, !m_ttrpcClient || !m_ttrpcClient->IsConnected(),
        "ttrpc client not connected for disk hot-add");

    DiskInfo disk{Path, ReadOnly != FALSE};
    const ULONG allocatedLun = AllocateLun();

    auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
        FreeLun(allocatedLun);
    });

    auto hostPath = wsl::shared::string::WideToMultiByte(Path);
    THROW_IF_FAILED(m_ttrpcClient->AttachScsiDisk(0, allocatedLun, hostPath, ReadOnly != FALSE));

    m_attachedDisks.emplace(allocatedLun, std::move(disk));
    cleanup.release();

    *Lun = allocatedLun;
    return S_OK;
}
CATCH_RETURN()

HRESULT OpenVmmVirtualMachine::DetachDisk(_In_ ULONG Lun)
try
{
    std::lock_guard lock(m_lock);

    auto it = m_attachedDisks.find(Lun);
    RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), it == m_attachedDisks.end());

    THROW_HR_IF_MSG(E_FAIL, !m_ttrpcClient || !m_ttrpcClient->IsConnected(),
        "ttrpc client not connected for disk hot-remove");

    THROW_IF_FAILED(m_ttrpcClient->DetachScsiDisk(0, Lun));

    FreeLun(Lun);
    m_attachedDisks.erase(it);

    return S_OK;
}
CATCH_RETURN()

HRESULT OpenVmmVirtualMachine::AddShare(_In_ LPCWSTR WindowsPath, _In_ BOOL ReadOnly, _Out_ GUID* ShareId)
try
{
    RETURN_HR_IF(E_POINTER, WindowsPath == nullptr || ShareId == nullptr);

    std::lock_guard lock(m_lock);

    // TODO: Requires vmservice.proto extension for Plan9/VirtioFS in ModifyResourceRequest.

    WSL_LOG(
        "OpenVmmAddShare",
        TraceLoggingValue(m_vmIdString.c_str(), "VmId"),
        TraceLoggingValue(WindowsPath, "WindowsPath"),
        TraceLoggingValue(ReadOnly, "ReadOnly"),
        TraceLoggingValue("NOT_IMPLEMENTED", "Status"));

    return E_NOTIMPL;
}
CATCH_RETURN()

HRESULT OpenVmmVirtualMachine::RemoveShare(_In_ REFGUID ShareId)
try
{
    std::lock_guard lock(m_lock);

    // TODO: Requires vmservice.proto extension. See AddShare.

    WSL_LOG(
        "OpenVmmRemoveShare",
        TraceLoggingValue(m_vmIdString.c_str(), "VmId"),
        TraceLoggingValue("NOT_IMPLEMENTED", "Status"));

    return E_NOTIMPL;
}
CATCH_RETURN()

HRESULT OpenVmmVirtualMachine::GetTerminationEvent(_Out_ HANDLE* Event)
try
{
    *Event = wslutil::DuplicateHandle(m_vmExitEvent.get());
    return S_OK;
}
CATCH_RETURN()

HRESULT OpenVmmVirtualMachine::ConnectToVsockPort(_In_ ULONG Port, _Out_ HANDLE* Socket)
try
{
    WSL_LOG("OpenVmmConnectToVsockPort",
        TraceLoggingValue(Port, "Port"),
        TraceLoggingValue(m_vsockPath.c_str(), "BridgePath"),
        TraceLoggingValue(m_vmIdString.c_str(), "VmId"));

    SOCKET unixSock = ::socket(AF_UNIX, SOCK_STREAM, 0);
    THROW_LAST_ERROR_IF(unixSock == INVALID_SOCKET);
    auto closeUnix = wil::scope_exit([&] { closesocket(unixSock); });

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    auto narrowPath = wsl::shared::string::WideToMultiByte(m_vsockPath.wstring());
    THROW_HR_IF_MSG(E_INVALIDARG, narrowPath.size() >= sizeof(addr.sun_path),
        "vsock bridge path too long: %hs", narrowPath.c_str());
    memcpy(addr.sun_path, narrowPath.c_str(), narrowPath.size() + 1);

    THROW_LAST_ERROR_IF(connect(unixSock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR);

    auto connectMsg = std::format("CONNECT {}\n", Port);
    int sent = send(unixSock, connectMsg.c_str(), static_cast<int>(connectMsg.size()), 0);
    THROW_LAST_ERROR_IF(sent == SOCKET_ERROR);
    THROW_HR_IF(E_FAIL, sent != static_cast<int>(connectMsg.size()));

    char response[64]{};
    int totalRead = 0;
    while (totalRead < static_cast<int>(sizeof(response) - 1))
    {
        int n = recv(unixSock, response + totalRead, 1, 0);
        THROW_LAST_ERROR_IF(n == SOCKET_ERROR);
        THROW_HR_IF_MSG(E_FAIL, n == 0, "vsock bridge closed during CONNECT handshake");
        totalRead += n;
        if (response[totalRead - 1] == '\n')
        {
            break;
        }
    }
    response[totalRead] = '\0';

    THROW_HR_IF_MSG(E_FAIL, strncmp(response, "OK ", 3) != 0,
        "vsock bridge CONNECT failed: %hs", response);

    WSL_LOG("OpenVmmConnectToVsockPortOK",
        TraceLoggingValue(Port, "Port"),
        TraceLoggingValue(response, "Response"));

    // Return the AF_UNIX socket directly. Callers that wrap it in a
    // SocketChannel should use blocking I/O mode since AF_UNIX on Windows
    // does not support overlapped I/O.
    closeUnix.release();
    *Socket = reinterpret_cast<HANDLE>(unixSock);
    return S_OK;
}
CATCH_RETURN()

HRESULT OpenVmmVirtualMachine::AcceptCrashDumpConnection(_Out_ HANDLE* Socket)
try
{
    THROW_HR_IF(E_UNEXPECTED, m_crashDumpListenSocket == INVALID_SOCKET);

    WSL_LOG("OpenVmmAcceptCrashDumpConnection",
        TraceLoggingValue(m_crashDumpListenPath.c_str(), "ListenPath"),
        TraceLoggingValue(m_vmIdString.c_str(), "VmId"));

    wil::unique_event acceptEvent(wil::EventOptions::ManualReset);
    WSAEventSelect(m_crashDumpListenSocket, acceptEvent.get(), FD_ACCEPT);

    HANDLE waitHandles[] = { acceptEvent.get(), m_vmExitEvent.get() };
    auto waitResult = WaitForMultipleObjects(ARRAYSIZE(waitHandles), waitHandles, FALSE, INFINITE);
    THROW_HR_IF(E_ABORT, waitResult != WAIT_OBJECT_0);

    SOCKET unixSock = accept(m_crashDumpListenSocket, nullptr, nullptr);
    THROW_LAST_ERROR_IF(unixSock == INVALID_SOCKET);

    // Return the AF_UNIX socket directly. Callers that wrap it in a
    // SocketChannel should use blocking I/O mode since AF_UNIX on Windows
    // does not support overlapped I/O.
    *Socket = reinterpret_cast<HANDLE>(unixSock);
    return S_OK;
}
CATCH_RETURN()

ULONG OpenVmmVirtualMachine::AllocateLun()
{
    for (ULONG index = 0; index < gsl::narrow_cast<ULONG>(m_lunBitmap.size()); index += 1)
    {
        if (!m_lunBitmap[index])
        {
            m_lunBitmap[index] = true;
            return index;
        }
    }

    THROW_HR(WSL_E_TOO_MANY_DISKS_ATTACHED);
}

void OpenVmmVirtualMachine::FreeLun(ULONG Lun)
{
    THROW_HR_IF(E_BOUNDS, Lun >= m_lunBitmap.size());
    THROW_HR_IF(E_INVALIDARG, !m_lunBitmap[Lun]);

    m_lunBitmap[Lun] = false;
}
