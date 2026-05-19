// Copyright (C) Microsoft Corporation. All rights reserved.

/*++

Module Name:

    OpenVmmVirtualMachine.cpp

Abstract:

    Implementation of IWSLCVirtualMachine using OpenVMM as the VMM backend.

    This is a proof-of-concept implementation that spawns openvmm.exe as a
    child process with CLI arguments to boot a Linux VM, and provides the
    same interface as HcsVirtualMachine.

    Current limitations (PoC):
    - AttachDisk/DetachDisk at runtime require gRPC integration (TODO).
    - AddShare/RemoveShare require OpenVMM proto extensions (TODO).
    - ConfigureNetworking needs socket passthrough design (TODO).
    - GPU passthrough is not supported (deferred).

--*/

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
    WI_ClearFlag(m_featureFlags, WslcFeatureFlagsGPU);      // No GPU passthrough on WHP
    WI_ClearFlag(m_featureFlags, WslcFeatureFlagsVirtioFs);  // No VirtioFS hot-add yet

    m_networkingMode = Settings->NetworkingMode;
    m_bootTimeoutMs = Settings->BootTimeoutMs;
    m_cpuCount = Settings->CpuCount;
    m_memoryMb = Settings->MemoryMb;

    // Resolve paths for kernel, initrd, and root VHD.
    auto basePath = wslutil::GetBasePath();

    // OpenVMM requires an uncompressed ELF vmlinux, not a bzImage.
    // The standard WSL kernel file is a bzImage (MZ/PE header) which HCS handles
    // natively. For OpenVMM, a separate vmlinux must be placed alongside it.
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

    // Pre-attach the container storage VHDX at boot time.
    // Create the VHDX if it doesn't exist — it will be formatted in the guest
    // by ConfigureStorage when the mount fails (unformatted disk detection).
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
            HANDLE diskHandle = INVALID_HANDLE_VALUE;
            THROW_IF_WIN32_ERROR_MSG(CreateVirtualDisk(
                &storageType, m_storageVhdPath.c_str(), VIRTUAL_DISK_ACCESS_NONE,
                nullptr, CREATE_VIRTUAL_DISK_FLAG_NONE, 0, &createParams, nullptr, &diskHandle),
                "Failed to create storage VHDX: %ls", m_storageVhdPath.c_str());
            if (diskHandle != INVALID_HANDLE_VALUE)
            {
                CloseHandle(diskHandle);
            }
        }
    }

    // Build kernel command line matching HcsVirtualMachine's format.
    m_kernelCmdLine = L"initrd=\\" LXSS_VM_MODE_INITRD_NAME L" " TEXT(WSLC_ROOT_INIT_ENV) L"=1 panic=-1";
    m_kernelCmdLine += std::format(L" nr_cpus={}", Settings->CpuCount);

    // Append common WSL kernel parameters (timesync, printk, page reporting).
    // Use a page reporting order of 5 (128KB) matching modern Windows builds.
    helpers::AppendCommonKernelCommandLine(m_kernelCmdLine, 5);

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
        m_vmId, m_vmExitEvent, true, false, L"", true /* always enable earlycon for OpenVMM */, std::move(dmesgOutputHandle));

    // OpenVMM always uses earlycon to capture kernel output from the very start
    // of boot via COM1, before the virtio console (hvc0) driver loads.
    m_kernelCmdLine += L" earlycon=uart8250,io,0x3f8,115200";

    m_kernelCmdLine += L" console=hvc0 debug";

    // Set up vsock bridge path for HvSocket emulation.
    // OpenVMM uses a Unix domain socket for the hybrid_vsock bridge.
    // The hybrid_vsock bridge appends "_<port>" (e.g. "_50000") to this path,
    // and Unix domain sockets have a 108-byte path limit on Windows.
    // The SYSTEM profile temp path is too long, so use a short fixed directory.
    auto vsockDir = std::filesystem::path(L"C:\\ProgramData\\wslc");
    std::filesystem::create_directories(vsockDir);
    // Use first 8 chars of the GUID to keep it short but unique.
    m_vsockPath = vsockDir / std::format(L"vm-{:.8}", m_vmIdString);

    // Set up the ttrpc socket path for runtime VM management.
    // OpenVMM will listen on this Unix domain socket for ttrpc RPCs.
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

        if (m_waitVmThread.joinable())
        {
            m_waitVmThread.join();
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

    // Create the Unix domain socket listener for the init connection BEFORE
    // launching openvmm. The guest's mini_init will connect to vsock port
    // 50000 immediately on boot, and OpenVMM's hybrid_vsock bridge will relay
    // that connection to the host.
    //
    // OpenVMM's hybrid_vsock uses the HvSocket GUID template to construct the
    // path: port 50000 (0xC350) becomes GUID 0000c350-facb-11e6-bd58-64006a7986d3,
    // and the bridge looks for <vsock_path>_<guid> on the host.
    auto portHex = std::format(L"{:08x}", LX_INIT_UTILITY_VM_INIT_PORT);
    m_initListenPath = std::format(L"{}_{}-facb-11e6-bd58-64006a7986d3", m_vsockPath.wstring(), portHex);
    DeleteFileW(m_initListenPath.c_str());

    m_initListenSocket = ::socket(AF_UNIX, SOCK_STREAM, 0);
    THROW_LAST_ERROR_IF(m_initListenSocket == INVALID_SOCKET);

    sockaddr_un initAddr{};
    initAddr.sun_family = AF_UNIX;
    auto narrowInitPath = wsl::shared::string::WideToMultiByte(m_initListenPath);
    THROW_HR_IF_MSG(E_INVALIDARG, narrowInitPath.size() >= sizeof(initAddr.sun_path),
        "vsock bridge path too long: %hs", narrowInitPath.c_str());
    memcpy(initAddr.sun_path, narrowInitPath.c_str(), narrowInitPath.size() + 1);

    THROW_LAST_ERROR_IF(bind(m_initListenSocket, reinterpret_cast<sockaddr*>(&initAddr), sizeof(initAddr)) == SOCKET_ERROR);
    THROW_LAST_ERROR_IF(listen(m_initListenSocket, 1) == SOCKET_ERROR);

    WSL_LOG("OpenVmmInitListenerReady", TraceLoggingValue(m_initListenPath.c_str(), "ListenPath"));

    // Create a Unix domain socket listener for crash dump collection.
    // Same hybrid_vsock pattern as the init connection, but for port 50005.
    auto crashPortHex = std::format(L"{:08x}", LX_INIT_UTILITY_VM_CRASH_DUMP_PORT);
    m_crashDumpListenPath = std::format(L"{}_{}-facb-11e6-bd58-64006a7986d3", m_vsockPath.wstring(), crashPortHex);
    DeleteFileW(m_crashDumpListenPath.c_str());

    m_crashDumpListenSocket = ::socket(AF_UNIX, SOCK_STREAM, 0);
    THROW_LAST_ERROR_IF(m_crashDumpListenSocket == INVALID_SOCKET);

    sockaddr_un crashAddr{};
    crashAddr.sun_family = AF_UNIX;
    auto narrowCrashPath = wsl::shared::string::WideToMultiByte(m_crashDumpListenPath);
    THROW_HR_IF_MSG(E_INVALIDARG, narrowCrashPath.size() >= sizeof(crashAddr.sun_path),
        "vsock bridge path too long: %hs", narrowCrashPath.c_str());
    memcpy(crashAddr.sun_path, narrowCrashPath.c_str(), narrowCrashPath.size() + 1);

    THROW_LAST_ERROR_IF(bind(m_crashDumpListenSocket, reinterpret_cast<sockaddr*>(&crashAddr), sizeof(crashAddr)) == SOCKET_ERROR);
    THROW_LAST_ERROR_IF(listen(m_crashDumpListenSocket, 1) == SOCKET_ERROR);

    WSL_LOG("OpenVmmCrashDumpListenerReady", TraceLoggingValue(m_crashDumpListenPath.c_str(), "ListenPath"));

    // Launch the openvmm process.
    LaunchOpenVmm();

    cleanupOnFailure.release();
}

