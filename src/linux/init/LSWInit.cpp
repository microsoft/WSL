#include "util.h"
#include "SocketChannel.h"
#include <utmp.h>
#include <sys/wait.h>

extern int InitializeLogging(bool SetStderr, wil::LogFunction* ExceptionCallback) noexcept;

extern std::set<pid_t> ListInitChildProcesses();

extern std::vector<unsigned int> ListScsiDisks();

extern int DetachScsiDisk(unsigned int Lun);

extern int g_LogFd;

void HandleMessageImpl(wsl::shared::SocketChannel& Channel, const LSW_MOUNT_REQUEST& message, const gsl::span<gsl::byte>& Buffer)
{
    LOG_ERROR("Received mount message: {}", message.ScsiLun);

    LSW_MOUNT_RESULT result{};
    result.Header.MessageType = LSW_MOUNT_RESULT::Type;
    result.Header.MessageSize = sizeof(result);
    Channel.SendMessage<LSW_MOUNT_RESULT>(result);
}

template <typename TMessage>
bool CallMessageHandler(wsl::shared::SocketChannel& Channel, LX_MESSAGE_TYPE Type, const gsl::span<gsl::byte>& Buffer)
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

        return true;
    }
    else
    {
        return false;
    }
}

void HandleMessage(wsl::shared::SocketChannel& Channel, LX_MESSAGE_TYPE Type, const gsl::span<gsl::byte>& Buffer)
{
    LOG_ERROR("Received unknown message type: {}", Type);
    THROW_ERRNO(EINVAL);
}

template <typename TMessage, typename... Args>
void HandleMessage(wsl::shared::SocketChannel& Channel, LX_MESSAGE_TYPE Type, const gsl::span<gsl::byte>& Buffer)
{
    if (CallMessageHandler<TMessage>(Channel, Type, Buffer))
    {
        return;
    }
    else
    {
        HandleMessage<TMessage>(Channel, Type, Buffer);
    }
}

void ProcessMessage(wsl::shared::SocketChannel& Channel, LX_MESSAGE_TYPE Type, const gsl::span<gsl::byte>& Buffer)
{
    try
    {
        HandleMessage<LSW_MOUNT_REQUEST>(Channel, Type, Buffer);
    }
    catch (...)
    {
        LOG_CAUGHT_EXCEPTION();

        // TODO: error message
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

    LOG_INFO("Init starting");

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

    while (true)
    {
        auto [Message, Range] = channel.ReceiveMessageOrClosed<MESSAGE_HEADER>();
        if (Message == nullptr)
        {
            break; // Socket was closed, exit
        }

        ProcessMessage(channel, Message->MessageType, Range);
    }

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