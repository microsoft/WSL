/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCVirtualMachine.cpp

Abstract:

    Client-side class for WSLC virtual machine operations.
    The VM is created via IWSLCVirtualMachine (running in the SYSTEM service).
    This class connects to the existing VM for unprivileged operations
    and delegates privileged operations back to IWSLCVirtualMachine.

--*/

#include "WSLCVirtualMachine.h"
#include <format>
#include <filesystem>
#include "ServiceProcessLauncher.h"
#include "wslutil.h"
#include "lxinitshared.h"

using namespace wsl::windows::common;
using wsl::windows::service::wslc::TypedHandle;
using wsl::windows::service::wslc::VmPortAllocation;
using wsl::windows::service::wslc::VMPortMapping;
using wsl::windows::service::wslc::WSLCProcess;
using wsl::windows::service::wslc::WSLCVirtualMachine;
namespace wslutil = wsl::windows::common::wslutil;

constexpr auto CONTAINER_PORT_RANGE = std::pair<uint16_t, uint16_t>(20002, 65535);

static_assert(c_ephemeralPortRange.second < CONTAINER_PORT_RANGE.first);

VmPortAllocation::VmPortAllocation(uint16_t port, int family, int protocol, WSLCVirtualMachine& vm) :
    m_port(port), m_family(family), m_protocol(protocol), m_vm(&vm)
{
}

VmPortAllocation::VmPortAllocation(VmPortAllocation&& Other)
{
    *this = std::move(Other);
}

VmPortAllocation& VmPortAllocation::operator=(VmPortAllocation&& Other)
{
    if (this != &Other)
    {
        Reset();
        m_port = Other.m_port;
        m_family = Other.m_family;
        m_protocol = Other.m_protocol;
        m_vm = Other.m_vm;

        Other.Release();
    }
    return *this;
}

VmPortAllocation::~VmPortAllocation()
{
    Reset();
}

void VmPortAllocation::Reset()
{
    if (m_vm != nullptr)
    {
        m_vm->ReleasePort(*this);
        Release();
    }
}

void VmPortAllocation::Release()
{
    m_vm = nullptr;
    m_port = 0;
    m_family = 0;
    m_protocol = 0;
}

uint16_t VmPortAllocation::Port() const
{
    return m_port;
}

int VmPortAllocation::Family() const
{
    return m_family;
}

int VmPortAllocation::Protocol() const
{
    return m_protocol;
}

VMPortMapping::VMPortMapping(int protocol, int Family, uint16_t Port, const char* Address) : Protocol(protocol)
{
    THROW_HR_IF_MSG(E_INVALIDARG, Protocol != IPPROTO_TCP && Protocol != IPPROTO_UDP, "Invalid protocol: %i", Protocol);
    THROW_HR_IF(E_POINTER, Address == nullptr);
    if (Family == AF_INET)
    {
        common::wslutil::ParseIpv4Address(Address, BindAddress.Ipv4.sin_addr);
        BindAddress.Ipv4.sin_port = htons(Port);
    }
    else if (Family == AF_INET6)
    {
        common::wslutil::ParseIpv6Address(Address, BindAddress.Ipv6.sin6_addr);
        BindAddress.Ipv6.sin6_port = htons(Port);
    }
    else
    {
        THROW_HR_MSG(E_INVALIDARG, "Invalid address family: %i", Family);
    }

    // Must be assigned after parsing is done, since inet_pton writes to the family field as well.
    BindAddress.si_family = Family;
}

VMPortMapping::~VMPortMapping()
{
    try
    {
        Unmap();
    }
    CATCH_LOG();
}

VMPortMapping::VMPortMapping(VMPortMapping&& Other)
{
    *this = std::move(Other);
}

void VMPortMapping::AssignVmPort(const std::shared_ptr<VmPortAllocation>& Port)
{
    WI_ASSERT(!VmPort);

    VmPort = Port;
}

void VMPortMapping::Unmap()
{
    if (Vm)
    {
        Vm->UnmapPort(*this);
        Vm = nullptr;
    }
}

void VMPortMapping::Release()
{
    Vm = nullptr;
    VmPort.reset();
}

bool VMPortMapping::IsLocalhost() const
{
    if (BindAddress.Ipv4.sin_family == AF_INET6)
    {
        return IN6_IS_ADDR_LOOPBACK(&BindAddress.Ipv6.sin6_addr);
    }
    else
    {
        return IN4ADDR_ISLOOPBACK(&BindAddress.Ipv4);
    }
}

uint16_t VMPortMapping::HostPort() const
{
    if (BindAddress.si_family == AF_INET6)
    {
        return ntohs(BindAddress.Ipv6.sin6_port);
    }
    else
    {
        WI_ASSERT(BindAddress.si_family == AF_INET);
        return ntohs(BindAddress.Ipv4.sin_port);
    }
}

std::string VMPortMapping::BindingAddressString() const
{
    char buffer[INET6_ADDRSTRLEN]{};
    if (BindAddress.Ipv4.sin_family == AF_INET6)
    {
        THROW_LAST_ERROR_IF(inet_ntop(AF_INET6, &BindAddress.Ipv6.sin6_addr, buffer, sizeof(buffer)) == nullptr);
    }
    else
    {
        THROW_LAST_ERROR_IF(inet_ntop(AF_INET, &BindAddress.Ipv4.sin_addr, buffer, sizeof(buffer)) == nullptr);
    }

    return buffer;
}

