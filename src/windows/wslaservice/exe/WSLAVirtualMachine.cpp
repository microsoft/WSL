/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLAVirtualMachine.cpp

Abstract:

    Client-side class for WSLA virtual machine operations.

    The VM is created via IWSLAVirtualMachine (running in the SYSTEM service).
    This class connects to the existing VM for unprivileged operations
    and delegates privileged operations back to IWSLAVirtualMachine.

--*/

#include "WSLAVirtualMachine.h"
#include <format>
#include <filesystem>
#include "ServiceProcessLauncher.h"
#include "GuestDeviceManager.h"
#include "wslutil.h"
#include "lxinitshared.h"

using namespace wsl::windows::common;
using wsl::windows::service::wsla::WSLAProcess;
using wsl::windows::service::wsla::WSLAVirtualMachine;
namespace wslutil = wsl::windows::common::wslutil;

constexpr auto CONTAINER_PORT_RANGE = std::pair<uint16_t, uint16_t>(20002, 65535);

static_assert(c_ephemeralPortRange.second < CONTAINER_PORT_RANGE.first);

WSLAVirtualMachine::WSLAVirtualMachine(_In_ IWSLAVirtualMachine* Vm, _In_ const WSLA_SESSION_INIT_SETTINGS* Settings) :
    m_vm(Vm),
    m_featureFlags(static_cast<WSLAFeatureFlags>(Settings->FeatureFlags)),
    m_networkingMode(Settings->NetworkingMode),
    m_bootTimeoutMs(Settings->BootTimeoutMs),
    m_rootVhdType(Settings->RootVhdTypeOverride ? Settings->RootVhdTypeOverride : "ext4")
{
    THROW_IF_FAILED(m_vm->GetId(&m_vmId));

    // Establish a socket channel with mini_init in the VM.
    wil::unique_socket socket;
    THROW_IF_FAILED(m_vm->AcceptConnection(reinterpret_cast<HANDLE*>(&socket)));

    m_initChannel = wsl::shared::SocketChannel{std::move(socket), "mini_init", m_vmTerminatingEvent.get()};

    // Create a thread to watch for exited processes.
    auto [__, ___, childChannel] = Fork(WSLA_FORK::Thread);

    WSLA_WATCH_PROCESSES watchMessage{};
    childChannel.SendMessage(watchMessage);

    THROW_HR_IF(E_FAIL, childChannel.ReceiveMessage<RESULT_MESSAGE<uint32_t>>().Result != 0);

    m_processExitThread = std::thread(std::bind(&WSLAVirtualMachine::WatchForExitedProcesses, this, std::move(childChannel)));

    // Configure networking
    ConfigureNetworking();

    // Configure initial mounts
    ConfigureInitialMounts();

    // Configure GPU mounts if enabled
    MountGpuLibraries("/usr/lib/wsl/lib", "/usr/lib/wsl/drivers");

    // Configure cold discard hint size for page reporting.
    // This sets the minimum order of pages that will be reported as free to the hypervisor.
    {
        const auto windowsVersion = wsl::windows::common::helpers::GetWindowsVersion();
        int pageReportingOrder = (windowsVersion.BuildNumber >= wsl::windows::common::helpers::WindowsBuildNumbers::Germanium) ? 5 : 9; // 128k or 2MB
        auto cmdStr = std::format("echo {} > /sys/module/page_reporting/parameters/page_reporting_order", pageReportingOrder);
        std::vector<const char*> args{"/bin/sh", "-c", cmdStr.c_str()};

        auto [_, __, channel] = Fork(m_initChannel, WSLA_FORK::Process);

        wsl::shared::MessageWriter<WSLA_EXEC> execMessage;
        execMessage.WriteString(execMessage->ExecutableIndex, "/bin/sh");
        execMessage.WriteString(execMessage->CurrentDirectoryIndex, "/");
        execMessage.WriteStringArray(execMessage->CommandLineIndex, args.data(), static_cast<ULONG>(args.size()));
        execMessage.WriteStringArray(execMessage->EnvironmentIndex, nullptr, 0);

        channel.SendMessage<WSLA_EXEC>(execMessage.Span());
        ExpectClosedChannelOrError(channel);
    }
}

WSLAVirtualMachine::~WSLAVirtualMachine()
{
    WSL_LOG("WSLATerminateVmStart");

    m_vmTerminatingEvent.SetEvent();

    m_initChannel.Close();

    // Terminate the VM.
    m_vm.reset();

    if (m_processExitThread.joinable())
    {
        m_processExitThread.join();
    }

    // Clear the state of all remaining processes now that the VM has exited.
    for (auto& e : m_trackedProcesses)
    {
        e->OnVmTerminated();
    }
}