std::wstring OpenVmmVirtualMachine::BuildCommandLine() const
{
    // Build the openvmm.exe command line.
    //
    // In ttrpc orchestration mode, the command line only specifies the ttrpc
    // socket path. All VM configuration (kernel, disks, memory, networking,
    // etc.) is sent via the CreateVM RPC after the ttrpc server is ready.
    // This matches the Kata Containers / vmservice workflow.

    std::wstring cmd = std::format(L"\"{}\"", m_openvmmPath.wstring());

    // ttrpc server mode — OpenVMM will listen on this Unix domain socket
    // for vmservice RPCs (CreateVM, ResumeVM, ModifyResource, etc.).
    cmd += std::format(L" --ttrpc \"{}\"", m_ttrpcSocketPath.wstring());

    return cmd;
}

// Build the ttrpc CreateVM configuration from the stored VM settings.
TtrpcClient::VmConfig OpenVmmVirtualMachine::BuildVmConfig() const
{
    TtrpcClient::VmConfig config;

    // Linux direct boot paths.
    config.KernelPath = wsl::shared::string::WideToMultiByte(m_kernelPath.wstring());
    config.InitrdPath = wsl::shared::string::WideToMultiByte(m_initrdPath.wstring());

    // Kernel command line — the server prepends "panic=-1 debug pci=off console=ttyS0 "
    // automatically via HyperVGen2LinuxDirect chipset type.
    config.KernelCmdLine = wsl::shared::string::WideToMultiByte(m_kernelCmdLine);

    // Memory (ensure 2MB granularity like HCS).
    // Cap at 4GB for the PoC — OpenVMM on WHP allocates guest RAM upfront.
    constexpr ULONG maxMemoryMbPoC = 4096;
    config.MemoryMb = std::min(m_memoryMb, maxMemoryMbPoC) & ~0x1;

    // Processor count.
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

    // Networking: virtio-net NIC with consomme NAT backend.
    // Consomme provides a self-contained userspace TCP/IP stack with DHCP,
    // so no host-side network setup is needed.
    if (m_networkingMode == WSLCNetworkingModeConsomme)
    {
        // Generate a deterministic NIC instance ID from the VM ID so it's
        // stable across restarts but unique per VM.
        GUID nicGuid = m_vmId;
        nicGuid.Data1 ^= 0x4E494300; // "NIC\0" — distinguish from VM GUID

        config.Nic = TtrpcClient::VmConfig::ConsommeNic{
            .NicId = wsl::shared::string::GuidToString<char>(nicGuid),
            .MacAddress = "00-15-5D-00-00-01",
        };
    }

    // Serial ports for dmesg collection.
    // Early boot console: COM1 (port 0) connected to the DmesgCollector's early console pipe.
    // Always enabled for OpenVMM to capture output before hvc0 driver loads.
    config.SerialPorts.push_back({
        .Port = 0,
        .SocketPath = wsl::shared::string::WideToMultiByte(m_dmesgCollector->EarlyConsoleName()),
    });

    // Virtio serial console (/dev/hvc0) connected to the DmesgCollector's virtio console pipe.
    config.VirtioConsolePath = wsl::shared::string::WideToMultiByte(m_dmesgCollector->VirtioConsoleName());

    return config;
}

