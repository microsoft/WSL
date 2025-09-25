/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLAInit.cpp

Abstract:

    Init implementation for WSLA.

--*/

#include "util.h"
#include "SocketChannel.h"
#include "message.h"
#include "localhost.h"
#include <utmp.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/syscall.h>
#include <sys/epoll.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#include <pty.h>
#include "mountutilcpp.h"
#include <filesystem>

extern int InitializeLogging(bool SetStderr, wil::LogFunction* ExceptionCallback) noexcept;

extern std::set<pid_t> ListInitChildProcesses();

extern std::vector<unsigned int> ListScsiDisks();

extern int DetachScsiDisk(unsigned int Lun);

extern std::string GetLunDeviceName(unsigned int Lun);

void ProcessMessages(wsl::shared::SocketChannel& Channel);
int MountInit(const char* Target);

extern int EnableInterface(int Socket, const char* Name);

extern int SetCloseOnExec(int Fd, bool Enable);

int Chroot(const char* Target);

extern int g_LogFd;

void HandleMessageImpl(wsl::shared::SocketChannel& Channel, const WSLA_GET_DISK& Message, const gsl::span<gsl::byte>& Buffer)
{
    wsl::shared::MessageWriter<WSLA_GET_DISK_RESULT> writer;

    try
    {
        auto deviceName = GetLunDeviceName(Message.ScsiLun);

        writer->Result = 0;
        writer.WriteString("/dev/" + deviceName);
    }
    catch (...)
    {
        writer->Result = wil::ResultFromCaughtException();
    }

    Channel.SendMessage<WSLA_GET_DISK::TResponse>(writer.Span());
}

void HandleMessageImpl(wsl::shared::SocketChannel& Channel, const WSLA_ACCEPT& Message, const gsl::span<gsl::byte>& Buffer)
{
    sockaddr_vm SocketAddress{};
    wil::unique_fd ListenSocket{UtilListenVsockAnyPort(&SocketAddress, 1, true)};
    THROW_LAST_ERROR_IF(!ListenSocket);

    Channel.SendResultMessage<uint32_t>(SocketAddress.svm_port);

    wil::unique_fd Socket{UtilAcceptVsock(ListenSocket.get(), SocketAddress, SESSION_LEADER_ACCEPT_TIMEOUT_MS)};
    THROW_LAST_ERROR_IF(!Socket);

    THROW_LAST_ERROR_IF(dup2(Socket.get(), Message.Fd) < 0);
}

void HandleMessageImpl(wsl::shared::SocketChannel& Channel, const WSLA_CONNECT& Message, const gsl::span<gsl::byte>& Buffer)
{
    int32_t result = -EINVAL;
    auto sendResult = wil::scope_exit([&]() { Channel.SendResultMessage(result); });

    auto fd = UtilConnectVsock(Message.HostPort, true);
    if (!fd)
    {
        result = -errno;
    }
    else
    {
        result = fd.release();
    }
}