void WSLAVirtualMachine::ConfigureInitialMounts()
{
    // Get paths for root and modules VHDs
    auto basePath = wslutil::GetBasePath();

#ifdef WSL_KERNEL_MODULES_PATH
    auto kernelModulesPath = std::filesystem::path(TEXT(WSL_KERNEL_MODULES_PATH));
#else
    auto kernelModulesPath = basePath / L"tools" / L"modules.vhd";
#endif

    // Determine device paths based on whether pmem VHDs are used
    std::string rootDevice;
    std::string modulesDevice;

    if (!FeatureEnabled(WslaFeatureFlagsPmemVhds))
    {
        // SCSI attached disks - query device paths from guest
        rootDevice = GetVhdDevicePath(0);
        modulesDevice = GetVhdDevicePath(1);
    }
    else
    {
        // PMEM devices - use known device paths
        rootDevice = "/dev/pmem0";
        modulesDevice = "/dev/pmem1";
    }

    // Mount root filesystem with overlay
    Mount(m_initChannel, rootDevice.c_str(), "/mnt", m_rootVhdType.c_str(), "ro", WSLA_MOUNT::Chroot | WSLA_MOUNT::OverlayFs);

    // Mount standard filesystems
    Mount(m_initChannel, nullptr, "/dev", "devtmpfs", "", 0);
    Mount(m_initChannel, nullptr, "/sys", "sysfs", "", 0);
    Mount(m_initChannel, nullptr, "/proc", "proc", "", 0);
    Mount(m_initChannel, nullptr, "/dev/pts", "devpts", "noatime,nosuid,noexec,gid=5,mode=620", 0);

    // Mount kernel modules
    Mount(m_initChannel, modulesDevice.c_str(), "", "ext4", "ro", WSLA_MOUNT::KernelModules);

    // Mount cgroup
    Mount(m_initChannel, nullptr, "/sys/fs/cgroup", "cgroup2", "", 0);
}

void WSLAVirtualMachine::ConfigureNetworking()
{
    if (m_networkingMode == WSLANetworkingModeNone)
    {
        return;
    }

    // Fork to launch /gns
    auto [pid, _, gnsChannel] = Fork(m_initChannel, WSLA_FORK::Process);

    // Allocate sockets for GNS channel (and DNS channel if enabled)
    auto gnsSocket = ConnectSocket(gnsChannel, -1);

    ConnectedSocket dnsSocket;
    bool enableDnsTunneling = FeatureEnabled(WslaFeatureFlagsDnsTunneling);
    if (enableDnsTunneling)
    {
        dnsSocket = ConnectSocket(gnsChannel, -1);
    }

    // Build command line for /gns
    std::vector<const char*> cmd{"/gns", LX_INIT_GNS_SOCKET_ARG};
    std::string gnsSocketFdArg = std::to_string(gnsSocket.Fd);
    cmd.push_back(gnsSocketFdArg.c_str());

    std::string dnsSocketFdArg;
    if (enableDnsTunneling)
    {
        dnsSocketFdArg = std::to_string(dnsSocket.Fd);
        cmd.push_back(LX_INIT_GNS_DNS_SOCKET_ARG);
        cmd.push_back(dnsSocketFdArg.c_str());
        cmd.push_back(LX_INIT_GNS_DNS_TUNNELING_IP);
        cmd.push_back(LX_INIT_DNS_TUNNELING_IP_ADDRESS);
    }

    // Send WSLA_EXEC to launch /gns via /init
    wsl::shared::MessageWriter<WSLA_EXEC> execMessage;
    execMessage.WriteString(execMessage->ExecutableIndex, "/init");
    execMessage.WriteString(execMessage->CurrentDirectoryIndex, "/");
    execMessage.WriteStringArray(execMessage->CommandLineIndex, cmd.data(), static_cast<ULONG>(cmd.size()));
    execMessage.WriteStringArray(execMessage->EnvironmentIndex, nullptr, 0);

    gnsChannel.SendMessage<WSLA_EXEC>(execMessage.Span());

    // Wait for exec to complete - on success the channel closes, on failure we get an error
    auto execResult = ExpectClosedChannelOrError(gnsChannel);
    THROW_HR_IF_MSG(E_FAIL, execResult != 0, "exec /gns failed with errno: %d", execResult);

    // Call back to the service to configure the networking engine
    // The service takes ownership of the sockets via system_handle marshalling
    HANDLE gnsSocketHandle = reinterpret_cast<HANDLE>(gnsSocket.Socket.release());
    HANDLE dnsSocketHandle = enableDnsTunneling ? reinterpret_cast<HANDLE>(dnsSocket.Socket.release()) : nullptr;
    HANDLE* dnsSocketPtr = enableDnsTunneling ? &dnsSocketHandle : nullptr;
    THROW_IF_FAILED(m_vm->ConfigureNetworking(gnsSocketHandle, dnsSocketPtr));

    // Launch port relay for port forwarding
    LaunchPortRelay();
}