void OpenVmmVirtualMachine::LaunchOpenVmm()
{
    auto cmd = BuildCommandLine();

    WSL_LOG("LaunchOpenVmm", TraceLoggingValue(cmd.c_str(), "cmd"));

    SubProcess process(m_openvmmPath.c_str(), cmd.c_str());

    // Redirect stdout and stderr to a log file for diagnostics.
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE}; // bInheritHandle = TRUE
    auto logPath = m_vsockPath.wstring() + L".log";
    wil::unique_hfile logFile{CreateFileW(
        logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)};

    process.SetStdHandles(nullptr, logFile.get(), logFile.get());

    // Start the process. The returned handle is the process handle.
    m_processHandle = process.Start();

    // Assign the openvmm process to a kill-on-close job object. This ensures
    // the child process is terminated if the service exits without running our
    // destructor (e.g., killed or crashes).
    m_jobObject.reset(CreateJobObjectW(nullptr, nullptr));
    THROW_LAST_ERROR_IF(!m_jobObject);

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jobLimits{};
    jobLimits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    THROW_IF_WIN32_BOOL_FALSE(SetInformationJobObject(
        m_jobObject.get(), JobObjectExtendedLimitInformation, &jobLimits, sizeof(jobLimits)));
    THROW_IF_WIN32_BOOL_FALSE(AssignProcessToJobObject(m_jobObject.get(), m_processHandle.get()));

    // Close our copy of the log handle.
    logFile.reset();

    // Start a thread to watch for openvmm process exit.
    m_processWatchThread = std::thread(&OpenVmmVirtualMachine::WatchProcessExit, this);

    // Connect the ttrpc client. In --ttrpc mode, the server is ready almost
    // immediately (it only binds the socket, no VM setup yet). Connect()
    // retries with backoff internally.
    m_ttrpcClient = std::make_unique<TtrpcClient>();
    THROW_IF_FAILED_MSG(
        m_ttrpcClient->Connect(m_ttrpcSocketPath.wstring(), 30000),
        "Failed to connect to OpenVMM ttrpc server");

    // Send CreateVM with the full VM configuration (kernel, disks, memory, etc.).
    auto vmConfig = BuildVmConfig();
    THROW_IF_FAILED_MSG(
        m_ttrpcClient->CreateVm(vmConfig),
        "Failed to create VM via ttrpc CreateVM");

    // Resume the VM — CreateVM leaves it in a paused state.
    THROW_IF_FAILED_MSG(
        m_ttrpcClient->ResumeVm(),
        "Failed to resume VM via ttrpc ResumeVM");

    // Start a thread that calls WaitVM to detect when the guest halts.
    // This is analogous to HCS's OnVmExitCallback — when the guest performs
    // an ACPI S5 power-down, WaitVM returns and we signal m_vmExitEvent.
    // Uses a separate ttrpc connection because WaitVM blocks indefinitely and
    // SendRequest holds the client lock for the entire send+recv — a single
    // connection would deadlock all other RPCs (AttachDisk, etc.).
    m_waitVmThread = std::thread([this]() {
        TtrpcClient waitClient;
        auto hr = waitClient.Connect(m_ttrpcSocketPath.wstring(), 30000);
        if (SUCCEEDED(hr))
        {
            hr = waitClient.WaitVm();
        }

        if (SUCCEEDED(hr))
        {
            WSL_LOG("OpenVmmGuestHalted", TraceLoggingValue(m_vmIdString.c_str(), "VmId"));
        }
        else
        {
            WSL_LOG("OpenVmmWaitVmFailed",
                TraceLoggingValue(hr, "hr"),
                TraceLoggingValue(m_vmIdString.c_str(), "VmId"));
        }

        m_vmExitEvent.SetEvent();
    });
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

    // Ask openvmm to release all VM resources via TeardownVM.
    // This unblocks WaitVM and allows the process to exit.
    if (m_ttrpcClient)
    {
        LOG_IF_FAILED(m_ttrpcClient->TeardownVm());
        m_ttrpcClient->Disconnect();
        m_ttrpcClient.reset();
    }

    // Wait up to 5 seconds for the openvmm process to exit gracefully.
    // If it doesn't, forcefully terminate it.
    if (m_processHandle)
    {
        if (WaitForSingleObject(m_processHandle.get(), 5000) == WAIT_TIMEOUT)
        {
            WSL_LOG("OpenVmmForceTerminate", TraceLoggingValue(m_vmIdString.c_str(), "VmId"));
            TerminateProcess(m_processHandle.get(), 1);
        }
    }

    if (m_processWatchThread.joinable())
    {
        m_processWatchThread.join();
    }

    if (m_waitVmThread.joinable())
    {
        m_waitVmThread.join();
    }

    // Join relay threads (they exit when the exit event is signaled
    // or when the sockets they relay between close).
    if (m_initRelayThread.joinable())
    {
        m_initRelayThread.join();
    }

    if (m_crashDumpRelayThread.joinable())
    {
        m_crashDumpRelayThread.join();
    }

    {
        std::lock_guard lock(m_relayLock);
        for (auto& t : m_relayThreads)
        {
            if (t.joinable())
            {
                t.join();
            }
        }
        m_relayThreads.clear();
    }

    // Clean up the init listener socket if still open.
    if (m_initListenSocket != INVALID_SOCKET)
    {
        closesocket(m_initListenSocket);
        m_initListenSocket = INVALID_SOCKET;
    }
    DeleteFileW(m_initListenPath.c_str());

    // Clean up the crash dump listener socket if still open.
    if (m_crashDumpListenSocket != INVALID_SOCKET)
    {
        closesocket(m_crashDumpListenSocket);
        m_crashDumpListenSocket = INVALID_SOCKET;
    }
    DeleteFileW(m_crashDumpListenPath.c_str());

    // Clean up the vsock and ttrpc socket files.
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