void HandleMessageImpl(wsl::shared::SocketChannel& Channel, const WSLA_OPEN& Message, const gsl::span<gsl::byte>& Buffer)
{
    int32_t result = EINVAL;

    auto sendResult = wil::scope_exit([&]() { Channel.SendResultMessage(result); });

    auto path = wsl::shared::string::FromMessageBuffer<WSLA_OPEN>(Buffer);
    int flags = 0;

    WI_SetFlagIf(flags, O_APPEND, WI_IsFlagSet(Message.Flags, WslaOpenFlagsAppend));
    WI_SetFlagIf(flags, O_TRUNC, !WI_IsFlagSet(Message.Flags, WslaOpenFlagsAppend) && WI_IsFlagSet(Message.Flags, WslaOpenFlagsWrite));
    WI_SetFlagIf(flags, O_CREAT, WI_IsFlagSet(Message.Flags, WslaOpenFlagsCreate));
    if (WI_IsFlagSet(Message.Flags, WslaOpenFlagsRead) && WI_IsFlagSet(Message.Flags, WslaOpenFlagsWrite))
    {
        WI_SetFlag(flags, O_RDWR);
    }
    else if (WI_IsFlagSet(Message.Flags, WslaOpenFlagsRead))
    {
        static_assert(O_RDONLY == 0);
    }
    else if (WI_IsFlagSet(Message.Flags, WslaOpenFlagsWrite))
    {
        WI_SetFlag(flags, O_WRONLY);
    }
    else
    {
        LOG_ERROR("Invalid WSLA_OPEN flags: {}", Message.Flags);
        return; // Return -EINVAL if no opening flags are passed.
    }

    wil::unique_fd fd = open(path, flags);
    if (!fd)
    {
        result = errno;
        LOG_ERROR("open({}, {}) failed: {}", path, flags, result);
        return;
    }

    if (dup2(fd.get(), Message.Fd) < 0)
    {
        result = errno;
        LOG_ERROR("dup2({}, {}) failed: {}", fd.get(), Message.Fd, result);
        return;
    }

    result = 0;
}

void HandleMessageImpl(wsl::shared::SocketChannel& Channel, const WSLA_TTY_RELAY& Message, const gsl::span<gsl::byte>&)
{
    THROW_LAST_ERROR_IF(fcntl(Message.TtyMaster, F_SETFL, O_NONBLOCK) < 0);

    pollfd pollDescriptors[2];

    pollDescriptors[0].fd = Message.TtyInput;
    pollDescriptors[0].events = POLLIN;
    pollDescriptors[1].fd = Message.TtyMaster;
    pollDescriptors[1].events = POLLIN;

    std::vector<gsl::byte> pendingStdin;
    std::vector<gsl::byte> buffer;

    Channel.Close();

    while (true)
    {
        int bytesWritten = 0;
        auto result = poll(pollDescriptors, COUNT_OF(pollDescriptors), pendingStdin.empty() ? -1 : 100);
        if (!pendingStdin.empty())
        {
            bytesWritten = write(Message.TtyMaster, pendingStdin.data(), pendingStdin.size());
            if (bytesWritten < 0)
            {
                if (errno != EAGAIN && errno != EWOULDBLOCK)
                {
                    LOG_ERROR("delayed stdin write failed {}", errno);
                }
            }
            else
            {
                if (bytesWritten <= pendingStdin.size()) // Partial or complete write
                {
                    pendingStdin.erase(pendingStdin.begin(), pendingStdin.begin() + bytesWritten);
                }
                else
                {
                    LOG_ERROR("Unexpected write result {}, pending={}", bytesWritten, pendingStdin.size());
                }
            }
        }

        if (result < 0)
        {
            LOG_ERROR("poll failed {}", errno);
            break;
        }

        // Relay stdin.
        if (pollDescriptors[0].revents & (POLLIN | POLLHUP | POLLERR) && pendingStdin.empty())
        {
            auto bytesRead = UtilReadBuffer(pollDescriptors[0].fd, buffer);
            if (bytesRead < 0)
            {
                LOG_ERROR("read failed {}", errno);
                break;
            }
            else if (bytesRead == 0)
            { // Stdin has been closed.
                pollDescriptors[0].fd = -1;

                CLOSE(Message.TtyMaster);
            }
            else
            {
                bytesWritten = write(Message.TtyMaster, buffer.data(), bytesRead);
                if (bytesWritten < 0)
                {
                    //
                    // If writing on stdin's pipe would block, mark the write as pending an continue.
                    // This is required blocking on the write() could lead to a deadlock if the child process
                    // is blocking trying to write on stderr / stdout while the relay tries to write stdin.
                    //

                    if (errno == EWOULDBLOCK || errno == EAGAIN)
                    {
                        assert(pendingStdin.empty());
                        pendingStdin.assign(buffer.begin(), buffer.begin() + bytesRead);
                    }
                    else
                    {
                        LOG_ERROR("write failed {}", errno);
                        break;
                    }
                }
            }
        }

        // Relay stdout & stderr
        if (pollDescriptors[1].revents & (POLLIN | POLLHUP | POLLERR))
        {
            auto bytesRead = UtilReadBuffer(pollDescriptors[1].fd, buffer);
            if (bytesRead <= 0)
            {
                if (bytesRead < 0 && errno != EIO)
                {
                    LOG_ERROR("read failed {} {}", bytesRead, errno);
                }

                // The tty has been closed, stop relaying.
                CLOSE(pollDescriptors[1].fd);
                pollDescriptors[1].fd = -1;
                break;
            }

            bytesWritten = UtilWriteBuffer(Message.TtyOutput, buffer.data(), bytesRead);
            if (bytesWritten < 0)
            {
                LOG_ERROR("write failed {}", errno);
                CLOSE(pollDescriptors[1].fd);
                pollDescriptors[1].fd = -1;
            }
        }
    }

    // Shutdown sockets and tty
    UtilSocketShutdown(Message.TtyInput, SHUT_WR);
    UtilSocketShutdown(Message.TtyOutput, SHUT_WR);
}

