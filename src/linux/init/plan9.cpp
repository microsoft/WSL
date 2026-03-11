// Copyright (C) Microsoft Corporation. All rights reserved.
#include "common.h"
#include <memory>
#include <string>
#include <string_view>

#include <sys/resource.h>
#include <sys/socket.h>

#include <lxwil.h>
#include <p9fs.h>
#include <p9tracelogging.h>
#include <optional>

#include "wslpath.h"

#include "util.h"
#include "SocketChannel.h"
#include "WslDistributionConfig.h"

namespace {

// Callback used if the Plan 9 server encounters an exception.
void LogPlan9Exception(const char* message, const char* exceptionDescription) noexcept
{
    LogException(message, exceptionDescription);

    // Also log the message to the tracelogging output, if that is enabled.
    p9fs::Plan9TraceLoggingProvider::LogException(message, exceptionDescription);
}

// C++ helper for translating Windows paths to Linux paths.
std::string TranslatePath(char* windowsPath)
{
    std::string translatedPath = WslPathTranslate(windowsPath, TRANSLATE_FLAG_ABSOLUTE, TRANSLATE_MODE_UNIX);
    THROW_ERRNO_IF(EINVAL, translatedPath.empty());

    return translatedPath;
}

// Create a unix socket and bind it to the specified path.
wil::unique_fd CreateUnixServerSocket(const char* path)
{
    // Set up so the old working directory will be restored if it needs to be changed below.
    char oldCwdBuffer[PATH_MAX];
    char* oldCwd{};
    auto restoreCwd = wil::scope_exit([&oldCwd]() {
        if (oldCwd != nullptr)
        {
            chdir(oldCwd);
        }
    });

    // Check if the path will fit in a sockaddr_un (with room for null terminator).
    std::string_view pathView{path};
    if (pathView.length() >= sizeof(sockaddr_un::sun_path))
    {
        // It won't, so split the parent path and child name.
        auto index = pathView.find_last_of('/');

        // This really shouldn't happen unless the WSL service has a bug.
        THROW_ERRNO_IF(EINVAL, index == std::string_view::npos);

        const std::string parent{pathView.substr(0, index)};
        pathView = pathView.substr(index + 1);

        // Ensure the child name fits in sun_path (with null terminator).
        THROW_ERRNO_IF(ENAMETOOLONG, pathView.length() >= sizeof(sockaddr_un::sun_path));

        // Get the current working directory to restore it later, and change to the socket's parent
        // path.
        oldCwd = getcwd(oldCwdBuffer, sizeof(oldCwdBuffer));
        THROW_LAST_ERROR_IF(oldCwd == nullptr);
        THROW_LAST_ERROR_IF(chdir(parent.c_str()) < 0);
    }

    // Create the socket.
    wil::unique_fd server{socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0)};
    THROW_LAST_ERROR_IF(!server);

    // Delete the socket file if an old instance left it behind (e.g. if a crash occurred).
    if (unlink(path) < 0)
    {
        THROW_LAST_ERROR_IF(errno != ENOENT);
    }

    // Bind to the path.
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, pathView.data(), pathView.length());
    THROW_LAST_ERROR_IF(bind(server.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0);

    return server;
}

// Opens the log file, if one is specified, and sets the log level.
wil::unique_fd EnableLogging(const char* logFile, int logLevel, bool truncateLog)
{
    // Don't enable logging if no log file was specified.
    if (logFile == nullptr || strlen(logFile) == 0)
    {
        return {};
    }

    int flags = O_CREAT | O_WRONLY | O_APPEND;
    WI_SetFlagIf(flags, O_TRUNC, truncateLog);
    wil::unique_fd logFd{open(logFile, flags, 0600)};
    if (!logFd)
    {
        LOG_ERROR("FS: Could not open log file {}: {}", logFile, errno);
        return {};
    }

    p9fs::Plan9TraceLoggingProvider::SetLevel(logLevel);
    p9fs::Plan9TraceLoggingProvider::SetLogFileDescriptor(logFd.get());

    return logFd;
}

