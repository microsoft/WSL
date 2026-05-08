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

using namespace wsl::windows::common;
using wsl::windows::service::wslc::OpenVmmVirtualMachine;
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

    if (FeatureEnabled(WslcFeatureFlagsEarlyBootDmesg))
    {
        m_kernelCmdLine += L" earlycon=uart8250,io,0x3f8,115200";
    }

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

    // Record the initial boot disks in the LUN bitmap (LUN 0 = root, LUN 1 = modules).
    auto rootLun = AllocateLun();
    WI_ASSERT(rootLun == 0);
    m_attachedDisks.emplace(rootLun, DiskInfo{m_rootVhdPath, true});

    auto modulesLun = AllocateLun();
    WI_ASSERT(modulesLun == 1);
    m_attachedDisks.emplace(modulesLun, DiskInfo{m_modulesVhdPath, true});

    // Pre-attach the storage disk at LUN 2 (read-write).
    if (!m_storageVhdPath.empty() && std::filesystem::exists(m_storageVhdPath))
    {
        auto storageLun = AllocateLun();
        WI_ASSERT(storageLun == 2);
        m_attachedDisks.emplace(storageLun, DiskInfo{m_storageVhdPath, false});
    }

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

    // Launch the openvmm process.
    LaunchOpenVmm();
}

std::wstring OpenVmmVirtualMachine::BuildCommandLine() const
{
    // Build the openvmm.exe command line.
    //
    // Key mappings from HCS settings to OpenVMM CLI:
    //   CPU count       → -p <count>
    //   Memory          → -m <size>MB
    //   Kernel          → -k <path>
    //   Initrd          → -r <path>
    //   Cmdline args    → -c <string>
    //   Boot disks      → --disk file:<path>,ro
    //   HvSocket bridge → --vmbus-vsock-path <path>
    //   Hypervisor      → --hv --hypervisor whp
    //   Serial console  → --com1 stderr (for dmesg)

    std::wstring cmd = std::format(L"\"{}\"", m_openvmmPath.wstring());

    // Processor count
    cmd += std::format(L" -p {}", m_cpuCount);

    // Memory (ensure 2MB granularity like HCS).
    // Cap at 4GB for the PoC — OpenVMM on WHP allocates guest RAM upfront
    // (unlike HCS which uses demand-paging), so large allocations will fail
    // with ERROR_NO_SYSTEM_RESOURCES.
    // TODO: Use shared memory mode (--shared-memory) or file-backed memory
    // to support larger guest RAM without upfront allocation.
    constexpr ULONG maxMemoryMbPoC = 4096;
    const ULONG alignedMemoryMb = std::min(m_memoryMb, maxMemoryMbPoC) & ~0x1;
    cmd += std::format(L" -m {}MB", alignedMemoryMb);

    // Linux direct boot
    cmd += std::format(L" -k \"{}\"", m_kernelPath.wstring());
    cmd += std::format(L" -r \"{}\"", m_initrdPath.wstring());

    // Kernel command line arguments (pass each word as a separate -c)
    cmd += std::format(L" -c \"{}\"", m_kernelCmdLine);

    // Boot disks: root VHD (LUN 0) and modules VHD (LUN 1), both read-only.
    // Must use the file: prefix so the path's drive letter colon (e.g. C:\...)
    // is not misinterpreted as a disk kind delimiter.
    cmd += std::format(L" --disk \"file:{},ro\"", m_rootVhdPath.wstring());
    cmd += std::format(L" --disk \"file:{},ro\"", m_modulesVhdPath.wstring());

    // Storage VHDX (LUN 2), read-write — used for container layers.
    if (!m_storageVhdPath.empty() && m_attachedDisks.contains(2))
    {
        cmd += std::format(L" --disk \"file:{}\"", m_storageVhdPath.wstring());
    }

    // Enable Hyper-V enlightenments and WHP hypervisor
    cmd += L" --hv";
    cmd += L" --hypervisor whp";

    // Halt on guest reset instead of rebooting (prevents panic reboot loops)
    cmd += L" --halt-on-reset";

    // Networking: use virtio-net frontend with consomme NAT backend.
    // virtio-net is preferred over netvsp (VMBus NIC) for Linux guests.
    // consomme provides a self-contained userspace TCP/IP stack with DHCP —
    // no host-side GNS daemon or network setup needed.
    cmd += L" --virtio-net consomme";

    // HvSocket bridge via vsock path
    cmd += std::format(L" --vmbus-vsock-path \"{}\"", m_vsockPath.wstring());

    // Serial console for dmesg output
    if (FeatureEnabled(WslcFeatureFlagsEarlyBootDmesg))
    {
        cmd += L" --com1 stderr";
    }

    // TODO: Configure networking (--net or --nic) based on m_networkingMode.
    // For now, the session process handles networking via GNS after boot.

    // TODO: GPU support (--device on Windows) if WslcFeatureFlagsGPU is set.
    // This requires OpenVMM Windows GPU assignment support which doesn't exist yet.

    return cmd;
}