void VMPortMapping::Attach(WSLCVirtualMachine& Vm)
{
    WI_ASSERT(this->Vm == nullptr);

    this->Vm = &Vm;
}

void VMPortMapping::Detach()
{
    WI_ASSERT(Vm != nullptr);

    this->Vm = nullptr;
}

VMPortMapping VMPortMapping::LocalhostTcpMapping(int Family, uint16_t WindowsPort)
{
    WI_ASSERT(Family == AF_INET || Family == AF_INET6);

    return VMPortMapping(IPPROTO_TCP, Family, WindowsPort, Family == AF_INET ? "127.0.0.1" : "::1");
}

VMPortMapping VMPortMapping::FromWSLCPortMapping(const ::WSLCPortMapping& Mapping)
{
    return VMPortMapping(Mapping.Protocol, Mapping.Family, Mapping.HostPort, Mapping.BindingAddress);
}

VMPortMapping VMPortMapping::FromContainerMetaData(const wslc::WSLCPortMapping& Mapping)
{
    return VMPortMapping(Mapping.Protocol, Mapping.Family, Mapping.HostPort, Mapping.BindingAddress.c_str());
}

VMPortMapping& VMPortMapping::operator=(VMPortMapping&& Other)
{
    if (this != &Other)
    {
        Unmap();
        Protocol = Other.Protocol;
        VmPort = std::move(Other.VmPort);
        BindAddress = Other.BindAddress;
        Vm = Other.Vm;

        Other.Protocol = 0;
        ZeroMemory(&Other.BindAddress, sizeof(Other.BindAddress));
        Other.Vm = nullptr;
    }
    return *this;
}

WSLCVirtualMachine::WSLCVirtualMachine(_In_ IWSLCVirtualMachine* Vm, _In_ const WSLCSessionInitSettings* Settings) :
    m_vm(Vm),
    m_featureFlags(static_cast<WSLCFeatureFlags>(Settings->FeatureFlags)),
    m_networkingMode(Settings->NetworkingMode),
    m_bootTimeoutMs(Settings->BootTimeoutMs),
    m_rootVhdType(Settings->RootVhdTypeOverride ? Settings->RootVhdTypeOverride : "ext4")
{
    // N.B. The constructor should not run any operation that could throw, so the destructor runs even if the VM fails to boot.
}

void WSLCVirtualMachine::Initialize()
{
    THROW_IF_FAILED(m_vm->GetId(&m_vmId));

    // Start crash dump collection thread.
    auto crashDumpSocket = hvsocket::Listen(m_vmId, LX_INIT_UTILITY_VM_CRASH_DUMP_PORT);
    THROW_LAST_ERROR_IF(!crashDumpSocket);

    m_crashDumpThread = std::thread{[this, socket = std::move(crashDumpSocket)]() mutable { CollectCrashDumps(std::move(socket)); }};

    // Establish a socket channel with mini_init in the VM.
    wil::unique_socket socket;
    THROW_IF_FAILED(m_vm->AcceptConnection(reinterpret_cast<HANDLE*>(&socket)));

    m_initChannel = wsl::shared::SocketChannel{std::move(socket), "mini_init", m_vmTerminatingEvent.get()};

    // Create a thread to watch for exited processes.
    auto [__, ___, childChannel] = Fork(WSLC_FORK::Thread);

    WSLC_WATCH_PROCESSES watchMessage{};
    childChannel.SendMessage(watchMessage);

    THROW_HR_IF(E_FAIL, childChannel.ReceiveMessage<RESULT_MESSAGE<uint32_t>>().Result != 0);

    m_processExitThread = std::thread(std::bind(&WSLCVirtualMachine::WatchForExitedProcesses, this, std::move(childChannel)));

    // Configure networking
    ConfigureNetworking();

    // Mount VHDs
    const auto rootDevice = GetVhdDevicePath(0);
    Mount(m_initChannel, rootDevice.c_str(), "/mnt", m_rootVhdType.c_str(), "ro", WSLC_MOUNT::Chroot | WSLC_MOUNT::OverlayFs);

    const auto modulesDevice = GetVhdDevicePath(1);
    Mount(m_initChannel, modulesDevice.c_str(), "", "ext4", "ro", WSLC_MOUNT::KernelModules);

    // Configure GPU mounts if enabled
    MountGpuLibraries("/usr/lib/wsl/lib", "/usr/lib/wsl/drivers");

    // Configure cold discard hint size for page reporting.
    // This sets the minimum order of pages that will be reported as free to the hypervisor.
    {
        const auto windowsVersion = wsl::windows::common::helpers::GetWindowsVersion();
        int pageReportingOrder = (windowsVersion.BuildNumber >= wsl::windows::common::helpers::WindowsBuildNumbers::Germanium) ? 5 : 9; // 128k or 2MB
        auto cmdStr = std::format("echo {} > /sys/module/page_reporting/parameters/page_reporting_order", pageReportingOrder);
        std::vector<const char*> args{"/bin/sh", "-c", cmdStr.c_str()};

        WSLCProcessOptions options{};
        options.CommandLine = {.Values = args.data(), .Count = static_cast<ULONG>(args.size())};
        CreateLinuxProcessImpl("/bin/sh", options, {}, nullptr, [](const auto&) {});
    }
}