// Shut down the server, optionally only if there are no clients.
// Returns true if the server was stopped, false if there were clients preventing it from stopping.
bool StopPlan9Server(p9fs::IPlan9FileSystem& fileSystem, bool force)
try
{
    if (!force)
    {
        if (fileSystem.HasConnections())
        {
            // Can't shut down because there are connections.
            return false;
        }
    }

    // Disable exception logging to ignore expected errors from the server
    // shutting down.
    wil::g_LogExceptionCallback = nullptr;

    // Close all connections and stop listening.
    fileSystem.Pause();

    // Tear down the socket.
    fileSystem.Teardown();

    return true;
}
catch (...)
{
    LOG_CAUGHT_EXCEPTION_MSG("Could not stop file system server.");

    // Allow instance termination on failure to stop.
    return true;
}

void RunPlan9ControlFile(p9fs::IPlan9FileSystem& fileSystem, wsl::shared::SocketChannel& channel)
try
{
    std::vector<gsl::byte> Buffer;
    for (;;)
    {
        auto [Message, _] = channel.ReceiveMessageOrClosed<LX_INIT_STOP_PLAN9_SERVER>();
        if (Message == nullptr)
        {
            _exit(0);
        }

        channel.SendResultMessage<bool>(StopPlan9Server(fileSystem, Message->Force));
    }
}
CATCH_LOG();

} // namespace

void RunPlan9Server(const char* socketPath, const char* logFile, int logLevel, bool truncateLog, int controlSocket, int serverFd, wil::unique_fd& pipeFd)
{
    // Initialize logging.
    InitializeLogging(false, LogPlan9Exception);
    auto logFd = EnableLogging(logFile, logLevel, truncateLog);

    // Increase the limit for number of open file descriptors to the max allowed.
    rlimit limit{};
    THROW_LAST_ERROR_IF(getrlimit(RLIMIT_NOFILE, &limit) < 0);

    limit.rlim_cur = limit.rlim_max;
    if (setrlimit(RLIMIT_NOFILE, &limit) < 0)
    {
        LOG_ERROR("setrlimit(RLIMIT_NOFILE, {}lu, {}lu) failed {}", limit.rlim_cur, limit.rlim_max, errno);
    }

    // Open the root.
    wil::unique_fd rootFd{open("/", O_PATH | O_DIRECTORY | O_CLOEXEC)};
    THROW_LAST_ERROR_IF(!rootFd);

    {
        // Create the file system server.
        auto fileSystem = p9fs::CreateFileSystem(serverFd);

        // Add the share (the share takes ownership of the fd).
        fileSystem->AddShare("", rootFd.get());
        rootFd.release();

        fileSystem->Resume();

        // Close the pipe to signal the parent process that the plan9 server is started.
        pipeFd.reset();

        wsl::shared::SocketChannel channel({controlSocket}, "Plan9Control");
        RunPlan9ControlFile(*fileSystem, channel);
    }

    // Unlink the socket path (don't care about failure).
    if (socketPath != nullptr)
    {
        unlink(socketPath);
    }
}