bool WSLAVirtualMachine::FeatureEnabled(WSLAFeatureFlags Value) const
{
    return static_cast<ULONG>(m_featureFlags) & static_cast<ULONG>(Value);
}

void WSLAVirtualMachine::WatchForExitedProcesses(wsl::shared::SocketChannel& Channel)
try
{
    // TODO: Terminate the VM if this thread exits unexpectedly.
    while (true)
    {
        auto [message, _] = Channel.ReceiveMessageOrClosed<WSLA_PROCESS_EXITED>();
        if (message == nullptr)
        {
            break; // Channel has been closed, exit
        }

        WSL_LOG(
            "ProcessExited",
            TraceLoggingValue(message->Pid, "Pid"),
            TraceLoggingValue(message->Code, "Code"),
            TraceLoggingValue(message->Signaled, "Signaled"));

        // Signal the exited process, if it's been monitored.
        {
            std::lock_guard lock{m_trackedProcessesLock};

            bool found = false;
            for (auto& e : m_trackedProcesses)
            {
                if (e->GetPid() == message->Pid)
                {
                    WI_ASSERT(!found);

                    try
                    {
                        e->OnExited(message->Signaled ? 128 + message->Code : message->Code);
                    }
                    CATCH_LOG();

                    found = true;
                }
            }
        }
    }
}
CATCH_LOG();

std::pair<ULONG, std::string> WSLAVirtualMachine::AttachDisk(_In_ PCWSTR Path, _In_ BOOL ReadOnly)
{
    ULONG Lun{};
    std::string Device;

    // Delegate to IWSLAVirtualMachine for the privileged HCS operation
    THROW_IF_FAILED(m_vm->AttachDisk(Path, ReadOnly, &Lun));

    // Query the guest for the device path
    Device = GetVhdDevicePath(Lun);

    WSL_LOG(
        "WSLAAttachDisk",
        TraceLoggingValue(Path, "Path"),
        TraceLoggingValue(ReadOnly, "ReadOnly"),
        TraceLoggingValue(Device.c_str(), "Device"),
        TraceLoggingValue(Lun, "Lun"));

    m_attachedDisks.emplace(Lun, AttachedDisk{Path, Device});

    return {Lun, Device};
}

void WSLAVirtualMachine::Unmount(_In_ const char* Path)
{
    auto [pid, _, subChannel] = Fork(WSLA_FORK::Thread);

    wsl::shared::MessageWriter<WSLA_UNMOUNT> message;
    message.WriteString(Path);

    const auto& response = subChannel.Transaction<WSLA_UNMOUNT>(message.Span());

    // TODO: Return errno to caller
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), response.Result == EINVAL);
    THROW_HR_IF(E_FAIL, response.Result != 0);
}

void WSLAVirtualMachine::DetachDisk(_In_ ULONG Lun)
{
    std::lock_guard lock{m_lock};

    // Find the disk
    auto it = m_attachedDisks.find(Lun);
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), it == m_attachedDisks.end());

    // Detach it from the guest
    WSLA_DETACH message;
    message.Lun = Lun;
    const auto& response = m_initChannel.Transaction(message);

    // TODO: Return errno to caller
    THROW_HR_IF(E_FAIL, response.Result != 0);

    // Remove it from the VM
    m_attachedDisks.erase(it);

    THROW_IF_FAILED(m_vm->DetachDisk(Lun));
}

std::tuple<int32_t, int32_t, wsl::shared::SocketChannel> WSLAVirtualMachine::Fork(enum WSLA_FORK::ForkType Type)
{
    std::lock_guard lock{m_lock};
    return Fork(m_initChannel, Type);
}

std::tuple<int32_t, int32_t, wsl::shared::SocketChannel> WSLAVirtualMachine::Fork(
    wsl::shared::SocketChannel& Channel, enum WSLA_FORK::ForkType Type, ULONG TtyRows, ULONG TtyColumns)
{
    uint32_t port{};
    int32_t pid{};
    int32_t ptyMaster{};
    {
        WSLA_FORK message;
        message.ForkType = Type;
        message.TtyColumns = static_cast<uint16_t>(TtyColumns);
        message.TtyRows = static_cast<uint16_t>(TtyRows);
        const auto& response = Channel.Transaction(message);
        port = response.Port;
        pid = response.Pid;
        ptyMaster = response.PtyMasterFd;
    }

    THROW_HR_IF_MSG(E_FAIL, pid <= 0, "fork() returned %i", pid);

    auto socket = wsl::windows::common::hvsocket::Connect(m_vmId, port, m_vmTerminatingEvent.get(), m_bootTimeoutMs);

    return std::make_tuple(pid, ptyMaster, wsl::shared::SocketChannel{std::move(socket), std::to_string(pid), m_vmTerminatingEvent.get()});
}