WSLCVirtualMachine::~WSLCVirtualMachine()
{
    WSL_LOG("WSLCTerminateVmStart");

    m_vmTerminatingEvent.SetEvent();

    m_initChannel.Close();

    // Terminate the VM.
    m_vm.reset();

    if (m_processExitThread.joinable())
    {
        m_processExitThread.join();
    }

    if (m_crashDumpThread.joinable())
    {
        m_crashDumpThread.join();
    }

    // Clear the state of all remaining processes now that the VM has exited.
    for (auto& e : m_trackedProcesses)
    {
        if (auto locked = e.lock())
        {
            locked->OnVmTerminated();
        }
    }
}

void WSLCVirtualMachine::ConfigureNetworking()
{
    if (m_networkingMode == WSLCNetworkingModeNone)
    {
        return;
    }

    // Launch /gns with auto-allocated file descriptors for the GNS channel (and DNS channel if enabled).
    std::vector<WSLCProcessFd> fds;
    fds.emplace_back(WSLCProcessFd{.Fd = -1, .Type = WSLCFdType::WSLCFdTypeDefault});

    bool enableDnsTunneling = FeatureEnabled(WslcFeatureFlagsDnsTunneling);
    if (enableDnsTunneling)
    {
        fds.emplace_back(WSLCProcessFd{.Fd = -1, .Type = WSLCFdType::WSLCFdTypeDefault});
    }

    // Because the file descriptor numbers aren't known in advance, the command line needs to be generated after the
    // file descriptors are allocated.
    std::vector<const char*> cmd{"/gns", LX_INIT_GNS_SOCKET_ARG};
    std::string gnsSocketFdArg;
    std::string dnsSocketFdArg;
    int gnsChannelFd = -1;
    int dnsChannelFd = -1;

    WSLCProcessOptions options{};
    auto prepareCommandLine = [&](const auto& sockets) {
        gnsChannelFd = sockets[0].Fd;
        gnsSocketFdArg = std::to_string(gnsChannelFd);
        cmd.push_back(gnsSocketFdArg.c_str());

        if (enableDnsTunneling)
        {
            dnsChannelFd = sockets[1].Fd;
            dnsSocketFdArg = std::to_string(dnsChannelFd);
            cmd.push_back(LX_INIT_GNS_DNS_SOCKET_ARG);
            cmd.push_back(dnsSocketFdArg.c_str());
            cmd.push_back(LX_INIT_GNS_DNS_TUNNELING_IP);
            cmd.push_back(LX_INIT_DNS_TUNNELING_IP_ADDRESS);
        }

        options.CommandLine = {.Values = cmd.data(), .Count = static_cast<ULONG>(cmd.size())};
    };

    auto process = CreateLinuxProcessImpl("/init", options, fds, nullptr, prepareCommandLine);

    // Call back to the service to configure the networking engine.
    auto gnsHandle = process->GetStdHandle(gnsChannelFd);

    wil::unique_handle dnsHandle;
    HANDLE dnsSocketHandle = nullptr;
    if (enableDnsTunneling)
    {
        dnsHandle = process->GetStdHandle(dnsChannelFd);
        dnsSocketHandle = dnsHandle.get();
    }

    THROW_IF_FAILED(m_vm->ConfigureNetworking(gnsHandle.get(), enableDnsTunneling ? &dnsSocketHandle : nullptr));

    // Launch port relay for port forwarding
    LaunchPortRelay();
}

bool WSLCVirtualMachine::FeatureEnabled(WSLCFeatureFlags Value) const
{
    return static_cast<ULONG>(m_featureFlags) & static_cast<ULONG>(Value);
}

void WSLCVirtualMachine::WatchForExitedProcesses(wsl::shared::SocketChannel& Channel)
try
{
    // TODO: Terminate the VM if this thread exits unexpectedly.
    while (true)
    {
        auto [message, _] = Channel.ReceiveMessageOrClosed<WSLC_PROCESS_EXITED>();
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
        // N.B. Lock weak_ptr under lock, then call OnExited outside it to avoid
        // deadlock with the destructor's m_lock -> m_trackedProcessesLock ordering.
        std::shared_ptr<VMProcessControl> exited;
        {
            std::lock_guard lock{m_trackedProcessesLock};

            for (auto& e : m_trackedProcesses)
            {
                auto locked = e.lock();
                if (locked && locked->GetPid() == message->Pid)
                {
                    exited = std::move(locked);
                    break;
                }
            }
        }

        if (exited)
        {
            try
            {
                exited->OnExited(message->Signaled ? 128 + message->Code : message->Code);
            }
            CATCH_LOG();
        }
    }
}
CATCH_LOG();