// Start listening for Plan 9 file server clients.
std::pair<unsigned int, wsl::shared::SocketChannel> StartPlan9Server(const char* socketWindowsPath, const wsl::linux::WslDistributionConfig& Config)
try
{
    unsigned int result = LX_INIT_UTILITY_VM_INVALID_PORT;

    // Don't run the server if no socket was specified by init.
    // N.B. This is used to prevent the server from running when disabled with feature staging.
    // N.B. VM mode does not use a socket path.
    if (!UtilIsUtilityVm() && strlen(socketWindowsPath) == 0)
    {
        return {LX_INIT_UTILITY_VM_INVALID_PORT, wsl::shared::SocketChannel{}};
    }

    int sockets[] = {-1, -1};
    THROW_LAST_ERROR_IF(socketpair(PF_LOCAL, SOCK_STREAM, 0, sockets) < 0);

    wil::unique_fd parentSocket{sockets[0]};
    wil::unique_fd childSocket{sockets[1]};

    THROW_LAST_ERROR_IF(fcntl(parentSocket.get(), F_SETFD, FD_CLOEXEC) < 0);

    // Set the umask to the default.
    umask(Config.Umask);

    std::string translatedSocketPath;
    wil::unique_fd server;
    if (UtilIsUtilityVm())
    {
        sockaddr_vm address;
        server.reset(UtilBindVsockAnyPort(&address, (SOCK_STREAM | SOCK_NONBLOCK)));
        THROW_LAST_ERROR_IF(!server);

        // Increase the vsock send/receive buffers to increase throughput.
        int bufferSize = LX_INIT_UTILITY_VM_PLAN9_BUFFER_SIZE;
        THROW_LAST_ERROR_IF(setsockopt(server.get(), SOL_SOCKET, SO_SNDBUF, &bufferSize, sizeof(bufferSize)) < 0);
        THROW_LAST_ERROR_IF(setsockopt(server.get(), SOL_SOCKET, SO_RCVBUF, &bufferSize, sizeof(bufferSize)) < 0);
        result = address.svm_port;
    }
    else
    {
        // Translate the socket path (store a copy for unlinking on shutdown).
        translatedSocketPath = TranslatePath(const_cast<char*>(socketWindowsPath));

        // Create the server socket.
        server = CreateUnixServerSocket(translatedSocketPath.c_str());
    }

    wil::unique_pipe pipe = wil::unique_pipe::create(0);
    THROW_LAST_ERROR_IF(fcntl(pipe.read().get(), F_SETFD, FD_CLOEXEC) < 0)

    const int childPid = UtilCreateChildProcess(
        "Plan9",
        [&translatedSocketPath, localChildSocket = std::move(childSocket), &Config, server = std::move(server), pipe = std::move(pipe.write())]() {
            const std::string controlFdStr = std::to_string(localChildSocket.get());
            const std::string logLevelStr = std::to_string(Config.Plan9LogLevel);
            const std::string serverFdStr = std::to_string(server.get());
            const std::string pipeFdStr = std::to_string(pipe.get());
            std::vector<const char*> Arguments{
                LX_INIT_PLAN9,
                LX_INIT_PLAN9_CONTROL_SOCKET_ARG,
                controlFdStr.c_str(),
                LX_INIT_PLAN9_LOG_LEVEL_ARG,
                logLevelStr.c_str(),
                LX_INIT_PLAN9_SERVER_FD_ARG,
                serverFdStr.c_str(),
                LX_INIT_PLAN9_PIPE_FD_ARG,
                pipeFdStr.c_str()};

            if (!translatedSocketPath.empty())
            {
                Arguments.emplace_back(LX_INIT_PLAN9_SOCKET_PATH_ARG);
                Arguments.emplace_back(translatedSocketPath.c_str());
            }

            if (Config.Plan9LogTruncate)
            {
                Arguments.emplace_back(LX_INIT_PLAN9_TRUNCATE_LOG_ARG);
            }

            if (Config.Plan9LogFile.has_value())
            {
                Arguments.emplace_back(LX_INIT_PLAN9_LOG_FILE_ARG);
                Arguments.emplace_back(Config.Plan9LogFile->c_str());
            }

            Arguments.emplace_back(nullptr);

            if (execv(LX_INIT_PATH, (char* const*)(Arguments.data())) < 0)
            {
                LOG_ERROR("execv failed {}", errno);
            }

            _exit(0);
        });

    THROW_LAST_ERROR_IF(childPid < 0);

    // The child will close the pipe once the plan9 server has been started.
    // This wait is necessary because we want to make sure that no connection request
    // comes before the plan9 server is ready to accept it.
    char readBuf = 0;
    THROW_LAST_ERROR_IF(read(pipe.read().get(), &readBuf, 1) != 0);

    return {result, wsl::shared::SocketChannel{std::move(parentSocket), "Plan9Control"}};
}
catch (...)
{
    LOG_CAUGHT_EXCEPTION_MSG("Could not start file system server.")
    return {LX_INIT_UTILITY_VM_INVALID_PORT, wsl::shared::SocketChannel{}};
}