void HandleMessageImpl(wsl::shared::SocketChannel& Channel, const WSLA_FORK& Message, const gsl::span<gsl::byte>& Buffer)
{
    sockaddr_vm SocketAddress{};
    wil::unique_fd ListenSocket{UtilListenVsockAnyPort(&SocketAddress, 1, true)};
    THROW_LAST_ERROR_IF(!ListenSocket);

    WSLA_FORK_RESULT Response{};
    Response.Header.MessageSize = sizeof(Response);
    Response.Header.MessageType = WSLA_FORK_RESULT::Type;
    Response.Port = SocketAddress.svm_port;

    std::promise<pid_t> childPid;

    {
        auto childLogic = [ListenSocket = std::move(ListenSocket), &SocketAddress, &Channel, &Message, &childPid]() mutable {
            // Close parent channel
            if (Message.ForkType == WSLA_FORK::Process || Message.ForkType == WSLA_FORK::Pty)
            {
                Channel.Close();
            }

            childPid.set_value(getpid());

            wil::unique_fd ProcessSocket{UtilAcceptVsock(ListenSocket.get(), SocketAddress, SESSION_LEADER_ACCEPT_TIMEOUT_MS)};
            THROW_LAST_ERROR_IF(!ProcessSocket);

            ListenSocket.reset();

            auto subChannel = wsl::shared::SocketChannel{std::move(ProcessSocket), "ForkedChannel"};
            ProcessMessages(subChannel);
        };

        if (Message.ForkType == WSLA_FORK::Thread)
        {
            std::thread thread{std::move(childLogic)};
            thread.detach();

            Response.Pid = childPid.get_future().get();
        }
        else if (Message.ForkType == WSLA_FORK::Process)
        {
            Response.Pid = UtilCreateChildProcess("CreateChildProcess", std::move(childLogic));
        }
        else if (Message.ForkType == WSLA_FORK::Pty)
        {
            THROW_LAST_ERROR_IF(prctl(PR_SET_CHILD_SUBREAPER, 1) < 0);

            winsize ttySize{};
            ttySize.ws_col = Message.TtyColumns;
            ttySize.ws_row = Message.TtyRows;

            wil::unique_fd ttyMaster;
            auto result = forkpty(ttyMaster.addressof(), nullptr, nullptr, &ttySize);
            THROW_ERRNO_IF(errno, result < 0);

            if (result == 0) // Child
            {
                sigset_t SignalMask;
                sigemptyset(&SignalMask);
                try
                {
                    childLogic();
                }
                CATCH_LOG();
                exit(0);
            }

            Response.PtyMasterFd = ttyMaster.release();
            Response.Pid = result;
        }
        else
        {
            LOG_ERROR("Unexpected fork type: {}", Message.Type);
            THROW_ERRNO(EINVAL);
        }
    }

    ListenSocket.reset();
    Channel.SendMessage(Response);
}