std::pair<ULONG, std::string> WSLCVirtualMachine::AttachDisk(_In_ PCWSTR Path, _In_ BOOL ReadOnly)
{
    std::lock_guard lock{m_lock};

    ULONG Lun{};
    std::string Device;

    // Delegate to IWSLCVirtualMachine for the privileged HCS operation
    THROW_IF_FAILED(m_vm->AttachDisk(Path, ReadOnly, &Lun));

    // Detach on failure so the service-side state stays consistent.
    auto detachOnFailure = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() { LOG_IF_FAILED(m_vm->DetachDisk(Lun)); });

    // Query the guest for the device path
    Device = GetVhdDevicePath(Lun);

    WSL_LOG(
        "WSLCAttachDisk",
        TraceLoggingValue(Path, "Path"),
        TraceLoggingValue(ReadOnly, "ReadOnly"),
        TraceLoggingValue(Device.c_str(), "Device"),
        TraceLoggingValue(Lun, "Lun"));

    m_attachedDisks.emplace(Lun, AttachedDisk{Path, Device});

    detachOnFailure.release();

    return {Lun, Device};
}

void WSLCVirtualMachine::Ext4Format(const std::string& Device)
{
    constexpr auto mkfsPath = "/usr/sbin/mkfs.ext4";
    ServiceProcessLauncher launcher(mkfsPath, {mkfsPath, Device});
    auto result = launcher.Launch(*this).WaitAndCaptureOutput();

    THROW_HR_IF_MSG(E_FAIL, result.Code != 0, "%hs", launcher.FormatResult(result).c_str());
}

void WSLCVirtualMachine::Unmount(_In_ const char* Path)
{
    auto [pid, _, subChannel] = Fork(WSLC_FORK::Thread);

    wsl::shared::MessageWriter<WSLC_UNMOUNT> message;
    message.WriteString(Path);

    const auto& response = subChannel.Transaction<WSLC_UNMOUNT>(message.Span());

    // TODO: Return errno to caller
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), response.Result == EINVAL);
    THROW_HR_IF(E_FAIL, response.Result != 0);
}

void WSLCVirtualMachine::DetachDisk(_In_ ULONG Lun)
{
    std::lock_guard lock{m_lock};

    // Find the disk
    auto it = m_attachedDisks.find(Lun);
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), it == m_attachedDisks.end());

    // Detach it from the guest
    WSLC_DETACH message;
    message.Lun = Lun;
    const auto& response = m_initChannel.Transaction(message);

    // TODO: Return errno to caller
    THROW_HR_IF(E_FAIL, response.Result != 0);

    // Remove it from the VM
    THROW_IF_FAILED(m_vm->DetachDisk(Lun));

    m_attachedDisks.erase(it);
}

std::tuple<int32_t, int32_t, wsl::shared::SocketChannel> WSLCVirtualMachine::Fork(enum WSLC_FORK::ForkType Type)
{
    std::lock_guard lock{m_lock};
    return Fork(m_initChannel, Type);
}