WSLAVirtualMachine::ConnectedSocket WSLAVirtualMachine::ConnectSocket(wsl::shared::SocketChannel& Channel, int32_t Fd)
{
    WSLA_ACCEPT message{};
    message.Fd = Fd;
    const auto& response = Channel.Transaction(message);

    ConnectedSocket socket;
    socket.Socket = wsl::windows::common::hvsocket::Connect(m_vmId, response.Result);

    // If the FD was unspecified, read the Linux file descriptor from the guest.
    if (Fd == -1)
    {
        socket.Fd = Channel.ReceiveMessage<RESULT_MESSAGE<int32_t>>().Result;
    }
    else
    {
        socket.Fd = Fd;
    }

    return socket;
}

std::string WSLAVirtualMachine::GetVhdDevicePath(ULONG Lun)
{
    WSLA_GET_DISK message{};
    message.Header.MessageSize = sizeof(message);
    message.Header.MessageType = WSLA_GET_DISK::Type;
    message.ScsiLun = Lun;
    const auto& response = m_initChannel.Transaction(message);
    THROW_HR_IF_MSG(E_FAIL, response.Result != 0, "Failed to get disk path, init returned: %lu", response.Result);

    return response.Buffer;
}

Microsoft::WRL::ComPtr<WSLAProcess> WSLAVirtualMachine::CreateLinuxProcess(
    _In_ LPCSTR Executable, _In_ const WSLA_PROCESS_OPTIONS& Options, int* Errno, const TPrepareCommandLine& PrepareCommandLine)
{
    // Check if this is a tty or not
    std::vector<WSLAProcessFd> fds;
    if (WI_IsFlagSet(Options.Flags, WSLAProcessFlagsTty))
    {
        fds.emplace_back(WSLAProcessFd{.Fd = WSLAFDTty, .Type = WSLAFdType::WSLAFdTypeTty});
        fds.emplace_back(WSLAProcessFd{.Fd = 0, .Type = WSLAFdType::WSLAFdTypeTtyControl});
    }
    else
    {
        if (WI_IsFlagSet(Options.Flags, WSLAProcessFlagsStdin))
        {
            fds.emplace_back(WSLAProcessFd{.Fd = WSLAFDStdin, .Type = WSLAFdType::WSLAFdTypeDefault});
        }

        fds.emplace_back(WSLAProcessFd{.Fd = WSLAFDStdout, .Type = WSLAFdType::WSLAFdTypeDefault});
        fds.emplace_back(WSLAProcessFd{.Fd = WSLAFDStderr, .Type = WSLAFdType::WSLAFdTypeDefault});
    }

    return CreateLinuxProcessImpl(Executable, Options, fds, Errno, PrepareCommandLine);
}

