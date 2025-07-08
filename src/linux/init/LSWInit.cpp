/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    LSWInit.cpp

Abstract:

    TODO

--*/
#include "util.h"
#include "SocketChannel.h"
#include "message.h"
#include <utmp.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/syscall.h>
#include "mountutilcpp.h"

extern int InitializeLogging(bool SetStderr, wil::LogFunction* ExceptionCallback) noexcept;

extern std::set<pid_t> ListInitChildProcesses();

extern std::vector<unsigned int> ListScsiDisks();

extern int DetachScsiDisk(unsigned int Lun);

extern std::string GetLunDeviceName(unsigned int Lun);

void ProcessMessages(wsl::shared::SocketChannel& Channel);

extern int g_LogFd;

void HandleMessageImpl(wsl::shared::SocketChannel& Channel, const LSW_GET_DISK& Message, const gsl::span<gsl::byte>& Buffer)
{
    wsl::shared::MessageWriter<LSW_GET_DISK_RESULT> writer;

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

    Channel.SendMessage<LSW_GET_DISK::TResponse>(writer.Span());
}

void HandleMessageImpl(wsl::shared::SocketChannel& Channel, const LSW_CONNECT& Message, const gsl::span<gsl::byte>& Buffer)
{
    sockaddr_vm SocketAddress{};
    wil::unique_fd ListenSocket{UtilListenVsockAnyPort(&SocketAddress, 1, false)};
    THROW_LAST_ERROR_IF(!ListenSocket);

    Channel.SendResultMessage<uint32_t>(SocketAddress.svm_port);

    wil::unique_fd Socket{UtilAcceptVsock(ListenSocket.get(), SocketAddress, SESSION_LEADER_ACCEPT_TIMEOUT_MS)};
    THROW_LAST_ERROR_IF(!Socket);

    THROW_LAST_ERROR_IF(dup2(Socket.get(), Message.Fd) < 0);
}

void HandleMessageImpl(wsl::shared::SocketChannel& Channel, const LSW_FORK& Message, const gsl::span<gsl::byte>& Buffer)
{
    sockaddr_vm SocketAddress{};
    wil::unique_fd ListenSocket{UtilListenVsockAnyPort(&SocketAddress, 1, false)};
    THROW_LAST_ERROR_IF(!ListenSocket);

    LSW_FORK_RESULT Response{};
    Response.Header.MessageSize = sizeof(Response);
    Response.Header.MessageType = LSW_FORK_RESULT::Type;
    Response.Port = SocketAddress.svm_port;

    auto childLogic = [ListenSocket = std::move(ListenSocket), &SocketAddress, &Channel, &Message]() {
        // Close parent channel

        if (!Message.Thread)
        {
            Channel.Close();
        }

        wil::unique_fd ProcessSocket{UtilAcceptVsock(ListenSocket.get(), SocketAddress, SESSION_LEADER_ACCEPT_TIMEOUT_MS)};
        THROW_LAST_ERROR_IF(!ProcessSocket);

        auto subChannel = wsl::shared::SocketChannel{std::move(ProcessSocket), "ForkedChannel"};
        ProcessMessages(subChannel);
    };

    if (Message.Thread)
    {
        std::thread thread{std::move(childLogic)};
        Response.Pid = 1; // TODO: get pid

        thread.detach();
    }
    else
    {

        Response.Pid = UtilCreateChildProcess("CreateChildProcess", std::move(childLogic));
    }

    Channel.SendMessage(Response);
}

void HandleMessageImpl(wsl::shared::SocketChannel& Channel, const LSW_MOUNT& Message, const gsl::span<gsl::byte>& Buffer)
{
    LSW_MOUNT_RESULT response{};
    response.Header.MessageType = LSW_MOUNT_RESULT::Type;
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
        THROW_LAST_ERROR_IF(UtilMount(source, target, readField(Message.TypeIndex), options.MountFlags, options.StringOptions.c_str()) < 0);

        if (Message.Chroot)
        {
            THROW_LAST_ERROR_IF(chdir(target));
            THROW_LAST_ERROR_IF(chroot(".")); // TODO: pivot_root
        }

        response.Result = 0;
    }
    catch (...)
    {
        LOG_CAUGHT_EXCEPTION();
        response.Result = wil::ResultFromCaughtException();
    }

    Channel.SendMessage<LSW_MOUNT_RESULT>(response);
}

void HandleMessageImpl(wsl::shared::SocketChannel& Channel, const LSW_EXEC& Message, const gsl::span<gsl::byte>& Buffer)
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

void HandleMessageImpl(wsl::shared::SocketChannel& Channel, const LSW_WAITPID& Message, const gsl::span<gsl::byte>& Buffer)
{
    LSW_WAITPID_RESULT response{};
    response.State = LSWProcessStateUnknown;

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
        response.State = LSWProcessStateRunning;
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
        response.State = childState.si_code == CLD_EXITED ? LSWProcessStateExited : LSWProcessStateSignaled;
        return;
    }

    LOG_ERROR("Poll returned an unexpected error state on fd: {} for pid: ", process.get(), Message.Pid);
}

void HandleMessageImpl(wsl::shared::SocketChannel& Channel, const LSW_SIGNAL& Message, const gsl::span<gsl::byte>& Buffer)
{
    auto result = kill(Message.Pid, Message.Signal);
    Channel.SendResultMessage(result < 0 ? errno : 0);
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
        HandleMessage<LSW_GET_DISK, LSW_MOUNT, LSW_EXEC, LSW_FORK, LSW_CONNECT, LSW_WAITPID, LSW_SIGNAL>(Channel, Type, Buffer);
    }
    catch (...)
    {
        LOG_CAUGHT_EXCEPTION();

        // TODO: error message
    }
}

void ProcessMessages(wsl::shared::SocketChannel& Channel)
{
    while (true)
    {
        auto [Message, Range] = Channel.ReceiveMessageOrClosed<MESSAGE_HEADER>();
        if (Message == nullptr || Message->MessageType == LxMessageLswShutdown)
        {
            break;
        }

        ProcessMessage(Channel, Message->MessageType, Range);
    }
}

int LswEntryPoint(int Argc, char* Argv[])
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
    // Enable logging when processes receive fatal signals.
    //

    if (WriteToFile("/proc/sys/kernel/print-fatal-signals", "1\n") < 0)
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

    //
    // Ensure /dev/console is present and set as the controlling terminal.
    // If opening /dev/console times out, stdout and stderr to the logging file descriptor.
    //
    wil::unique_fd ConsoleFd{};

    try
    {
        wsl::shared::retry::RetryWithTimeout<void>(
            [&]() {
                ConsoleFd = open("/dev/console", O_RDWR);
                THROW_LAST_ERROR_IF(!ConsoleFd);
            },
            c_defaultRetryPeriod,
            c_defaultRetryTimeout);

        THROW_LAST_ERROR_IF(login_tty(ConsoleFd.get()) < 0);
    }
    catch (...)
    {
        if (dup2(g_LogFd, STDOUT_FILENO) < 0)
        {
            LOG_ERROR("dup2 failed {}", errno);
        }

        if (dup2(g_LogFd, STDERR_FILENO) < 0)
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