std::tuple<int32_t, int32_t, wsl::shared::SocketChannel> WSLCVirtualMachine::Fork(
    wsl::shared::SocketChannel& Channel, enum WSLC_FORK::ForkType Type, ULONG TtyRows, ULONG TtyColumns)
{
    uint32_t port{};
    int32_t pid{};
    int32_t ptyMaster{};
    {
        WSLC_FORK message;
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

WSLCVirtualMachine::ConnectedSocket WSLCVirtualMachine::ConnectSocket(wsl::shared::SocketChannel& Channel, int32_t Fd)
{
    WSLC_ACCEPT message{};
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

std::string WSLCVirtualMachine::GetVhdDevicePath(ULONG Lun)
{
    WSLC_GET_DISK message{};
    message.Header.MessageSize = sizeof(message);
    message.Header.MessageType = WSLC_GET_DISK::Type;
    message.ScsiLun = Lun;
    const auto& response = m_initChannel.Transaction(message);
    THROW_HR_IF_MSG(E_FAIL, response.Result != 0, "Failed to get disk path, init returned: %lu", response.Result);

    return response.Buffer;
}

Microsoft::WRL::ComPtr<WSLCProcess> WSLCVirtualMachine::CreateLinuxProcess(
    _In_ LPCSTR Executable, _In_ const WSLCProcessOptions& Options, int* Errno, const TPrepareCommandLine& PrepareCommandLine)
{
    // Check if this is a tty or not
    std::vector<WSLCProcessFd> fds;
    if (WI_IsFlagSet(Options.Flags, WSLCProcessFlagsTty))
    {
        fds.emplace_back(WSLCProcessFd{.Fd = WSLCFDTty, .Type = WSLCFdType::WSLCFdTypeTty});
        fds.emplace_back(WSLCProcessFd{.Fd = 0, .Type = WSLCFdType::WSLCFdTypeTtyControl});
    }
    else
    {
        if (WI_IsFlagSet(Options.Flags, WSLCProcessFlagsStdin))
        {
            fds.emplace_back(WSLCProcessFd{.Fd = WSLCFDStdin, .Type = WSLCFdType::WSLCFdTypeDefault});
        }

        fds.emplace_back(WSLCProcessFd{.Fd = WSLCFDStdout, .Type = WSLCFdType::WSLCFdTypeDefault});
        fds.emplace_back(WSLCProcessFd{.Fd = WSLCFDStderr, .Type = WSLCFdType::WSLCFdTypeDefault});
    }

    return CreateLinuxProcessImpl(Executable, Options, fds, Errno, PrepareCommandLine);
}

Microsoft::WRL::ComPtr<WSLCProcess> WSLCVirtualMachine::CreateLinuxProcessImpl(
    LPCSTR Executable, const WSLCProcessOptions& Options, const std::vector<WSLCProcessFd>& Fds, int* Errno, const TPrepareCommandLine& PrepareCommandLine)
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
    const WSLCProcessFd* tty = nullptr;
    const WSLCProcessFd* ttyControl = nullptr;
    auto [pid, _, childChannel] = Fork(WSLC_FORK::Process);

    std::vector<WSLCVirtualMachine::ConnectedSocket> sockets;
    for (const auto& e : Fds)
    {
        if (e.Type == WSLCFdTypeTty)
        {
            THROW_HR_IF_MSG(E_INVALIDARG, tty != nullptr, "Multiple terminal fds specified");
            tty = &e;
        }
        else if (e.Type == WSLCFdTypeTtyControl)
        {
            THROW_HR_IF_MSG(E_INVALIDARG, ttyControl != nullptr, "Multiple terminal control fds specified");
            ttyControl = &e;
        }

        sockets.emplace_back(ConnectSocket(childChannel, static_cast<int32_t>(e.Fd)));
    }

    PrepareCommandLine(sockets);

    wsl::shared::MessageWriter<WSLC_EXEC> Message;

    Message.WriteString(Message->ExecutableIndex, Executable);
    Message.WriteString(Message->CurrentDirectoryIndex, Options.CurrentDirectory ? Options.CurrentDirectory : "/");
    Message.WriteStringArray(Message->CommandLineIndex, Options.CommandLine.Values, Options.CommandLine.Count);
    Message.WriteStringArray(Message->EnvironmentIndex, Options.Environment.Values, Options.Environment.Count);

    // If this is an interactive tty, we need a relay process
    if (tty != nullptr)
    {
        auto [grandChildPid, ptyMaster, grandChildChannel] = Fork(childChannel, WSLC_FORK::Pty, Options.TtyRows, Options.TtyColumns);
        WSLC_TTY_RELAY relayMessage{};
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

        grandChildChannel.SendMessage<WSLC_EXEC>(Message.Span());
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
        childChannel.SendMessage<WSLC_EXEC>(Message.Span());
        auto result = ExpectClosedChannelOrError(childChannel);
        if (result != 0)
        {
            setErrno(result);
            THROW_HR_MSG(E_FAIL, "errno: %i", result);
        }
    }

    wil::unique_socket ttyControlHandle;

    std::map<ULONG, TypedHandle> stdHandles;
    for (auto& [fd, handle] : sockets)
    {
        if (ttyControl != nullptr && fd == ttyControl->Fd)
        {
            ttyControlHandle = std::move(handle);
            continue;
        }

        stdHandles.emplace(fd, TypedHandle{wil::unique_handle{reinterpret_cast<HANDLE>(handle.release())}, WSLCHandleTypeSocket});
    }

    auto io = std::make_unique<VMProcessIO>(std::move(stdHandles));
    auto control = std::make_shared<VMProcessControl>(*this, pid, std::move(ttyControlHandle));

    {
        std::lock_guard lock{m_trackedProcessesLock};
        m_trackedProcesses.emplace_back(control);
    }

    auto process = wil::MakeOrThrow<WSLCProcess>(std::move(control), std::move(io), Options.Flags);

    setErrno(0);

    return process;
}

void WSLCVirtualMachine::Mount(LPCSTR Source, LPCSTR Target, LPCSTR Type, LPCSTR Options, ULONG Flags)
{
    std::lock_guard lock{m_lock};

    Mount(m_initChannel, Source, Target, Type, Options, Flags);
}

void WSLCVirtualMachine::Mount(shared::SocketChannel& Channel, LPCSTR Source, LPCSTR Target, LPCSTR Type, LPCSTR Options, ULONG Flags)
{
    static_assert(WSLCMountFlagsNone == WSLC_MOUNT::None);
    static_assert(WSLCMountFlagsReadOnly == WSLC_MOUNT::ReadOnly);
    static_assert(WSLCMountFlagsChroot == WSLC_MOUNT::Chroot);
    static_assert(WSLCMountFlagsWriteableOverlayFs == WSLC_MOUNT::OverlayFs);

    wsl::shared::MessageWriter<WSLC_MOUNT> message;

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

    const auto& response = Channel.Transaction<WSLC_MOUNT>(message.Span());

    WSL_LOG(
        "WSLCMount",
        TraceLoggingValue(Source == nullptr ? "<null>" : Source, "Source"),
        TraceLoggingValue(Target == nullptr ? "<null>" : Target, "Target"),
        TraceLoggingValue(Type == nullptr ? "<null>" : Type, "Type"),
        TraceLoggingValue(Options == nullptr ? "<null>" : Options, "Options"),
        TraceLoggingValue(Flags, "Flags"),
        TraceLoggingValue(response.Result, "Result"));

    THROW_HR_IF(E_FAIL, response.Result != 0);
}

int32_t WSLCVirtualMachine::ExpectClosedChannelOrError(wsl::shared::SocketChannel& Channel)
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

void WSLCVirtualMachine::Signal(_In_ LONG Pid, _In_ int Signal)
{
    std::lock_guard lock(m_lock);

    WSLC_SIGNAL message;
    message.Pid = Pid;
    message.Signal = Signal;
    const auto& response = m_initChannel.Transaction(message);

    THROW_HR_IF(E_FAIL, response.Result != 0);
}

void WSLCVirtualMachine::LaunchPortRelay()
{
    WI_ASSERT(!m_portRelayChannelRead);

    auto [_, __, channel] = Fork(WSLC_FORK::ForkType::Process);

    std::lock_guard lock(m_portRelaylock);
    auto relayPort = channel.Transaction<WSLC_PORT_RELAY>();

    wil::unique_handle readPipe;
    wil::unique_handle writePipe;
    THROW_IF_WIN32_BOOL_FALSE(CreatePipe(&readPipe, &m_portRelayChannelWrite, nullptr, 0));
    THROW_IF_WIN32_BOOL_FALSE(CreatePipe(&m_portRelayChannelRead, &writePipe, nullptr, 0));

    // TODO: move the port relay infra into this process. Create a thread, pass handle ownership to thread, refactor and remove
    // wslrelaymode. wsl::windows::wslrelay::localhost::RunWSLCPortRelay(
    //     readPipe.release(), writePipe.release(), m_vmId, relayPort.Result, m_vmTerminatingEvent.get());

    wsl::windows::common::helpers::SetHandleInheritable(readPipe.get());
    wsl::windows::common::helpers::SetHandleInheritable(writePipe.get());
    wsl::windows::common::helpers::SetHandleInheritable(m_vmTerminatingEvent.get());

    auto path = wsl::windows::common::wslutil::GetBasePath() / L"wslrelay.exe";

    auto cmd = std::format(
        L"\"{}\" {} {} {} {} {} {} {} {}",
        path,
        wslrelay::mode_option,
        static_cast<int>(wslrelay::RelayMode::WSLCPortRelay),
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

void WSLCVirtualMachine::MapRelayPort(_In_ int Family, _In_ unsigned short WindowsPort, _In_ unsigned short LinuxPort, _In_ bool Remove)
{
    std::lock_guard lock(m_portRelaylock);

    THROW_HR_IF(E_ILLEGAL_STATE_CHANGE, !m_portRelayChannelWrite);

    WSLC_MAP_PORT message;
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

void WSLCVirtualMachine::MapPort(VMPortMapping& Mapping)
{
    THROW_HR_IF_MSG(E_INVALIDARG, !Mapping.VmPort, "Can't map a VM port without an allocated port");

    if (m_networkingMode == WSLCNetworkingModeNone)
    {
        THROW_HR_MSG(E_ILLEGAL_STATE_CHANGE, "Port mapping is not supported with the current networking mode");
    }
    else if (m_networkingMode == WSLCNetworkingModeNAT)
    {
        THROW_HR_IF_MSG(
            HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED),
            !Mapping.IsLocalhost() || Mapping.Protocol != IPPROTO_TCP,
            "Unsupported port mapping for NAT mode: %hs, protocol: %i",
            Mapping.BindingAddressString().c_str(),
            Mapping.Protocol);

        MapRelayPort(Mapping.BindAddress.si_family, Mapping.HostPort(), Mapping.VmPort->Port(), false);
    }
    else if (m_networkingMode == WSLCNetworkingModeVirtioProxy)
    {
        // TODO: Switch to using the native virtionet relay.
        THROW_HR_IF_MSG(
            HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED),
            !Mapping.IsLocalhost() || Mapping.Protocol != IPPROTO_TCP,
            "Unsupported port mapping for virtionet mode: %hs, protocol: %i",
            Mapping.BindingAddressString().c_str(),
            Mapping.Protocol);

        MapRelayPort(Mapping.BindAddress.si_family, Mapping.HostPort(), Mapping.VmPort->Port(), false);
    }
    else
    {
        THROW_HR_MSG(E_UNEXPECTED, "Unexpected networking mode: %i", m_networkingMode);
    }

    Mapping.Attach(*this);
}

void WSLCVirtualMachine::UnmapPort(VMPortMapping& Mapping)
{
    THROW_HR_IF_MSG(E_INVALIDARG, !Mapping.VmPort, "Can't unmap a VM port without an allocated port");

    if (m_networkingMode == WSLCNetworkingModeNone)
    {
        THROW_HR_MSG(E_ILLEGAL_STATE_CHANGE, "Port mapping is not supported with the current networking mode");
    }
    else if (m_networkingMode == WSLCNetworkingModeNAT)
    {
        MapRelayPort(Mapping.BindAddress.si_family, Mapping.HostPort(), Mapping.VmPort->Port(), true);
    }
    else if (m_networkingMode == WSLCNetworkingModeVirtioProxy)
    {
        // TODO: Switch to using the native virtionet relay.
        MapRelayPort(Mapping.BindAddress.si_family, Mapping.HostPort(), Mapping.VmPort->Port(), true);
    }
    else
    {
        THROW_HR_MSG(E_UNEXPECTED, "Unexpected networking mode: %i", m_networkingMode);
    }

    Mapping.Detach();
}

HRESULT WSLCVirtualMachine::MountWindowsFolder(_In_ LPCWSTR WindowsPath, _In_ LPCSTR LinuxPath, _In_ BOOL ReadOnly)
{
    return MountWindowsFolderImpl(WindowsPath, LinuxPath, ReadOnly ? WSLCMountFlagsReadOnly : WSLCMountFlagsNone);
}

HRESULT WSLCVirtualMachine::MountWindowsFolderImpl(_In_ LPCWSTR WindowsPath, _In_ LPCSTR LinuxPath, _In_ WSLCMountFlags Flags)
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

        // Delegate to IWSLCVirtualMachine for the privileged share creation
        THROW_IF_FAILED(m_vm->AddShare(WindowsPath, WI_IsFlagSet(Flags, WSLCMountFlagsReadOnly), &shareGuid));

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
    if (!FeatureEnabled(WslcFeatureFlagsVirtioFs))
    {
        auto [_, __, channel] = Fork(WSLC_FORK::Process);

        WSLC_CONNECT message;
        message.HostPort = LX_INIT_UTILITY_VM_PLAN9_PORT;

        auto fd = channel.Transaction(message).Result;
        THROW_HR_IF_MSG(E_FAIL, fd < 0, "WSLC_CONNECT failed with %i", fd);

        auto mountOptions = std::format(
            "{},msize={},trans=fd,rfdno={},wfdno={},aname={},cache=mmap",
            WI_IsFlagSet(Flags, WSLCMountFlagsReadOnly) ? "ro" : "rw",
            LX_INIT_UTILITY_VM_PLAN9_BUFFER_SIZE,
            fd,
            fd,
            shareName);

        Mount(channel, shareName.c_str(), LinuxPath, "9p", mountOptions.c_str(), Flags);
    }
    else
    {
        std::string options = WI_IsFlagSet(Flags, WSLCMountFlagsReadOnly) ? "ro" : "rw";
        Mount(m_initChannel, shareName.c_str(), LinuxPath, "virtiofs", options.c_str(), Flags);
    }

    deleteOnFailure.release();

    return S_OK;
}
CATCH_RETURN();

HRESULT WSLCVirtualMachine::UnmountWindowsFolder(_In_ LPCSTR LinuxPath)
try
{
    std::lock_guard lock(m_lock);

    // Verify that this folder is mounted.
    auto it = m_mountedWindowsFolders.find(LinuxPath);
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), it == m_mountedWindowsFolders.end());

    // Unmount the folder from the guest.
    auto result = wil::ResultFromException([&]() { Unmount(LinuxPath); });
    THROW_HR_IF(result, FAILED(result) && result != HRESULT_FROM_WIN32(ERROR_NOT_FOUND));

    auto shareId = it->second;

    // Delegate to IWSLCVirtualMachine for the privileged share removal
    THROW_IF_FAILED(m_vm->RemoveShare(shareId));

    m_mountedWindowsFolders.erase(it);

    return S_OK;
}
CATCH_RETURN();