Microsoft::WRL::ComPtr<WSLAProcess> WSLAVirtualMachine::CreateLinuxProcessImpl(
    LPCSTR Executable, const WSLA_PROCESS_OPTIONS& Options, const std::vector<WSLAProcessFd>& Fds, int* Errno, const TPrepareCommandLine& PrepareCommandLine)
{
    // N.B This check is there to prevent processes from being started before the VM is done initializing.
    // to avoid potential deadlocks, since the processExitThread is required to signal the process exit events.
    // std::thread::joinable() is const, so this can be called without acquiring the lock.
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_processExitThread.joinable());

    THROW_WIN32_IF_MSG(
        ERROR_NOT_SUPPORTED, Options.User != nullptr, "Custom users are not supported for root namespace processes");

    auto setErrno = [Errno](int Error) {
        if (Errno != nullptr)
        {
            *Errno = Error;
        }
    };

    // Check if this is a tty or not
    const WSLAProcessFd* tty = nullptr;
    const WSLAProcessFd* ttyControl = nullptr;
    auto [pid, _, childChannel] = Fork(WSLA_FORK::Process);

    std::vector<WSLAVirtualMachine::ConnectedSocket> sockets;
    for (const auto& e : Fds)
    {
        if (e.Type == WSLAFdTypeTty)
        {
            THROW_HR_IF_MSG(E_INVALIDARG, tty != nullptr, "Multiple terminal fds specified");
            tty = &e;
        }
        else if (e.Type == WSLAFdTypeTtyControl)
        {
            THROW_HR_IF_MSG(E_INVALIDARG, ttyControl != nullptr, "Multiple terminal control fds specified");
            ttyControl = &e;
        }

        sockets.emplace_back(ConnectSocket(childChannel, static_cast<int32_t>(e.Fd)));
    }

    PrepareCommandLine(sockets);

    wsl::shared::MessageWriter<WSLA_EXEC> Message;

    Message.WriteString(Message->ExecutableIndex, Executable);
    Message.WriteString(Message->CurrentDirectoryIndex, Options.CurrentDirectory ? Options.CurrentDirectory : "/");
    Message.WriteStringArray(Message->CommandLineIndex, Options.CommandLine.Values, Options.CommandLine.Count);
    Message.WriteStringArray(Message->EnvironmentIndex, Options.Environment.Values, Options.Environment.Count);

    // If this is an interactive tty, we need a relay process
    if (tty != nullptr)
    {
        auto [grandChildPid, ptyMaster, grandChildChannel] = Fork(childChannel, WSLA_FORK::Pty, Options.TtyRows, Options.TtyColumns);
        WSLA_TTY_RELAY relayMessage{};
        relayMessage.TtyMaster = ptyMaster;
        relayMessage.Socket = tty->Fd;
        relayMessage.TtyControl = ttyControl == nullptr ? -1 : ttyControl->Fd;
        childChannel.SendMessage(relayMessage);

        auto result = ExpectClosedChannelOrError(childChannel);
        if (result != 0)
        {
            setErrno(result);
            THROW_HR_MSG(E_FAIL, "errno: %i", result);
        }

        grandChildChannel.SendMessage<WSLA_EXEC>(Message.Span());
        result = ExpectClosedChannelOrError(grandChildChannel);
        if (result != 0)
        {
            setErrno(result);
            THROW_HR_MSG(E_FAIL, "errno: %i", result);
        }

        pid = grandChildPid;
    }
    else
    {
        childChannel.SendMessage<WSLA_EXEC>(Message.Span());
        auto result = ExpectClosedChannelOrError(childChannel);
        if (result != 0)
        {
            setErrno(result);
            THROW_HR_MSG(E_FAIL, "errno: %i", result);
        }
    }

    wil::unique_socket ttyControlHandle;

    std::map<ULONG, wil::unique_handle> stdHandles;
    for (auto& [fd, handle] : sockets)
    {
        if (ttyControl != nullptr && fd == ttyControl->Fd)
        {
            ttyControlHandle = std::move(handle);
            continue;
        }

        stdHandles.emplace(fd, reinterpret_cast<HANDLE>(handle.release()));
    }

    auto io = std::make_unique<VMProcessIO>(std::move(stdHandles));
    auto control = std::make_unique<VMProcessControl>(*this, pid, std::move(ttyControlHandle));

    {
        std::lock_guard lock{m_trackedProcessesLock};
        m_trackedProcesses.emplace_back(control.get());
    }

    auto process = wil::MakeOrThrow<WSLAProcess>(std::move(control), std::move(io));

    setErrno(0);

    return process;
}

void WSLAVirtualMachine::Mount(LPCSTR Source, LPCSTR Target, LPCSTR Type, LPCSTR Options, ULONG Flags)
{
    std::lock_guard lock{m_lock};

    Mount(m_initChannel, Source, Target, Type, Options, Flags);
}

void WSLAVirtualMachine::Mount(shared::SocketChannel& Channel, LPCSTR Source, LPCSTR Target, LPCSTR Type, LPCSTR Options, ULONG Flags)
{
    static_assert(WSLAMountFlagsNone == WSLA_MOUNT::None);
    static_assert(WSLAMountFlagsReadOnly == WSLA_MOUNT::ReadOnly);
    static_assert(WSLAMountFlagsChroot == WSLA_MOUNT::Chroot);
    static_assert(WSLAMountFlagsWriteableOverlayFs == WSLA_MOUNT::OverlayFs);

    wsl::shared::MessageWriter<WSLA_MOUNT> message;

    auto optionalAdd = [&](auto value, unsigned int& index) {
        if (value != nullptr)
        {
            message.WriteString(index, value);
        }
    };

    optionalAdd(Source, message->SourceIndex);
    optionalAdd(Target, message->DestinationIndex);
    optionalAdd(Type, message->TypeIndex);
    optionalAdd(Options, message->OptionsIndex);
    message->Flags = Flags;

    const auto& response = Channel.Transaction<WSLA_MOUNT>(message.Span());

    WSL_LOG(
        "WSLAMount",
        TraceLoggingValue(Source == nullptr ? "<null>" : Source, "Source"),
        TraceLoggingValue(Target == nullptr ? "<null>" : Target, "Target"),
        TraceLoggingValue(Type == nullptr ? "<null>" : Type, "Type"),
        TraceLoggingValue(Options == nullptr ? "<null>" : Options, "Options"),
        TraceLoggingValue(Flags, "Flags"),
        TraceLoggingValue(response.Result, "Result"));

    THROW_HR_IF(E_FAIL, response.Result != 0);
}