// Bidirectional relay between a Unix domain socket and a TCP socket using select().
// Runs until either socket closes or exitEvent is signaled.
// Takes ownership of both sockets and closes them on exit.
static void RelaySocketData(SOCKET unixSock, SOCKET tcpSock, HANDLE exitEvent)
{
    auto cleanup = wil::scope_exit([&] {
        closesocket(unixSock);
        closesocket(tcpSock);
    });

    char buffer[65536];

    while (true)
    {
        // Check if we should exit.
        if (WaitForSingleObject(exitEvent, 0) == WAIT_OBJECT_0)
        {
            break;
        }

        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(unixSock, &readSet);
        FD_SET(tcpSock, &readSet);

        timeval timeout{};
        timeout.tv_sec = 0;
        timeout.tv_usec = 100000; // 100ms poll

        int ready = select(0, &readSet, nullptr, nullptr, &timeout);
        if (ready == SOCKET_ERROR)
        {
            break;
        }

        if (ready == 0)
        {
            continue; // Timeout, loop and check exitEvent
        }

        // Unix -> TCP
        if (FD_ISSET(unixSock, &readSet))
        {
            int bytes = recv(unixSock, buffer, sizeof(buffer), 0);
            if (bytes <= 0)
            {
                break;
            }

            int sent = 0;
            while (sent < bytes)
            {
                int n = send(tcpSock, buffer + sent, bytes - sent, 0);
                if (n <= 0)
                {
                    return;
                }
                sent += n;
            }
        }

        // TCP -> Unix
        if (FD_ISSET(tcpSock, &readSet))
        {
            int bytes = recv(tcpSock, buffer, sizeof(buffer), 0);
            if (bytes <= 0)
            {
                break;
            }

            int sent = 0;
            while (sent < bytes)
            {
                int n = send(unixSock, buffer + sent, bytes - sent, 0);
                if (n <= 0)
                {
                    return;
                }
                sent += n;
            }
        }
    }
}