void HandleMessageImpl(wsl::shared::SocketChannel& Channel, const WSLA_MOUNT& Message, const gsl::span<gsl::byte>& Buffer)
{
    WSLA_MOUNT_RESULT response{};
    response.Header.MessageType = WSLA_MOUNT_RESULT::Type;
    response.Header.MessageSize = sizeof(response);

    try
    {
        auto readField = [&](unsigned int index) -> const char* {
            if (index > 0)
            {
                return wsl::shared::string::FromSpan(Buffer, index);
            }

            return "";
        };

        mountutil::ParsedOptions options;
        if (Message.OptionsIndex > 0)
        {
            options = mountutil::MountParseFlags(wsl::shared::string::FromSpan(Buffer, Message.OptionsIndex));
        }

        const char* source = readField(Message.SourceIndex);
        const char* target = readField(Message.DestinationIndex);
        THROW_LAST_ERROR_IF(
            UtilMount(source, target, readField(Message.TypeIndex), options.MountFlags, options.StringOptions.c_str(), c_defaultRetryTimeout) < 0);

        std::optional<std::string> overlayTarget;
        if (WI_IsFlagSet(Message.Flags, WSLA_MOUNT::OverlayFs))
        {
            overlayTarget.emplace(target + std::string("-rw"));
            if (std::filesystem::exists(overlayTarget->c_str()))
            {
                LOG_ERROR("Overlay directory already exists: {}", overlayTarget.value());
                THROW_ERRNO(EEXIST);
            }

            THROW_LAST_ERROR_IF(UtilMountOverlayFs(overlayTarget->c_str(), target));

            if (WI_IsFlagSet(Message.Flags, WSLA_MOUNT::Chroot))
            {
                // If this is a chroot, simply mounts the overlay on top of the "-rw" folder.
                // We'll chroot into it later, so moving the mountpoint isn't needed.
                target = overlayTarget->c_str();

                THROW_LAST_ERROR_IF(MountInit((overlayTarget.value() + "/wsl-init").c_str()) < 0); // Required to call /gns later

                // If it exists, mount /etc/resolv.conf
                if (std::filesystem::exists("/etc/resolv.conf"))
                {
                    THROW_LAST_ERROR_IF(UtilMountFile("/etc/resolv.conf", (overlayTarget.value() + "/etc/resolv.conf").c_str()) < 0);
                }
            }
            else
            {
                // Move the "-rw" mount to its final target.
                THROW_LAST_ERROR_IF(mount(overlayTarget->c_str(), target, "none", MS_MOVE, nullptr) < 0);

                // Clean up the underlying mount point
                THROW_LAST_ERROR_IF(umount((overlayTarget.value() + "/rw").c_str()));

                std::error_code error;
                std::filesystem::remove_all(overlayTarget.value(), error);
                if (error.value() != 0)
                {
                    THROW_ERRNO(error.value());
                }
            }
        }

        if (WI_IsFlagSet(Message.Flags, WSLA_MOUNT::Chroot))
        {
            THROW_LAST_ERROR_IF(Chroot(target) < 0);
        }

        response.Result = 0;
    }
    catch (...)
    {
        LOG_CAUGHT_EXCEPTION();
        response.Result = wil::ResultFromCaughtException();
    }

    Channel.SendMessage<WSLA_MOUNT_RESULT>(response);
}

void HandleMessageImpl(wsl::shared::SocketChannel& Channel, const WSLA_EXEC& Message, const gsl::span<gsl::byte>& Buffer)
{
    auto Executable = wsl::shared::string::FromSpan(Buffer, Message.ExecutableIndex);
    auto ArgumentArray = wsl::shared::string::ArrayFromSpan(Buffer, Message.CommandLineIndex);
    ArgumentArray.push_back(nullptr);

    auto EnvironmentArray = wsl::shared::string::ArrayFromSpan(Buffer, Message.EnvironmentIndex);
    EnvironmentArray.push_back(nullptr);

    execve(Executable, (char* const*)(ArgumentArray.data()), (char* const*)(EnvironmentArray.data()));

    // Only reached if exec() fails
    Channel.SendResultMessage<int32_t>(errno);
}