int32_t WSLAVirtualMachine::ExpectClosedChannelOrError(wsl::shared::SocketChannel& Channel)
{
    auto [response, span] = Channel.ReceiveMessageOrClosed<RESULT_MESSAGE<int32_t>>();
    if (response != nullptr)
    {
        return response->Result;
    }
    else
    {
        return 0;
    }
}

void WSLAVirtualMachine::Signal(_In_ LONG Pid, _In_ int Signal)
{
    std::lock_guard lock(m_lock);

    WSLA_SIGNAL message;
    message.Pid = Pid;
    message.Signal = Signal;
    const auto& response = m_initChannel.Transaction(message);

    THROW_HR_IF(E_FAIL, response.Result != 0);
}

void WSLAVirtualMachine::LaunchPortRelay()
{
    WI_ASSERT(!m_portRelayChannelRead);

    auto [_, __, channel] = Fork(WSLA_FORK::ForkType::Process);

    std::lock_guard lock(m_portRelaylock);
    auto relayPort = channel.Transaction<WSLA_PORT_RELAY>();

    wil::unique_handle readPipe;
    wil::unique_handle writePipe;
    THROW_IF_WIN32_BOOL_FALSE(CreatePipe(&readPipe, &m_portRelayChannelWrite, nullptr, 0));
    THROW_IF_WIN32_BOOL_FALSE(CreatePipe(&m_portRelayChannelRead, &writePipe, nullptr, 0));

    // TODO: move the port relay infra into this process. Create a thread, pass handle ownership to thread, refactor and remove
    // wslrelaymode. wsl::windows::wslrelay::localhost::RunWSLAPortRelay(
    //     readPipe.release(), writePipe.release(), m_vmId, relayPort.Result, m_vmTerminatingEvent.get());

    wsl::windows::common::helpers::SetHandleInheritable(readPipe.get());
    wsl::windows::common::helpers::SetHandleInheritable(writePipe.get());
    wsl::windows::common::helpers::SetHandleInheritable(m_vmTerminatingEvent.get());

    auto path = wsl::windows::common::wslutil::GetBasePath() / L"wslrelay.exe";

    auto cmd = std::format(
        L"\"{}\" {} {} {} {} {} {} {} {}",
        path,
        wslrelay::mode_option,
        static_cast<int>(wslrelay::RelayMode::WSLAPortRelay),
        wslrelay::exit_event_option,
        HandleToUlong(m_vmTerminatingEvent.get()),
        wslrelay::port_option,
        relayPort.Result,
        wslrelay::vm_id_option,
        m_vmId);

    WSL_LOG("LaunchWslRelay", TraceLoggingValue(cmd.c_str(), "cmd"));

    wsl::windows::common::SubProcess process{nullptr, cmd.c_str()};
    process.SetStdHandles(readPipe.get(), writePipe.get(), nullptr);
    process.Start();

    readPipe.release();
    writePipe.release();
}

void WSLAVirtualMachine::MapPortImpl(_In_ int Family, _In_ short WindowsPort, _In_ short LinuxPort, _In_ bool Remove)
{
    std::lock_guard lock(m_portRelaylock);

    THROW_HR_IF(E_ILLEGAL_STATE_CHANGE, !m_portRelayChannelWrite);

    WSLA_MAP_PORT message;
    message.WindowsPort = WindowsPort;
    message.LinuxPort = LinuxPort;
    message.AddressFamily = Family;
    message.Stop = Remove;

    DWORD bytesTransfered{};
    THROW_IF_WIN32_BOOL_FALSE(WriteFile(m_portRelayChannelWrite.get(), &message, sizeof(message), &bytesTransfered, nullptr));
    THROW_HR_IF_MSG(E_UNEXPECTED, bytesTransfered != sizeof(message), "%u bytes transfered", bytesTransfered);

    HRESULT result = E_UNEXPECTED;
    THROW_IF_WIN32_BOOL_FALSE(ReadFile(m_portRelayChannelRead.get(), &result, sizeof(result), &bytesTransfered, nullptr));

    THROW_HR_IF(E_UNEXPECTED, bytesTransfered != sizeof(result));
    THROW_IF_FAILED_MSG(result, "Failed to map port: WindowsPort=%d, LinuxPort=%d, Family=%d, Remove=%d", WindowsPort, LinuxPort, Family, Remove);
}