// Creates a TCP loopback socket pair and starts a relay thread between an
// AF_UNIX socket and the TCP server socket. Returns the TCP client socket
// (which supports overlapped I/O) and the relay thread. The relay thread
// takes ownership of both the Unix socket and the TCP server socket.
static std::pair<wil::unique_socket, std::thread> CreateRelayedSocket(
    _In_ SOCKET unixSock, _In_ HANDLE exitEvent)
{
    // Create a TCP loopback listener on an ephemeral port.
    SOCKET tcpListener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    THROW_LAST_ERROR_IF(tcpListener == INVALID_SOCKET);
    auto closeListener = wil::scope_exit([&] { closesocket(tcpListener); });

    sockaddr_in loopback{};
    loopback.sin_family = AF_INET;
    loopback.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    loopback.sin_port = 0; // Let OS pick a port
    THROW_LAST_ERROR_IF(bind(tcpListener, reinterpret_cast<sockaddr*>(&loopback), sizeof(loopback)) == SOCKET_ERROR);
    THROW_LAST_ERROR_IF(listen(tcpListener, 1) == SOCKET_ERROR);

    // Get the port that was assigned.
    sockaddr_in boundAddr{};
    int addrLen = sizeof(boundAddr);
    THROW_LAST_ERROR_IF(getsockname(tcpListener, reinterpret_cast<sockaddr*>(&boundAddr), &addrLen) == SOCKET_ERROR);

    // Connect a TCP client to the listener.
    SOCKET tcpClient = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    THROW_LAST_ERROR_IF(tcpClient == INVALID_SOCKET);
    auto closeClient = wil::scope_exit([&] { closesocket(tcpClient); });

    THROW_LAST_ERROR_IF(connect(tcpClient, reinterpret_cast<sockaddr*>(&boundAddr), sizeof(boundAddr)) == SOCKET_ERROR);

    // Accept the server-side connection.
    SOCKET tcpServer = accept(tcpListener, nullptr, nullptr);
    THROW_LAST_ERROR_IF(tcpServer == INVALID_SOCKET);

    closeListener.release();
    closesocket(tcpListener);

    // Start a relay thread that takes ownership of unixSock and tcpServer.
    auto relayThread = std::thread(RelaySocketData, unixSock, tcpServer, exitEvent);

    // Return the TCP client socket — this supports overlapped I/O.
    closeClient.release();
    return {wil::unique_socket(tcpClient), std::move(relayThread)};
}

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

    // AF_UNIX sockets don't support overlapped I/O (WSARecv with OVERLAPPED),
    // which SocketChannel requires. Bridge via a TCP loopback relay.
    auto [tcpSocket, relayThread] = CreateRelayedSocket(unixSock, m_vmExitEvent.get());
    m_initRelayThread = std::move(relayThread);
    *Socket = reinterpret_cast<HANDLE>(tcpSocket.release());
    return S_OK;
}
CATCH_RETURN()