void HandleMessageImpl(wsl::shared::SocketChannel& Channel, const WSLA_PORT_RELAY& Message, const gsl::span<gsl::byte>& Buffer)
{
    sockaddr_vm SocketAddress{};
    wil::unique_fd ListenSocket{UtilListenVsockAnyPort(&SocketAddress, 10, false)};
    THROW_LAST_ERROR_IF(!ListenSocket);

    Channel.SendResultMessage<uint32_t>(SocketAddress.svm_port);
    Channel.Close();
    UtilSetThreadName("PortRelay");
    RunLocalHostRelay(SocketAddress, ListenSocket.get());
}

void HandleMessageImpl(wsl::shared::SocketChannel& Channel, const WSLA_WAITPID& Message, const gsl::span<gsl::byte>& Buffer)
{
    WSLA_WAITPID_RESULT response{};
    response.State = WSLAOpenFlagsUnknown;

    auto sendResponse = wil::scope_exit([&]() { Channel.SendMessage(response); });

    wil::unique_fd process = syscall(SYS_pidfd_open, Message.Pid, 0);
    if (!process)
    {
        LOG_ERROR("pidfd_open({}) failed, {}", Message.Pid, errno);
        response.Errno = errno;
        return;
    }

    pollfd pollResult{};
    pollResult.fd = process.get();
    pollResult.events = POLLIN | POLLERR;

    int result = poll(&pollResult, 1, Message.TimeoutMs);
    if (result < 0)
    {
        LOG_ERROR("poll failed {}", errno);
        response.Errno = errno;
        return;
    }
    else if (result == 0) // Timed out
    {
        response.State = WSLAOpenFlagsRunning;
        response.Errno = 0;
        return;
    }

    if (WI_IsFlagSet(pollResult.revents, POLLIN))
    {
        siginfo_t childState{};
        auto result = waitid(P_PIDFD, process.get(), &childState, WEXITED);
        if (result < 0)
        {
            LOG_ERROR("waitid({}) failed, {}", process.get(), errno);
            response.Errno = errno;
            return;
        }

        response.Code = childState.si_status;
        response.Errno = 0;
        response.State = childState.si_code == CLD_EXITED ? WSLAOpenFlagsExited : WSLAOpenFlagsSignaled;
        return;
    }

    LOG_ERROR("Poll returned an unexpected error state on fd: {} for pid: {}", process.get(), Message.Pid);
}

void HandleMessageImpl(wsl::shared::SocketChannel& Channel, const WSLA_SIGNAL& Message, const gsl::span<gsl::byte>& Buffer)
{
    auto result = kill(Message.Pid, Message.Signal);
    Channel.SendResultMessage(result < 0 ? errno : 0);
}

void HandleMessageImpl(wsl::shared::SocketChannel& Channel, const WSLA_UNMOUNT& Message, const gsl::span<gsl::byte>& Buffer)
{
    Channel.SendResultMessage<int32_t>(umount(Message.Buffer) == 0 ? 0 : errno);
}

void HandleMessageImpl(wsl::shared::SocketChannel& Channel, const WSLA_DETACH& Message, const gsl::span<gsl::byte>& Buffer)
{
    sync();

    Channel.SendResultMessage<int32_t>(DetachScsiDisk(Message.Lun));
}

template <typename TMessage, typename... Args>
void HandleMessage(wsl::shared::SocketChannel& Channel, LX_MESSAGE_TYPE Type, const gsl::span<gsl::byte>& Buffer)
{
    if (TMessage::Type == Type)
    {
        if (Buffer.size() < sizeof(TMessage))
        {
            LOG_ERROR("Received message {}, but size is too small: {}. Expected {}", Type, Buffer.size(), sizeof(TMessage));
            THROW_ERRNO(EINVAL);
        }

        const auto Message = gslhelpers::try_get_struct<TMessage>(Buffer);
        HandleMessageImpl(Channel, *Message, Buffer);

        return;
    }
    else
    {
        if constexpr (sizeof...(Args) > 0)
        {
            HandleMessage<Args...>(Channel, Type, Buffer);
        }
        else
        {
            LOG_ERROR("Received unknown message type: {}", Type);
            THROW_ERRNO(EINVAL);
        }
    }
}