void WSLAVirtualMachine::MapPort(_In_ int Family, _In_ short WindowsPort, _In_ short LinuxPort)
{
    MapPortImpl(Family, WindowsPort, LinuxPort, false);
}

void WSLAVirtualMachine::UnmapPort(_In_ int Family, _In_ short WindowsPort, _In_ short LinuxPort)
{
    MapPortImpl(Family, WindowsPort, LinuxPort, true);
}

HRESULT WSLAVirtualMachine::MountWindowsFolder(_In_ LPCWSTR WindowsPath, _In_ LPCSTR LinuxPath, _In_ BOOL ReadOnly)
{
    return MountWindowsFolderImpl(WindowsPath, LinuxPath, ReadOnly ? WSLAMountFlagsReadOnly : WSLAMountFlagsNone);
}

HRESULT WSLAVirtualMachine::MountWindowsFolderImpl(_In_ LPCWSTR WindowsPath, _In_ LPCSTR LinuxPath, _In_ WSLAMountFlags Flags)
try
{
    std::filesystem::path path(WindowsPath);
    THROW_HR_IF_MSG(E_INVALIDARG, !path.is_absolute(), "Path is not absolute: '%ls'", WindowsPath);
    THROW_HR_IF_MSG(
        HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND), !std::filesystem::is_directory(path), "Path is not a directory: '%ls'", WindowsPath);

    GUID shareGuid{};
    {
        std::lock_guard lock(m_lock);

        // Verify that this folder isn't already mounted.
        auto it = m_mountedWindowsFolders.find(LinuxPath);
        THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), it != m_mountedWindowsFolders.end());

        // Delegate to IWSLAVirtualMachine for the privileged share creation
        THROW_IF_FAILED(m_vm->AddShare(WindowsPath, WI_IsFlagSet(Flags, WSLAMountFlagsReadOnly), &shareGuid));

        m_mountedWindowsFolders.emplace(LinuxPath, shareGuid);
    }

    auto deleteOnFailure = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
        std::lock_guard lock(m_lock);
        auto mountIt = m_mountedWindowsFolders.find(LinuxPath);
        if (WI_VERIFY(mountIt != m_mountedWindowsFolders.end()))
        {
            m_mountedWindowsFolders.erase(mountIt);
            LOG_IF_FAILED(m_vm->RemoveShare(shareGuid));
        }
    });

    // Create the guest mount
    auto shareName = shared::string::GuidToString<char>(shareGuid, shared::string::None);
    if (!FeatureEnabled(WslaFeatureFlagsVirtioFs))
    {
        auto [_, __, channel] = Fork(WSLA_FORK::Process);

        WSLA_CONNECT message;
        message.HostPort = LX_INIT_UTILITY_VM_PLAN9_PORT;

        auto fd = channel.Transaction(message).Result;
        THROW_HR_IF_MSG(E_FAIL, fd < 0, "WSLA_CONNECT failed with %i", fd);

        auto mountOptions = std::format(
            "{},msize={},trans=fd,rfdno={},wfdno={},aname={},cache=mmap",
            WI_IsFlagSet(Flags, WSLAMountFlagsReadOnly) ? "ro" : "rw",
            LX_INIT_UTILITY_VM_PLAN9_BUFFER_SIZE,
            fd,
            fd,
            shareName);

        Mount(channel, shareName.c_str(), LinuxPath, "9p", mountOptions.c_str(), Flags);
    }
    else
    {
        std::string options = WI_IsFlagSet(Flags, WSLAMountFlagsReadOnly) ? "ro" : "rw";
        Mount(m_initChannel, shareName.c_str(), LinuxPath, "virtiofs", options.c_str(), Flags);
    }

    deleteOnFailure.release();

    return S_OK;
}
CATCH_RETURN();

HRESULT WSLAVirtualMachine::UnmountWindowsFolder(_In_ LPCSTR LinuxPath)
{
    std::lock_guard lock(m_lock);

    // Verify that this folder is mounted.
    auto it = m_mountedWindowsFolders.find(LinuxPath);
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), it == m_mountedWindowsFolders.end());

    // Unmount the folder from the guest.
    auto result = wil::ResultFromException([&]() { Unmount(LinuxPath); });
    THROW_HR_IF(result, FAILED(result) && result != HRESULT_FROM_WIN32(ERROR_NOT_FOUND));

    auto shareId = it->second;
    m_mountedWindowsFolders.erase(it);

    // Delegate to IWSLAVirtualMachine for the privileged share removal
    THROW_IF_FAILED(m_vm->RemoveShare(shareId));

    return S_OK;
}