HRESULT OpenVmmVirtualMachine::ConfigureNetworking(_In_ HANDLE GnsSocket, _In_opt_ HANDLE* DnsSocket)
try
{
    std::lock_guard lock(m_lock);

    // The OpenVMM backend only supports consomme networking.
    WI_ASSERT(m_networkingMode == WSLCNetworkingModeConsomme);
    THROW_HR_IF(E_INVALIDARG, m_networkingMode != WSLCNetworkingModeConsomme);

    // In ttrpc orchestration mode, consomme networking is configured
    // server-side via the ConsommeBackend in NICConfig. No host-side
    // networking engine is needed — this call is a no-op.
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

    // TODO: Implement share hot-add.
    //
    // This requires extending OpenVMM's ModifyResource proto to support
    // Plan9/VirtioFS share hot-add. Currently, shares can only be configured
    // at VM creation time via --virtio-9p or --virtio-fs CLI args.
    //
    // For the PoC, pre-configure commonly needed shares at boot time by
    // adding --virtio-9p or --virtio-fs arguments in BuildCommandLine().
    //
    // Future work:
    // 1. Extend vmservice.proto with Plan9Config/VirtioFSConfig in ModifyResourceRequest
    // 2. Implement handler in openvmm_entry/src/ttrpc/mod.rs
    // 3. Call ModifyResource(ADD, plan9_config) from here

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

    // TODO: Implement share hot-remove. See AddShare comments.

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

    // AF_UNIX sockets don't support overlapped I/O (WSARecv with OVERLAPPED),
    // which SocketChannel requires. Bridge via a TCP loopback relay.
    closeUnix.release();
    auto [tcpSocket, relayThread] = CreateRelayedSocket(unixSock, m_vmExitEvent.get());
    {
        std::lock_guard lock(m_relayLock);
        m_relayThreads.emplace_back(std::move(relayThread));
    }
    *Socket = reinterpret_cast<HANDLE>(tcpSocket.release());
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

    // AF_UNIX sockets don't support overlapped I/O (WSARecv with OVERLAPPED),
    // which SocketChannel requires. Bridge via a TCP loopback relay.
    auto [tcpSocket, relayThread] = CreateRelayedSocket(unixSock, m_vmExitEvent.get());
    m_crashDumpRelayThread = std::move(relayThread);
    *Socket = reinterpret_cast<HANDLE>(tcpSocket.release());
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