void OpenVmmVirtualMachine::LaunchOpenVmm()
{
    auto cmd = BuildCommandLine();

    WSL_LOG("LaunchOpenVmm", TraceLoggingValue(cmd.c_str(), "cmd"));

    SubProcess process(m_openvmmPath.c_str(), cmd.c_str());

    // Create a pipe for stdin so we can send REPL commands (e.g., add-disk).
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE}; // bInheritHandle = TRUE
    wil::unique_hfile stdinRead;
    wil::unique_hfile stdinWrite;
    THROW_IF_WIN32_BOOL_FALSE(CreatePipe(&stdinRead, &stdinWrite, &sa, 0));

    // Only the read end should be inherited by the child.
    THROW_IF_WIN32_BOOL_FALSE(SetHandleInformation(stdinWrite.get(), HANDLE_FLAG_INHERIT, 0));

    // Redirect stdout and stderr to a log file for diagnostics.
    auto logPath = m_vsockPath.wstring() + L".log";
    wil::unique_hfile logFile{CreateFileW(
        logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)};

    process.SetStdHandles(stdinRead.get(), logFile.get(), logFile.get());

    // Start the process. The returned handle is the process handle.
    m_processHandle = process.Start();

    // Keep the write end of stdin for sending REPL commands.
    m_stdinWrite = std::move(stdinWrite);

    // Close our copies of the read end and log handle.
    stdinRead.reset();
    logFile.reset();

    // Start a thread to watch for openvmm process exit.
    m_processWatchThread = std::thread(&OpenVmmVirtualMachine::WatchProcessExit, this);

    // TODO: Wait for openvmm to signal readiness (e.g., vsock bridge is up).
    // For now, use a simple delay. A proper implementation would use a named
    // event or poll the vsock path until it exists.
    Sleep(m_bootTimeoutMs > 0 ? std::min(m_bootTimeoutMs, 5000UL) : 5000);
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

    // Terminate the openvmm process if still running.
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

    // Clean up the init listener socket if still open.
    if (m_initListenSocket != INVALID_SOCKET)
    {
        closesocket(m_initListenSocket);
        m_initListenSocket = INVALID_SOCKET;
    }
    DeleteFileW(m_initListenPath.c_str());

    // Clean up the vsock socket file.
    try
    {
        if (std::filesystem::exists(m_vsockPath))
        {
            std::filesystem::remove(m_vsockPath);
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

    // Return the AF_UNIX socket directly. SocketChannel will auto-detect
    // the socket type and use SyncSocketTransport for non-overlapped I/O.
    *Socket = reinterpret_cast<HANDLE>(unixSock);
    return S_OK;
}
CATCH_RETURN()

HRESULT OpenVmmVirtualMachine::ConfigureNetworking(_In_ HANDLE GnsSocket, _In_opt_ HANDLE* DnsSocket)
try
{
    std::lock_guard lock(m_lock);

    if (m_networkingMode == WSLCNetworkingModeNone)
    {
        return S_OK;
    }

    // TODO: Implement networking configuration for OpenVMM backend.
    //
    // The HCS implementation creates a NatNetworking or VirtioNetworking engine
    // that manages the guest's network stack. With OpenVMM, networking is
    // configured differently:
    //
    // Option A: Use OpenVMM's built-in consomme/tap networking backend,
    //   configured at VM creation time via --net or --nic CLI args.
    //
    // Option B: Pass the GNS/DNS sockets through to the VM via the vsock
    //   bridge, similar to how the session process does it today. This keeps
    //   the WSL service's networking architecture unchanged.
    //
    // For the PoC, we store the socket handles but don't configure networking.
    // The guest will boot without network connectivity.

    WSL_LOG(
        "OpenVmmConfigureNetworking",
        TraceLoggingValue(m_vmIdString.c_str(), "VmId"),
        TraceLoggingValue(static_cast<int>(m_networkingMode), "Mode"),
        TraceLoggingValue("SKIPPED", "Status"));

    // Return S_OK so session setup proceeds. The guest won't have networking.
    return S_OK;
}
CATCH_RETURN()

HRESULT OpenVmmVirtualMachine::AttachDisk(_In_ LPCWSTR Path, _In_ BOOL ReadOnly, _Out_ ULONG* Lun)
try
{
    RETURN_HR_IF(E_POINTER, Path == nullptr || Lun == nullptr);

    std::lock_guard lock(m_lock);

    // Check if this disk was pre-attached at boot time (e.g., storage.vhdx).
    // If so, return the pre-allocated LUN.
    std::filesystem::path requestedPath(Path);
    for (const auto& [lun, disk] : m_attachedDisks)
    {
        if (std::filesystem::weakly_canonical(requestedPath) == std::filesystem::weakly_canonical(std::filesystem::path(disk.Path)))
        {
            WSL_LOG(
                "OpenVmmAttachDiskPreAttached",
                TraceLoggingValue(m_vmIdString.c_str(), "VmId"),
                TraceLoggingValue(Path, "Path"),
                TraceLoggingValue(lun, "Lun"));

            *Lun = lun;
            return S_OK;
        }
    }

    // Hot-add is not supported in the PoC. Return ERROR_FILE_NOT_FOUND
    // if the file doesn't exist so ConfigureStorage can create it.
    // For files that exist but aren't pre-attached, return E_NOTIMPL.
    if (!std::filesystem::exists(Path))
    {
        return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    }

    WSL_LOG(
        "OpenVmmAttachDisk",
        TraceLoggingValue(m_vmIdString.c_str(), "VmId"),
        TraceLoggingValue(Path, "Path"),
        TraceLoggingValue(ReadOnly, "ReadOnly"),
        TraceLoggingValue("NOT_IMPLEMENTED", "Status"));

    return E_NOTIMPL;
}
CATCH_RETURN()

HRESULT OpenVmmVirtualMachine::DetachDisk(_In_ ULONG Lun)
try
{
    std::lock_guard lock(m_lock);

    // TODO: Implement runtime disk hot-remove via gRPC ModifyResource(REMOVE, SCSIDisk).
    // See AttachDisk comments for details.

    WSL_LOG(
        "OpenVmmDetachDisk",
        TraceLoggingValue(m_vmIdString.c_str(), "VmId"),
        TraceLoggingValue(Lun, "Lun"),
        TraceLoggingValue("NOT_IMPLEMENTED", "Status"));

    return E_NOTIMPL;
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

    // Return the AF_UNIX socket directly. SocketChannel will auto-detect
    // the socket type and use SyncSocketTransport for non-overlapped I/O.
    closeUnix.release();
    *Socket = reinterpret_cast<HANDLE>(unixSock);
    return S_OK;
}
CATCH_RETURN()

ULONG OpenVmmVirtualMachine::AllocateLun()
{
    for (ULONG i = 0; i < OPENVMM_MAX_VHD_COUNT; i++)
    {
        if (!m_lunBitmap.test(i))
        {
            m_lunBitmap.set(i);
            return i;
        }
    }

    THROW_HR(HRESULT_FROM_WIN32(ERROR_NO_SYSTEM_RESOURCES));
}

void OpenVmmVirtualMachine::FreeLun(ULONG Lun)
{
    WI_ASSERT(Lun < OPENVMM_MAX_VHD_COUNT);
    WI_ASSERT(m_lunBitmap.test(Lun));
    m_lunBitmap.reset(Lun);
}