void WSLAVirtualMachine::MountGpuLibraries(_In_ LPCSTR LibrariesMountPoint, _In_ LPCSTR DriversMountpoint)
{
    if (!FeatureEnabled(WslaFeatureFlagsGPU))
    {
        return;
    }

    auto windowsPath = wil::GetWindowsDirectoryW<std::wstring>();

    // Mount drivers.
    THROW_IF_FAILED(MountWindowsFolderImpl(
        std::format(L"{}\\System32\\DriverStore\\FileRepository", windowsPath).c_str(), DriversMountpoint, WSLAMountFlagsReadOnly));

    // Mount the inbox libraries.
    auto inboxLibPath = std::format(L"{}\\System32\\lxss\\lib", windowsPath);
    std::optional<std::string> inboxLibMountPoint;
    if (std::filesystem::is_directory(inboxLibPath))
    {
        inboxLibMountPoint = std::format("{}/inbox", LibrariesMountPoint);
        THROW_IF_FAILED(MountWindowsFolderImpl(inboxLibPath.c_str(), inboxLibMountPoint->c_str(), WSLAMountFlagsReadOnly));
    }

    // Mount the packaged libraries.
#ifdef WSL_GPU_LIB_PATH

    auto packagedLibPath = std::filesystem::path(TEXT(WSL_GPU_LIB_PATH));

#else

    auto packagedLibPath = wslutil::GetBasePath() / L"lib";

#endif

    auto packagedLibMountPoint = std::format("{}/packaged", LibrariesMountPoint);
    THROW_IF_FAILED(MountWindowsFolderImpl(packagedLibPath.c_str(), packagedLibMountPoint.c_str(), WSLAMountFlagsReadOnly));

    // Mount an overlay containing both inbox and packaged libraries (the packaged mount takes precedence).
    std::string options = "lowerdir=" + packagedLibMountPoint;
    if (inboxLibMountPoint.has_value())
    {
        options += ":" + inboxLibMountPoint.value();
    }

    Mount(m_initChannel, "none", LibrariesMountPoint, "overlay", options.c_str(), 0);
}

void WSLAVirtualMachine::OnProcessReleased(int Pid)
{
    std::lock_guard lock{m_trackedProcessesLock};

    std::erase_if(m_trackedProcesses, [Pid](const auto* e) { return e->GetPid() == Pid; });
}

// TODO: Handle reservations per family.
bool WSLAVirtualMachine::TryAllocatePort(uint16_t Port)
{
    std::lock_guard lock{m_lock};

    WSL_LOG("AllocatePort", TraceLoggingValue(Port, "Port"));

    auto [_, inserted] = m_allocatedPorts.insert(Port);

    return inserted;
}

std::set<uint16_t> WSLAVirtualMachine::AllocatePorts(uint16_t Count)
{
    std::lock_guard lock{m_lock};

    std::set<uint16_t> allocatedRange;

    // Add ports to the allocated list until we have enough
    for (auto i = CONTAINER_PORT_RANGE.first; i <= CONTAINER_PORT_RANGE.second && allocatedRange.size() < Count; i++)
    {
        if (!m_allocatedPorts.contains(i))
        {
            WI_VERIFY(allocatedRange.insert(i).second);
        }
    }

    // Fail if we couldn't find enough free ports.
    THROW_HR_IF_MSG(
        HRESULT_FROM_WIN32(ERROR_NO_SYSTEM_RESOURCES),
        allocatedRange.size() < Count,
        "Failed to allocate %u ports, only %zu available",
        Count,
        allocatedRange.size());

    // Reserve the ports we found.
    m_allocatedPorts.insert(allocatedRange.begin(), allocatedRange.end());

    return allocatedRange;
}

void WSLAVirtualMachine::ReleasePorts(const std::set<uint16_t>& Ports)
{
    std::lock_guard lock{m_lock};

    for (const auto& port : Ports)
    {
        WSL_LOG("ReleasePort", TraceLoggingValue(port, "Port"));

        WI_VERIFY(m_allocatedPorts.erase(port) == 1);
    }
}

wil::unique_socket WSLAVirtualMachine::ConnectUnixSocket(const char* Path)
{
    auto [_, __, channel] = Fork(WSLA_FORK::Thread);

    shared::MessageWriter<WSLA_UNIX_CONNECT> message;
    message.WriteString(message->PathOffset, Path);

    auto result = channel.Transaction<WSLA_UNIX_CONNECT>(message.Span());

    THROW_HR_IF_MSG(E_FAIL, result.Result < 0, "Failed to connect to unix socket: '%hs', %i", Path, result.Result);

    return channel.Release();
}