void WSLCVirtualMachine::MountGpuLibraries(_In_ LPCSTR LibrariesMountPoint, _In_ LPCSTR DriversMountpoint)
{
    if (!FeatureEnabled(WslcFeatureFlagsGPU))
    {
        return;
    }

    auto windowsPath = wil::GetWindowsDirectoryW<std::wstring>();

    // Mount drivers.
    THROW_IF_FAILED(MountWindowsFolderImpl(
        std::format(L"{}\\System32\\DriverStore\\FileRepository", windowsPath).c_str(), DriversMountpoint, WSLCMountFlagsReadOnly));

    // Mount the inbox libraries.
    auto inboxLibPath = std::format(L"{}\\System32\\lxss\\lib", windowsPath);
    std::optional<std::string> inboxLibMountPoint;
    if (std::filesystem::is_directory(inboxLibPath))
    {
        inboxLibMountPoint = std::format("{}/inbox", LibrariesMountPoint);
        THROW_IF_FAILED(MountWindowsFolderImpl(inboxLibPath.c_str(), inboxLibMountPoint->c_str(), WSLCMountFlagsReadOnly));
    }

    // Mount the packaged libraries.
#ifdef WSL_GPU_LIB_PATH

    auto packagedLibPath = std::filesystem::path(TEXT(WSL_GPU_LIB_PATH));

#else

    auto packagedLibPath = wslutil::GetBasePath() / L"lib";

#endif

    auto packagedLibMountPoint = std::format("{}/packaged", LibrariesMountPoint);
    THROW_IF_FAILED(MountWindowsFolderImpl(packagedLibPath.c_str(), packagedLibMountPoint.c_str(), WSLCMountFlagsReadOnly));

    // Mount an overlay containing both inbox and packaged libraries (the packaged mount takes precedence).
    std::string options = "lowerdir=" + packagedLibMountPoint;
    if (inboxLibMountPoint.has_value())
    {
        options += ":" + inboxLibMountPoint.value();
    }

    Mount(m_initChannel, "none", LibrariesMountPoint, "overlay", options.c_str(), 0);
}