void ProcessMessage(wsl::shared::SocketChannel& Channel, LX_MESSAGE_TYPE Type, const gsl::span<gsl::byte>& Buffer)
{
    try
    {
        HandleMessage<WSLA_GET_DISK, WSLA_MOUNT, WSLA_EXEC, WSLA_FORK, WSLA_CONNECT, WSLA_WAITPID, WSLA_SIGNAL, WSLA_TTY_RELAY, WSLA_PORT_RELAY, WSLA_OPEN, WSLA_UNMOUNT, WSLA_DETACH, WSLA_ACCEPT>(
            Channel, Type, Buffer);
    }
    catch (...)
    {
        LOG_CAUGHT_EXCEPTION();

        // TODO: error message
    }
}

void ProcessMessages(wsl::shared::SocketChannel& Channel)
{
    while (Channel.Connected())
    {
        auto [Message, Range] = Channel.ReceiveMessageOrClosed<MESSAGE_HEADER>();
        if (Message == nullptr || Message->MessageType == LxMessageWSLAShutdown)
        {
            break;
        }

        ProcessMessage(Channel, Message->MessageType, Range);
    }

    LOG_INFO("Process {} exiting", getpid());
}

int WSLAEntryPoint(int Argc, char* Argv[])
{

    //
    // Mount devtmpfs.
    //

    int Result = UtilMount(nullptr, "/dev", "devtmpfs", 0, nullptr);
    if (Result < 0)
    {
        FATAL_ERROR("Failed to mount /dev");
    }

    if (UtilMount(nullptr, "/proc", "proc", 0, nullptr) < 0)
    {
        return -1;
    }

    if (UtilMount(nullptr, "/sys", "sysfs", 0, nullptr) < 0)
    {
        return -1;
    }

    //
    // Open kmesg for logging and ensure that the file descriptor is not set to one of the standard file descriptors.
    //
    // N.B. This is to work around a rare race condition where init is launched without /dev/console set as the controlling terminal.
    //

    InitializeLogging(false);
    if (g_LogFd <= STDERR_FILENO)
    {
        LOG_ERROR("/init was started without /dev/console");
        if (dup2(g_LogFd, 3) < 0)
        {
            LOG_ERROR("dup2 failed {}", errno);
        }

        close(g_LogFd);
        g_LogFd = 3;
    }

    //
    // Increase the soft and hard limit for number of open file descriptors.
    // N.B. the soft limit shouldn't be too high. See https://github.com/microsoft/WSL/issues/12985 .
    //

    rlimit Limit{};
    Limit.rlim_cur = 1024 * 10;
    Limit.rlim_max = 1024 * 1024;
    if (setrlimit(RLIMIT_NOFILE, &Limit) < 0)
    {
        LOG_ERROR("setrlimit(RLIMIT_NOFILE) failed {}", errno);
        return -1;
    }

    Limit.rlim_cur = 0x4000000;
    Limit.rlim_max = 0x4000000;
    if (setrlimit(RLIMIT_MEMLOCK, &Limit) < 0)
    {
        LOG_ERROR("setrlimit(RLIMIT_MEMLOCK) failed {}", errno);
        return -1;
    }

    //
    // Enable logging when processes receive fatal signals.
    //

    if (WriteToFile("/proc/sys/kernel/print-fatal-signals", "1\n") < 0)
    {
        return -1;
    }

    if (WriteToFile("/proc/sys/kernel/print-fatal-signals", "0\n") < 0)
    {
        return -1;
    }

    //
    // Disable rate limiting of user writes to dmesg.
    //

    if (WriteToFile("/proc/sys/kernel/printk_devkmsg", "on\n") < 0)
    {
        return -1;
    }

    THROW_LAST_ERROR_IF(UtilSetSignalHandlers(g_SavedSignalActions, false) < 0);

    //
    // Ensure /dev/console is present and set as the controlling terminal.
    // If opening /dev/console times out, stdout and stderr to the logging file descriptor.
    //

    wil::unique_fd ConsoleFd{};

    try
    {

        wsl::shared::retry::RetryWithTimeout<void>(
            [&]() {
                ConsoleFd = open("/dev/console", O_RDWR | O_CLOEXEC);
                THROW_LAST_ERROR_IF(!ConsoleFd);
            },
            c_defaultRetryPeriod,
            c_defaultRetryTimeout);

        THROW_LAST_ERROR_IF(login_tty(ConsoleFd.get()) < 0);
    }
    catch (...)
    {
        if (dup3(g_LogFd, STDOUT_FILENO, O_CLOEXEC) < 0)
        {
            LOG_ERROR("dup2 failed {}", errno);
        }

        if (dup3(g_LogFd, STDERR_FILENO, O_CLOEXEC) < 0)
        {
            LOG_ERROR("dup2 failed {}", errno);
        }
    }

    //
    // Open /dev/null for stdin.
    //

    {
        wil::unique_fd Fd{TEMP_FAILURE_RETRY(open("/dev/null", O_RDONLY))};
        if (!Fd)
        {
            LOG_ERROR("open({}) failed {}", "/dev/null", errno);
            return -1;
        }

        if (Fd.get() == STDIN_FILENO)
        {
            Fd.release();
        }
        else
        {
            if (TEMP_FAILURE_RETRY(dup2(Fd.get(), STDIN_FILENO)) < 0)
            {
                LOG_ERROR("dup2 failed {}", errno);
                return -1;
            }
        }
    }

    //
    // Enable the loopback interface.
    //

    {
        wil::unique_fd Fd{socket(AF_INET, SOCK_DGRAM, IPPROTO_IP)};
        if (!Fd)
        {
            LOG_ERROR("socket failed {}", errno);
            return -1;
        }

        if (EnableInterface(Fd.get(), "lo") < 0)
        {
            return -1;
        }
    }

    //
    // Make sure not to leak std fds to user processes.
    //

    for (int fd : {STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO})
    {
        SetCloseOnExec(fd, true);
    }

    //
    // Establish the message channel with the service via hvsocket.
    //

    wsl::shared::SocketChannel channel = {UtilConnectVsock(LX_INIT_UTILITY_VM_INIT_PORT, true), "mini_init"};
    if (channel.Socket() < 0)
    {
        FATAL_ERROR("Failed to connect to host hvsocket");
    }
    try
    {
        ProcessMessages(channel);
    }
    CATCH_LOG();

    LOG_INFO("Init exiting");

    try
    {
        auto children = ListInitChildProcesses();

        while (!children.empty())
        {

            // send SIGKILL to all running processes.
            for (auto pid : children)
            {
                if (kill(pid, SIGKILL) < 0)
                {
                    LOG_ERROR("Failed to send SIGKILL to {}: {}", pid, errno);
                }
            }

            // Wait for processes to actually exit.
            while (!children.empty())
            {
                auto Result = waitpid(-1, nullptr, 0);
                THROW_ERRNO_IF(errno, Result <= 0);
                LOG_INFO("Process {} exited", Result);
                children.erase(Result);
            }

            children = ListInitChildProcesses();
        }
    }
    CATCH_LOG();

    sync();

    try
    {
        for (auto disk : ListScsiDisks())
        {
            if (DetachScsiDisk(disk) < 0)
            {
                LOG_ERROR("Failed to detach disk: {}", disk);
            }
        }
    }
    CATCH_LOG();

    reboot(RB_POWER_OFF);

    return 0;
}