void WSLCVirtualMachine::OnProcessReleased(int Pid)
{
    std::lock_guard lock{m_trackedProcessesLock};

    std::erase_if(m_trackedProcesses, [Pid](const auto& e) {
        auto locked = e.lock();
        return !locked || locked->GetPid() == Pid;
    });
}

std::shared_ptr<VmPortAllocation> WSLCVirtualMachine::TryAllocatePort(uint16_t Port, int Family, int Protocol)
{
    std::lock_guard lock{m_lock};

    WSL_LOG("AllocatePort", TraceLoggingValue(Port, "Port"));

    auto [_, inserted] = m_allocatedPorts.insert(Port);

    if (inserted)
    {
        return std::make_shared<VmPortAllocation>(Port, Family, Protocol, *this);
    }
    else
    {
        return {};
    }
}

std::shared_ptr<VmPortAllocation> WSLCVirtualMachine::AllocatePort(int Family, int Protocol)
{
    std::lock_guard lock{m_lock};

    for (auto i = CONTAINER_PORT_RANGE.first; i <= CONTAINER_PORT_RANGE.second; i++)
    {
        if (!m_allocatedPorts.contains(i))
        {
            WI_VERIFY(m_allocatedPorts.insert(i).second);
            return std::make_shared<VmPortAllocation>(i, Family, Protocol, *this);
        }
    }

    // Fail if we couldn't find a port.
    THROW_HR_MSG(HRESULT_FROM_WIN32(ERROR_NO_SYSTEM_RESOURCES), "Failed to allocate port");
}

void WSLCVirtualMachine::ReleasePort(VmPortAllocation& Port)
{
    std::lock_guard lock{m_lock};

    WSL_LOG("ReleasePort", TraceLoggingValue(Port.Port(), "Port"));

    LOG_HR_IF(E_UNEXPECTED, m_allocatedPorts.erase(Port.Port()) != 1);
}

wil::unique_socket WSLCVirtualMachine::ConnectUnixSocket(const char* Path)
{
    auto [_, __, channel] = Fork(WSLC_FORK::Thread);

    shared::MessageWriter<WSLC_UNIX_CONNECT> message;
    message.WriteString(message->PathOffset, Path);

    auto result = channel.Transaction<WSLC_UNIX_CONNECT>(message.Span());

    THROW_HR_IF_MSG(E_FAIL, result.Result < 0, "Failed to connect to unix socket: '%hs', %i", Path, result.Result);

    return channel.Release();
}

void WSLCVirtualMachine::CollectCrashDumps(wil::unique_socket&& listenSocket)
{
    // No impersonation needed - the session process already runs as the user.
    wslutil::SetThreadDescription(L"CrashDumpCollection");

    const auto crashDumpFolder = filesystem::GetTempFolderPath(GetCurrentProcessToken()) / L"wslc-crashes";

    while (!m_vmTerminatingEvent.is_signaled())
    {
        try
        {
            auto socket = hvsocket::CancellableAccept(listenSocket.get(), INFINITE, m_vmTerminatingEvent.get());
            if (!socket)
            {
                // VM is exiting.
                break;
            }

            constexpr DWORD timeout = 30 * 1000;
            THROW_LAST_ERROR_IF(setsockopt(socket->get(), SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout)) == SOCKET_ERROR);

            auto channel = wsl::shared::SocketChannel{std::move(socket.value()), "crash_dump", m_vmTerminatingEvent.get()};

            const auto& message = channel.ReceiveMessage<LX_PROCESS_CRASH>();
            const char* process = reinterpret_cast<const char*>(&message.Buffer);

            constexpr auto dumpExtension = ".dmp";
            constexpr auto dumpPrefix = "wsl-crash";

            auto filename = std::format("{}-{}-{}-{}-{}{}", dumpPrefix, message.Timestamp, message.Pid, process, message.Signal, dumpExtension);

            std::replace_if(filename.begin(), filename.end(), [](auto e) { return !std::isalnum(e) && e != '.' && e != '-'; }, '_');

            auto fullPath = crashDumpFolder / filename;

            WSL_LOG(
                "WSLCLinuxCrash",
                TraceLoggingValue(fullPath.c_str(), "FullPath"),
                TraceLoggingValue(message.Pid, "Pid"),
                TraceLoggingValue(message.Signal, "Signal"),
                TraceLoggingValue(process, "process"));

            filesystem::EnsureDirectory(crashDumpFolder.c_str());

            // Only delete files that:
            // - have the temporary flag set
            // - start with 'wsl-crash'
            // - end in .dmp
            //
            // This logic is here to prevent accidental user file deletion
            auto pred = [&dumpExtension, &dumpPrefix](const auto& e) {
                return WI_IsFlagSet(GetFileAttributes(e.path().c_str()), FILE_ATTRIBUTE_TEMPORARY) && e.path().has_extension() &&
                       e.path().extension() == dumpExtension && e.path().has_filename() &&
                       e.path().filename().string().find(dumpPrefix) == 0;
            };

            wslutil::EnforceFileLimit(crashDumpFolder.c_str(), 10, pred);

            wil::unique_hfile file{CreateFileW(fullPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, nullptr)};
            THROW_LAST_ERROR_IF(!file);

            channel.SendResultMessage<std::int32_t>(0);
            relay::InterruptableRelay(reinterpret_cast<HANDLE>(channel.Socket()), file.get(), nullptr);
        }
        CATCH_LOG()
    }
}
