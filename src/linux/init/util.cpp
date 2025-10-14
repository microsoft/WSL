/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    util.c

Abstract:

    This file utility function definitions.

--*/

#include <sys/mount.h>
#include <sys/wait.h>
#include <sys/epoll.h>
#include <sys/utsname.h>
#include <sys/types.h>
#include <grp.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <ctype.h>
#include <optional>
#include <fstream>
#include <iostream>
#include <sstream>
#include <regex>
#include "common.h"
#include "wslpath.h"
#include "util.h"
#include "drvfs.h"
#include "escape.h"
#include "config.h"
#include "mountutilcpp.h"
#include "message.h"
#include "RuntimeErrorWithSourceLocation.h"
#include "SocketChannel.h"
#include "Localization.h"

#define INITIAL_MESSAGE_BUFFER_SIZE (0x1000)

#define PLAN9_RDR_PREFIX "\\\\wsl.localhost\\"
#define PLAN9_RDR_COMPAT_PREFIX "\\\\wsl$\\"

#define WSLENV_ENV "WSLENV"

#define WSL_CGROUPS_FIELD_ENABLED (3)
#define WSL_CGROUPS_FIELD_MAX WSL_CGROUPS_FIELD_ENABLED
#define WSL_CGROUPS_FIELD_SEP '\t'
#define WSL_CGROUPS_FIELD_SUBSYSTEM (0)

#define WSL_MOUNT_OPTION_SEP ','

int g_IsVmMode = -1;
static sigset_t g_originalSignals;
thread_local std::string g_threadName;

namespace wil {

thread_local std::optional<std::stringstream> ScopedWarningsCollector::g_collectedWarnings;

}

int InteropServer::Create()

/*++

Routine Description:

    This routine creates an interop server unix socket and starts listening on it.

Arguments:

    None.

Return Value:

    0 on success, -1 on failure.

--*/

{
    if (!m_InteropSocketPath.empty())
    {
        LOG_ERROR("Interop server already created");
        return -1;
    }

    //
    // Generate a unique name to be used for the interop socket path.
    //

    m_InteropSocketPath = std::format(WSL_INTEROP_SOCKET_FORMAT, WSL_TEMP_FOLDER, getpid(), WSL_INTEROP_SOCKET);

    //
    // Ensure the WSL temp folder exists and has the correct mode.
    //

    if (UtilMkdir(WSL_TEMP_FOLDER, WSL_TEMP_FOLDER_MODE) < 0)
    {
        return -1;
    }

    //
    // Create a unix socket to handle interop requests.
    //
    // N.B. This is done before the child process is created to ensure that
    //      the socket is ready for connections.
    //

    m_InteropSocket.reset(socket(AF_UNIX, (SOCK_STREAM | SOCK_CLOEXEC), 0));
    if (!m_InteropSocket)
    {
        LOG_ERROR("socket failed {}", errno);
        return -1;
    }

    sockaddr_un InteropSocketAddress{};
    InteropSocketAddress.sun_family = AF_UNIX;
    strncpy(InteropSocketAddress.sun_path, m_InteropSocketPath.c_str(), (sizeof(InteropSocketAddress.sun_path) - 1));

    auto Result = bind(m_InteropSocket.get(), reinterpret_cast<sockaddr*>(&InteropSocketAddress), sizeof(InteropSocketAddress));
    if (Result < 0)
    {
        LOG_ERROR("bind failed {}", errno);
        return -1;
    }

    Result = listen(m_InteropSocket.get(), -1);
    if (Result < 0)
    {
        LOG_ERROR("listen failed {}", errno);
        return -1;
    }

    //
    // Ensure that any users can connect to the interop socket.
    //

    Result = chmod(m_InteropSocketPath.c_str(), 0777);
    if (Result < 0)
    {
        LOG_ERROR("chmod failed {}", errno);
        return -1;
    }

    return 0;
}

wil::unique_fd InteropServer::Accept() const

/*++

Routine Description:

    This routine accepts a connection on the interop server.

Arguments:

    None.

Return Value:

    The socket.

--*/

{
    wil::unique_fd InteropConnection{accept4(m_InteropSocket.get(), nullptr, nullptr, SOCK_CLOEXEC)};
    if (!InteropConnection)
    {
        LOG_ERROR("accept4 failed {}", errno);
    }

    timeval Timeout{};
    Timeout.tv_sec = INTEROP_TIMEOUT_SEC;
    if (setsockopt(InteropConnection.get(), SOL_SOCKET, SO_RCVTIMEO, &Timeout, sizeof(Timeout)) < 0)
    {
        LOG_ERROR("setsockopt(SO_RCVTIMEO) failed {}", errno);
    }

    return InteropConnection;
}

void InteropServer::Reset()
{
    if (!m_InteropSocketPath.empty())
    {
        unlink(m_InteropSocketPath.c_str());
        m_InteropSocketPath = {};
    }
}

InteropServer::~InteropServer()
{
    Reset();
}

int UtilAcceptVsock(int SocketFd, sockaddr_vm SocketAddress, int Timeout)

/*++

Routine Description:

    This routine accepts a socket connection.

Arguments:

    SocketFd - Supplies a socket file descriptor.

    SocketAddress - Supplies the socket address. This is passed by value instead
        of by reference because accept4 modifies the structure to contain the
        address of the peer socket.

    Timeout - Supplies a timeout.

Return Value:

    A file descriptor representing the socket, -1 on failure.

--*/

{
    //
    // If a timeout was specified, use a pollfd to wait for the accept.
    //

    int Result = 0;
    if (Timeout == -1)
    {
        pollfd PollDescriptor{SocketFd, POLLIN, 0};

        while (true)
        {
            Result = poll(&PollDescriptor, 1, 60 * 1000);
            if (Result < 0)
            {
                LOG_ERROR("poll({}) failed, {}", SocketFd, errno);
                return Result;
            }
            else if ((Result == 0) || ((PollDescriptor.revents & POLLIN) == 0))
            {
                LOG_ERROR("Waiting for abnormally long accept({})", SocketFd);
            }
            else
            {
                break;
            }
        }
    }
    else
    {
        pollfd PollDescriptor{SocketFd, POLLIN, 0};
        Result = poll(&PollDescriptor, 1, Timeout);
        if ((Result <= 0) || ((PollDescriptor.revents & POLLIN) == 0))
        {
            errno = ETIMEDOUT;
            Result = -1;
        }
    }

    if (Result != -1)
    {
        socklen_t SocketAddressSize = sizeof(SocketAddress);
        Result = accept4(SocketFd, reinterpret_cast<sockaddr*>(&SocketAddress), &SocketAddressSize, SOCK_CLOEXEC);
    }

    if (Result < 0)
    {
        LOG_ERROR("accept4 failed {}", errno);
    }

    return Result;
}

int UtilBindVsockAnyPort(struct sockaddr_vm* SocketAddress, int Type)

/*++

Routine Description:

    This routine creates a bound vsock socket an available port.

Arguments:

    SocketAddress - Supplies a buffer to receive the socket address of the
        socket.

    Type - Supplies the socket type.

Return Value:

    A file descriptor representing the bound socket, -1 on failure.

--*/

{
    int Result;
    socklen_t SocketAddressSize;
    int SocketFd;

    SocketFd = socket(AF_VSOCK, Type, 0);
    if (SocketFd < 0)
    {
        Result = -1;
        LOG_ERROR("socket failed {}", errno);
        goto BindVsockAnyPortExit;
    }

    memset(SocketAddress, 0, sizeof(*SocketAddress));
    SocketAddress->svm_family = AF_VSOCK;
    SocketAddress->svm_cid = VMADDR_CID_ANY;
    SocketAddress->svm_port = VMADDR_PORT_ANY;
    SocketAddressSize = sizeof(*SocketAddress);
    Result = bind(SocketFd, (const struct sockaddr*)SocketAddress, SocketAddressSize);

    if (Result < 0)
    {
        LOG_ERROR("bind failed {}", errno);
        goto BindVsockAnyPortExit;
    }

    //
    // Query the socket name to get the assigned port.
    //

    Result = getsockname(SocketFd, (struct sockaddr*)SocketAddress, &SocketAddressSize);

    if (Result < 0)
    {
        LOG_ERROR("getsockname failed {}", errno);
        goto BindVsockAnyPortExit;
    }

    Result = SocketFd;
    SocketFd = -1;

BindVsockAnyPortExit:
    if (SocketFd != -1)
    {
        CLOSE(SocketFd);
    }

    return Result;
}

size_t UtilCanonicalisePathSeparator(char* Path, char Separator)

/*++

Routine Description:

    This routine ensures all separators in Path use the specified separator.

Arguments:

    Path - Supplies the path to canonicalise.

    Separator - Supplies the separator character to be used.

Return Value:

    The size of the new string.

--*/

{
    size_t DestIndex;
    size_t PathLength;
    size_t SourceIndex;

    DestIndex = 0;
    SourceIndex = 0;
    PathLength = strlen(Path);

    //
    // Iterate through the path, replacing all separators.
    //

    for (; SourceIndex < PathLength; SourceIndex++)
    {
        if (Path[SourceIndex] == PATH_SEP || Path[SourceIndex] == PATH_SEP_NT)
        {
            //
            // Don't add a separator if previous char already is a separator.
            // Also handle the special case where 'Path' is a UNC path (\\X or //X)
            // where both separators should be kept.
            //

            if (DestIndex > 1 && Path[DestIndex - 1] == Separator)
            {
                continue;
            }

            Path[DestIndex] = Separator;
        }
        else
        {
            Path[DestIndex] = Path[SourceIndex];
        }

        DestIndex++;
    }

    Path[DestIndex] = '\0';
    return DestIndex;
}

void UtilCanonicalisePathSeparator(std::string& Path, char Separator)

/*++

Routine Description:

    This routine ensures all separators in Path use the specified separator.

Arguments:

    Path - Supplies the path to canonicalise.

    Separator - Supplies the separator character to be used.

Return Value:

    None.

--*/

{
    Path.resize(UtilCanonicalisePathSeparator(Path.data(), Separator));
}

wil::unique_fd UtilConnectToInteropServer(std::optional<pid_t> Pid)

/*++

Routine Description:

    This routine connects to the interop server of the current client process.

Arguments:

    Pid - Supplies an optional process ID to connect to.

Return Value:

    A file descriptor representing the connected socket, -1 on failure.

--*/

try
{
    char* InteropSocketPath;
    std::string Path;
    if (Pid.has_value())
    {
        Path = std::format(WSL_INTEROP_SOCKET_FORMAT, WSL_TEMP_FOLDER, Pid.value(), WSL_INTEROP_SOCKET);
        InteropSocketPath = Path.data();
    }
    else
    {
        //
        // Query the interop server environment variable. If the process does not
        // have the environment variable, or if the socket does not exists, search through parent process tree for an
        // interop server.
        //

        InteropSocketPath = getenv(WSL_INTEROP_ENV);
        if (InteropSocketPath == nullptr || (access(InteropSocketPath, F_OK) < 0 && errno == ENOENT))
        {
            pid_t Parent = getppid();
            while (Parent > 0)
            {
                Path = std::format(WSL_INTEROP_SOCKET_FORMAT, WSL_TEMP_FOLDER, Parent, WSL_INTEROP_SOCKET);
                if (access(Path.c_str(), F_OK) == 0)
                {
                    InteropSocketPath = Path.data();
                    break;
                }

                Parent = UtilGetPpid(Parent);
            }

            if (InteropSocketPath == nullptr)
            {
                return {};
            }

            setenv(WSL_INTEROP_ENV, InteropSocketPath, 1);
        }
    }

    //
    // Connect to the server and return the connected socket to the caller.
    //

    return UtilConnectUnix(InteropSocketPath);
}
CATCH_RETURN_ERRNO()

wil::unique_fd UtilConnectUnix(const char* Path)

/*++

Routine Description:

    This routine connects to the specified unix socket path.

Arguments:

    Path - Supplies the path of the unix socket.

Return Value:

    The connected socket, or a default-initialized value on failure.

--*/

{
    wil::unique_fd Socket{socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)};
    if (!Socket)
    {
        LOG_ERROR("socket failed {}", errno);
        return {};
    }

    sockaddr_un SocketAddress{};
    SocketAddress.sun_family = AF_UNIX;
    strncpy(SocketAddress.sun_path, Path, sizeof(SocketAddress.sun_path) - 1);
    if (connect(Socket.get(), reinterpret_cast<sockaddr*>(&SocketAddress), sizeof(SocketAddress)) < 0)
    {
        LOG_ERROR("connect failed {}", errno);
        return {};
    }

    return Socket;
}

wil::unique_fd UtilConnectVsock(unsigned int Port, bool CloseOnExec, std::optional<int> SocketBuffer, const std::source_location& Source) noexcept

/*++

Routine Description:

    This routine connects to a vsock with the specified port.

Arguments:

    Port - Supplies the port to connect to.

    CloseOnExec - Supplies a boolean specifying if the socket file descriptor should be closed on exec.

    SocketBuffer - Optionally supplies the size to use for the socket send and receive buffers.

    Source - Supplies the caller location.

Return Value:

    A file descriptor representing the connected socket, -1 on failure.

--*/

{
    int Type = SOCK_STREAM;
    WI_SetFlagIf(Type, SOCK_CLOEXEC, CloseOnExec);
    wil::unique_fd SocketFd{socket(AF_VSOCK, Type, 0)};
    if (!SocketFd)
    {
        LOG_ERROR("socket failed {} (from: {})", errno, Source);
        return {};
    }

    //
    // Set the socket connect timeout.
    //

    timeval Timeout{};
    Timeout.tv_sec = LX_INIT_HVSOCKET_TIMEOUT_SECONDS;
    if (setsockopt(SocketFd.get(), AF_VSOCK, SO_VM_SOCKETS_CONNECT_TIMEOUT, &Timeout, sizeof(Timeout)) < 0)
    {
        LOG_ERROR("setsockopt SO_VM_SOCKETS_CONNECT_TIMEOUT failed {}, (from: {})", errno, Source);
        return {};
    }

    if (SocketBuffer)
    {
        int BufferSize = *SocketBuffer;
        if (setsockopt(SocketFd.get(), SOL_SOCKET, SO_SNDBUF, &BufferSize, sizeof(BufferSize)) < 0)
        {
            LOG_ERROR("setsockopt(SO_SNDBUF, {}) failed {}, (from: {})", BufferSize, errno, Source);
            return {};
        }

        if (setsockopt(SocketFd.get(), SOL_SOCKET, SO_RCVBUF, &BufferSize, sizeof(BufferSize)) < 0)
        {
            LOG_ERROR("setsockopt(SO_RCVBUF, {}) failed {}, (from: {})", BufferSize, errno, Source);
            return {};
        }
    }

    sockaddr_vm SocketAddress{};
    SocketAddress.svm_family = AF_VSOCK;
    SocketAddress.svm_cid = VMADDR_CID_HOST;
    SocketAddress.svm_port = Port;
    if (connect(SocketFd.get(), (const struct sockaddr*)&SocketAddress, sizeof(SocketAddress)) < 0)
    {
        LOG_ERROR("connect port {} failed {} (from: {})", Port, errno, Source);
        return {};
    }

    return SocketFd;
}

int UtilCreateProcessAndWait(const char* const File, const char* const Argv[], int* Status, const std::map<std::string, std::string>& Env)

/*++

Routine Description:

    This routine creates a helper process from init and waits for it to exit.

Arguments:

    File - Supplies the file name to execute.

    Argv - Supplies the arguments for the command.

    Status - Supplies an optional pointer that receives the exit status of the
        process.

Return Value:

    0 on success, -1 on failure.

--*/
{
    pid_t ChildPid;
    int Result;
    int LocalStatus;
    pid_t WaitResult;

    Result = -1;

    //
    // Init needs to not ignore SIGCHLD so it can wait for this child.
    //

    auto restore = signal(SIGCHLD, SIG_DFL);

    ChildPid = fork();
    if (ChildPid < 0)
    {
        LOG_ERROR("Forking child process for {} failed with {}", File, errno);
        goto CreateProcessAndWaitEnd;
    }

    if (ChildPid == 0)
    {
        //
        // Restore default signal dispositions for the child process.
        //

        if (UtilSetSignalHandlers(g_SavedSignalActions, false) < 0 || UtilRestoreBlockedSignals() < 0)
        {
            exit(-1);
        }

        //
        // Set environment variables.
        //

        for (const auto& e : Env)
        {
            setenv(e.first.c_str(), e.second.c_str(), 1);
        }

        //
        // Invoke the executable.
        //

        // This explicit cast is okay for now because:
        // 1. execv function is guaranteed to not alter the arguments
        // 2. In sometime we probably will replace most of these string constants
        // with std::string anyway.
        execv(File, const_cast<char* const*>(Argv));
        LOG_ERROR("execv({}) failed with {}", File, errno);
        exit(-1);
    }

    if (Status == nullptr)
    {
        Status = &LocalStatus;
    }

    //
    // TODO_LX: Do we need a timeout when waiting for the process?
    //

    WaitResult = waitpid(ChildPid, Status, 0);
    if (WaitResult < 0)
    {
        LOG_ERROR("Waiting for {} failed with {}", File, errno);
        goto CreateProcessAndWaitEnd;
    }

    if (*Status != 0)
    {
        LOG_ERROR("{} failed with status {:#x}", File, *Status);
        goto CreateProcessAndWaitEnd;
    }

    Result = 0;

CreateProcessAndWaitEnd:

    //
    // Restore the disposition of SIGCHLD.
    //

    signal(SIGCHLD, restore);

    return Result;
}

int UtilExecCommandLine(const char* CommandLine, std::string* Output, int ExpectedStatus, bool PrintError)

/*++

Routine Description:

    This routine runs the command and optionally returns the output.

Arguments:

    CommandLine - Supplies the command line of the process to launch.

    Output - Supplies an optional pointer to a std::string to receive the output of the command.
        If no buffer is provided the output will appear in stdout.

    ExpectedStatus - Supplies the expected return status of the command.

    PrintError - Supplies a boolean that specifies if an error should be printed if the process does not return the expected status.

Return Value:

    0 on success, -1 on failure.

--*/

{
    //
    // Exec the command and read the output.
    //

    wil::unique_file Pipe{popen(CommandLine, "re")};
    if (!Pipe)
    {
        LOG_ERROR("popen({}) failed {}", CommandLine, errno);
        return -1;
    }

    std::vector<char> Buffer(1024);
    int Result = -1;
    while (fgets(Buffer.data(), Buffer.size(), Pipe.get()) != nullptr)
    {
        if (Output)
        {
            (*Output) += Buffer.data();
            if (Result < 0)
            {
                goto ErrorExit;
            }
        }
        else
        {
            fputs(Buffer.data(), stdout);
        }
    }

    if (ferror(Pipe.get()))
    {
        Result = -1;
        LOG_ERROR("fgets failed {}", errno);
        goto ErrorExit;
    }

    Result = 0;

ErrorExit:
    if (Pipe)
    {
        Result = pclose(Pipe.release());
        if (Result == -1)
        {
            LOG_ERROR("pclose failed {}", errno);
        }
        else
        {
            Result = UtilProcessChildExitCode(Result, CommandLine, ExpectedStatus, PrintError);
        }
    }

    return Result;
}

std::string UtilFindMount(const char* MountInfoFile, const char* Path, bool WinPath, size_t* PrefixLength)

/*++

Routine Description:

    This routine parses the /proc/self/mountinfo file to find a mount that
    matches the specified path.

    N.B. The caller is responsible for freeing the returned replacement prefix
         buffer.

Arguments:

    MountInfoFile - Supplies the path to the mountinfo file.

    Path - Supplies the path.

    WinPath - Supplies a value that indicates whether the path is a Windows
        path.

    PrefixLength - Supplies a pointer which receives the length of the prefix
        that should be stripped from the path.

Return Value:

    The replacement prefix on success, or an empty string on failure.

--*/

try
{
    char** MatchField;
    char** ReplacementField;

    mountutil::MountEnum MountEnum{MountInfoFile};
    if (WinPath != false)
    {
        MatchField = &MountEnum.Current().Source;
        ReplacementField = &MountEnum.Current().MountPoint;
    }
    else
    {
        MatchField = &MountEnum.Current().MountPoint;
        ReplacementField = &MountEnum.Current().Source;
    }

    std::string FoundReplacement;
    size_t FoundPrefixLength = 0;
    while (MountEnum.Next())
    {
        //
        // If a mount point was previously found, and this mount point is a
        // prefix of the path (or the previously found mount point, for Windows
        // to Linux translation), it means that the path is not actually on
        // the previously found mount, so discard that result.
        //
        // For example:
        // - When translating /mnt/c/foo/bar, first /mnt/c is found, but a
        //   later entry indicates /mnt/c/foo is also a mount point (e.g. using
        //   tmpfs). This means /mnt/c/foo/bar is not on the /mnt/c mount.
        // - When translating C:\foo, first /mnt/c is found. A later entry
        //   indicates /mnt itself is a mount point, making the earlier /mnt/c
        //   mount unreachable.
        //
        // TODO_LX: This doesn't catch the case when translating C:\foo\bar and
        //          /mnt/c/foo is a mount point. Handling that is more complicated.
        //

        if (!FoundReplacement.empty())
        {
            const char* LinuxPath = WinPath ? FoundReplacement.c_str() : Path;
            size_t LinuxPrefixLength = UtilIsPathPrefix(LinuxPath, MountEnum.Current().MountPoint, false);
            if (LinuxPrefixLength > 0)
            {
                FoundReplacement.resize(0);
            }
        }

        //
        // For Plan 9, parse the actual mount source from the superblock options.
        // For virtiofs, parse the mount source from source (for example drvfsC or drvfsaC).
        // If the file system isn't Plan 9, virtiofs, or DrvFs, skip this mount.
        //

        std::string MountSource;
        if (strcmp(MountEnum.Current().FileSystemType, PLAN9_FS_TYPE) == 0)
        {
            MountSource = UtilParsePlan9MountSource(MountEnum.Current().SuperOptions);
            if (MountSource.empty())
            {
                continue;
            }

            MountEnum.Current().Source = MountSource.data();
        }
        else if (strcmp(MountEnum.Current().FileSystemType, VIRTIO_FS_TYPE) == 0)
        {
            MountSource = UtilParseVirtiofsMountSource(MountEnum.Current().Source);
            if (MountSource.empty())
            {
                continue;
            }
            MountEnum.Current().Source = MountSource.data();
        }
        else if (strcmp(MountEnum.Current().FileSystemType, DRVFS_FS_TYPE) == 0)
        {
            //
            // The mount source is a Windows path and may use forward slashes;
            // flip them to backslashes.
            //

            UtilCanonicalisePathSeparator(MountEnum.Current().Source, PATH_SEP_NT);
        }
        else
        {
            continue;
        }

        //
        // Strip the trailing backslash if present.
        //

        size_t Length = strlen(MountEnum.Current().Source);
        if ((Length > 0) && (MountEnum.Current().Source[Length - 1] == PATH_SEP_NT))
        {
            MountEnum.Current().Source[Length - 1] = '\0';
        }

        //
        // For bind mounts, use the concatenation of the mount source and root
        // of the mount as the mount source string.
        //

        std::string CombinedMountSource;
        if (strcmp(MountEnum.Current().Root, "/") != 0)
        {
            CombinedMountSource += MountEnum.Current().Source;
            CombinedMountSource += MountEnum.Current().Root;
            UtilCanonicalisePathSeparator(CombinedMountSource, PATH_SEP_NT);
            MountEnum.Current().Source = CombinedMountSource.data();
        }

        //
        // Check if the match field is a prefix of the path.
        //
        // N.B. For Windows paths, only matches longer than the existing match
        //      are considered. This is because Windows mounts aren't
        //      guaranteed to be in order and NTFS directory mounts should be
        //      preferred over plain drive letter mounts if they match.
        //

        Length = UtilIsPathPrefix(Path, *MatchField, WinPath);
        if ((Length == 0) || ((WinPath != false) && (Length < FoundPrefixLength)))
        {
            continue;
        }

        //
        // Store the length of the prefix so the caller can strip it from the
        // string.
        //

        FoundPrefixLength = Length;

        //
        // Store the replacement.
        //

        FoundReplacement = *ReplacementField;

        //
        // Continue searching the file even if a mount has been found, since
        // newer mounts could shadow this one or be a nested mount.
        //
    }

    if (!FoundReplacement.empty() && PrefixLength != nullptr)
    {
        *PrefixLength = FoundPrefixLength;
    }

    return FoundReplacement;
}
catch (...)
{
    LOG_CAUGHT_EXCEPTION();
    return {};
}

std::optional<std::string> UtilGetEnv(const char* Name, char* Environment)

/*++

Routine Description:

    This queries the specified environment variable.

Arguments:

    Name - Supplies the name to query.

    Environment - Supplies an environment block to search. If NULL is provided
        the environment of the calling process is used.

Return Value:

    The value of the specified environment variable, NULL if there is no match.

--*/

{
    char* Current;
    size_t Length;
    size_t NameLength;
    std::optional<std::string> Value;

    if (Environment == nullptr)
    {
        const auto* EnvValue = getenv(Name);
        if (EnvValue != nullptr)
        {
            Value = std::string{EnvValue};
        }
    }
    else
    {
        NameLength = strlen(Name);
        for (size_t Index = 0;;)
        {
            Current = Environment + Index;
            Length = strlen(Current);
            if (Length == 0)
            {
                break;
            }

            if ((strncmp(Current, Name, NameLength) == 0) && (Current[NameLength] == '='))
            {
                Value = std::string{&Current[NameLength + 1]};
                break;
            }

            Index += Length + 1;
        }
    }

    return Value;
}

std::string UtilGetEnvironmentVariable(const char* Name)

/*++

Routine Description:

    This queries the specified environment variable. If the value does not exist it gets the value from
    the WSL interop server.

Arguments:

    Name - Supplies the name to query.

Return Value:

    The value of the specified environment variable if there is a match.

--*/

try
{
    //
    // Try to get the environment variable value. If it is not set, query the interop server for value.
    //

    std::vector<gsl::byte> Buffer;
    auto Value = getenv(Name);
    if (Value == nullptr)
    {
        wsl::shared::SocketChannel channel{UtilConnectToInteropServer(), "InteropClient"};
        if (channel.Socket() < 0)
        {
            return {};
        }

        wsl::shared::MessageWriter<LX_INIT_QUERY_ENVIRONMENT_VARIABLE> Message(LxInitMessageQueryEnvironmentVariable);
        Message.WriteString(Name);

        channel.SendMessage<LX_INIT_QUERY_ENVIRONMENT_VARIABLE>(Message.Span());

        //
        // Read a response, this will contain the environment variable value if it exists.
        //

        Value = channel.ReceiveMessage<LX_INIT_QUERY_ENVIRONMENT_VARIABLE>().Buffer;

        //
        // Set the environment variable for future queries.
        //

        if (setenv(Name, Value, 1) < 0)
        {
            LOG_ERROR("setenv({}, {}, 1) failed {}", Name, Value, errno);
        }
    }

    return Value;
}
catch (...)
{
    LOG_CAUGHT_EXCEPTION();
    return {};
}

int UtilGetFeatureFlags(const wsl::linux::WslDistributionConfig& Config)

/*++

Routine Description:

    This routine gets the feature flags, either directly, from an environment
    variable, or by querying it from the init process.

Arguments:

    Config - Supplies the distribution config.

Return Value:

    The feature flags.

--*/

{
    //
    // If feature flags are already known, return them.
    //

    static std::optional<int> g_CachedFeatureFlags;
    if (g_CachedFeatureFlags)
    {
        return *g_CachedFeatureFlags;
    }

    //
    // If an error occurs, just return no features.
    //

    if (Config.FeatureFlags.has_value())
    {
        return Config.FeatureFlags.value();
    }

    //
    // Check if the environment variable is present.
    //
    // N.B. This is used for processes launched directly from init (e.g.
    //      mount.drvfs during initial configuration), because they may not be
    //      able to connect to init.
    //

    int FeatureFlags = LxInitFeatureNone;

    const char* FeatureFlagEnv = getenv(WSL_FEATURE_FLAGS_ENV);

    if (FeatureFlagEnv != nullptr)
    {
        FeatureFlags = strtol(FeatureFlagEnv, nullptr, 16);
    }
    else
    {
        //
        // Query init for the value.
        //

        wsl::shared::SocketChannel channel{UtilConnectUnix(WSL_INIT_INTEROP_SOCKET), "wslinfo"};
        if (channel.Socket() < 0)
        {
            return FeatureFlags;
        }

        MESSAGE_HEADER Message;
        Message.MessageType = LxInitMessageQueryFeatureFlags;
        Message.MessageSize = sizeof(Message);

        channel.SendMessage(Message);
        FeatureFlags = channel.ReceiveMessage<RESULT_MESSAGE<int32_t>>().Result;
    }

    g_CachedFeatureFlags = FeatureFlags;
    return FeatureFlags;
}

std::optional<LX_MINI_INIT_NETWORKING_MODE> UtilGetNetworkingMode(void)

/*++

Routine Description:

    This routine queries the networking mode from the init process.

Arguments:

    None.

Return Value:

    The networking mode if successful, std::nullopt otherwise.

--*/

try
{
    wsl::shared::SocketChannel channel{UtilConnectUnix(WSL_INIT_INTEROP_SOCKET), "wslinfo"};
    THROW_LAST_ERROR_IF(channel.Socket() < 0);

    MESSAGE_HEADER Message;
    Message.MessageType = LxInitMessageQueryNetworkingMode;
    Message.MessageSize = sizeof(Message);

    channel.SendMessage(Message);

    const auto& response = channel.ReceiveMessage<RESULT_MESSAGE<uint8_t>>();
    auto NetworkingMode = static_cast<LX_MINI_INIT_NETWORKING_MODE>(response.Result);

    THROW_ERRNO_IF(EINVAL, NetworkingMode < LxMiniInitNetworkingModeNone || NetworkingMode > LxMiniInitNetworkingModeVirtioProxy);

    return NetworkingMode;
}
catch (...)
{
    LOG_CAUGHT_EXCEPTION();
    return {};
}

pid_t UtilGetPpid(pid_t Pid)

/*++

Routine Description:

    This routine returns the parent process id of the specified process.

Arguments:

    Pid - Supplies the process id to get the parent of.

Return Value:

    The parent process id if successful, -1 otherwise.

--*/

{
    //
    // Open the /proc/[pid]/stat file.
    //

    const auto FilePath = std::format("/proc/{}/stat", Pid);
    std::ifstream File(FilePath);

    std::string Line;
    if (!File || !std::getline(File, Line))
    {
        return -1;
    }

    //
    // Parse the file. Sample format: "86 (bash) S 9".
    // N.B. The second entry can contain a space so we can't just use strtok.
    //

    const std::regex Pattern("^[0-9]+ \\(.*\\) \\w ([0-9]+).*");
    std::smatch Match;
    if (!std::regex_match(Line, Match, Pattern) || Match.size() != 2)
    {
        LOG_ERROR("Failed to parse: {}, content: {}", FilePath, Line);
        return -1;
    }

    auto Result = strtol(Match.str(1).c_str(), nullptr, 10);
    if (Result == 0)
    {
        LOG_ERROR("Failed to parse: {}, content: {}", FilePath, Line);
        return -1;
    }

    return Result;
}

std::string UtilGetVmId(void)

/*++

Routine Description:

    This routine queries the VM ID from the init process.

Arguments:

    None.

Return Value:

    The VM ID if successful, an empty string otherwise.

--*/

try
{
    wsl::shared::SocketChannel channel{UtilConnectUnix(WSL_INIT_INTEROP_SOCKET), "wslinfo"};
    THROW_LAST_ERROR_IF(channel.Socket() < 0);

    wsl::shared::MessageWriter<LX_INIT_QUERY_VM_ID> Message(LxInitMessageQueryVmId);
    channel.SendMessage<LX_INIT_QUERY_VM_ID>(Message.Span());

    return channel.ReceiveMessage<LX_INIT_QUERY_VM_ID>().Buffer;
}
catch (...)
{
    LOG_CAUGHT_EXCEPTION();
    return {};
}

void UtilInitGroups(const char* User, gid_t Gid)

/*++

Routine Description:

    This routine initializes the groups for the current process.
    N.B. This is needed because the musl version of initgroups has a hard-coded 32 group max.

Arguments:

    User - Supplies the user name.

    Gid - Supplies the group id.

Return Value:

    None.

--*/

{
    if (initgroups(User, Gid) < 0)
    {
        int Count{};
        getgrouplist(User, Gid, nullptr, &Count);
        std::vector<gid_t> Groups(Count);
        THROW_LAST_ERROR_IF(getgrouplist(User, Gid, Groups.data(), &Count) < 0);

        THROW_LAST_ERROR_IF(setgroups(Count, Groups.data()) < 0);
    }
}

void UtilInitializeMessageBuffer(std::vector<gsl::byte>& Buffer)

/*++

Routine Description:

    This routine ensures the supplied buffer is initialized.

Arguments:

    Buffer - Supplies the buffer to be initialized.

Return Value:

    None.

--*/

{
    if (Buffer.size() < INITIAL_MESSAGE_BUFFER_SIZE)
    {
        Buffer.resize(INITIAL_MESSAGE_BUFFER_SIZE);
    }
}

bool UtilIsAbsoluteWindowsPath(const char* Path)

/*++

Routine Description:

    This routine determines if the supplied path is an absolute Windows path.

Arguments:

    Path - Supplies the path to check.

Return Value:

    true if the supplied path is an absolute Windows path, false otherwise.

--*/

{
    if ((strlen(Path) < 3) || (!(((Path[0] == PATH_SEP_NT || Path[0] == PATH_SEP) && (Path[1] == PATH_SEP_NT || Path[1] == PATH_SEP)) ||
                                 (isalpha(Path[0]) && Path[1] == DRIVE_SEP_NT))))
    {
        return false;
    }

    return true;
}

size_t UtilIsPathPrefix(const char* Path, const char* Prefix, bool WinPath)

/*++

Routine Description:

    This routine checks if one path is a prefix of another.

Arguments:

    Path - Supplies the path to check for a prefix.

    Prefix - Supplies the prefix to check for.

    WinPath - Supplies a value that indicates whether Path is a Windows path.

Return Value:

    The length of the prefix, or 0 if there is no match.

--*/

{
    size_t PathLength;
    size_t PrefixLength;
    char Separator;

    if (WinPath != false)
    {
        Separator = PATH_SEP_NT;
    }
    else
    {
        Separator = PATH_SEP;
    }

    //
    // Check the lengths and make sure the next character is a separator.
    //

    PathLength = strlen(Path);
    PrefixLength = strlen(Prefix);
    if ((PathLength < PrefixLength) || ((PathLength > PrefixLength) && (Path[PrefixLength] != Separator)))
    {
        return 0;
    }

    //
    // Check if the prefix matches.
    //
    // N.B. For Windows paths, this is done case-insensitive.
    //

    if (!wsl::shared::string::StartsWith(Path, Prefix, WinPath))
    {
        return 0;
    }

    return PrefixLength;
}

bool UtilIsUtilityVm(void)

/*++

Routine Description:

    This routine determines if the current process is running in a Utility VM or
    an WSL1 based instance.

Arguments:

    None.

Return Value:

    true if this instance is VM Mode, false otherwise.

--*/

{
    //
    // If this process has not yet checked, inspect the Linux kernel release
    // string.
    //

    if (g_IsVmMode == -1)
    {
        //
        // The VM Mode kernel release contains "microsoft", the lxcore kernel
        // contains "Microsoft".
        //
        // TODO: Come up with a different way to detect if we are running under
        // lxcore versus a Utility VM.
        //

        struct utsname UnameBuffer;
        memset(&UnameBuffer, 0, sizeof(UnameBuffer));
        if (uname(&UnameBuffer) < 0)
        {
            FATAL_ERROR("uname failed {}", errno);
        }

        g_IsVmMode = (strstr(UnameBuffer.release, "Microsoft") == NULL);
    }

    return g_IsVmMode;
}

int UtilListenVsockAnyPort(struct sockaddr_vm* Address, int Backlog, bool CloseOnExec)

/*++

Routine Description:

    This routine creates a bound and listening vsock socket an available port.

Arguments:

    Address - Supplies a buffer to receive the socket address of the socket.

    Backlog - Supplies the length of the backlog.

Return Value:

    A file descriptor representing the listening socket, -1 on failure.

--*/

{
    int Result;
    int SocketFd;

    int flags = SOCK_STREAM;
    WI_SetFlagIf(flags, SOCK_CLOEXEC, CloseOnExec);

    SocketFd = UtilBindVsockAnyPort(Address, flags);
    if (SocketFd < 0)
    {
        Result = -1;
        goto ListenVsockAnyPortExit;
    }

    Result = listen(SocketFd, Backlog);
    if (Result < 0)
    {
        LOG_ERROR("listen failed {}", errno);
        goto ListenVsockAnyPortExit;
    }

    Result = SocketFd;
    SocketFd = -1;

ListenVsockAnyPortExit:
    if (SocketFd != -1)
    {
        CLOSE(SocketFd);
    }

    return Result;
}

int UtilMkdir(const char* Path, mode_t Mode)

/*++

Routine Description:

    This routine ensures the directory exists.

Arguments:

    Path - Supplies the path of the directory to create.

    Mode - Supplies the mode.

Return Value:

    0 on success, -1 on failure.

--*/

{
    if ((mkdir(Path, Mode) < 0) && (errno != EEXIST))
    {
        LOG_ERROR("mkdir({}, {:o}) failed {}", Path, Mode, errno);
        return -1;
    }

    return 0;
}

int UtilMkdirPath(const char* Path, mode_t Mode, bool SkipLast)

/*++

Routine Description:

    This routine ensures the directory exists. If necessary, all its parents
    are created as well.

Arguments:

    Path - Supplies the path of the directory to create.

    Mode - Supplies the mode.

    SkipLast - Indicates whether to skip creating the final entry in the path.

Return Value:

    0 on success, -1 on failure.

--*/

{
    std::string LocalPath{Path};
    std::string::size_type Index = 0;

    //
    // Because the search is always from index + 1, the first leading / is skipped.
    //

    for (;;)
    {
        Index = LocalPath.find_first_of(PATH_SEP, Index + 1);
        if (Index != std::string::npos)
        {
            LocalPath[Index] = '\0';
        }
        else if (SkipLast)
        {
            break;
        }

        if (UtilMkdir(LocalPath.c_str(), Mode) < 0)
        {
            return -1;
        }

        if (Index == std::string::npos)
        {
            break;
        }

        LocalPath[Index] = PATH_SEP;
    }

    return 0;
}

int UtilMount(const char* Source, const char* Target, const char* Type, unsigned long MountFlags, const char* Options, std::optional<std::chrono::seconds> TimeoutSeconds)

/*++

Routine Description:

    This routine performs a mount with a retry and timeout.

Arguments:

    Source - Supplies the source of the mount.

    Target - Supplies the target of the mount.

    Type - Supplies the filesystem type.

    MountFlags - Supplies mount flags.

    Options - Supplies the mount options.

    TimeoutSeconds - Supplies an optional retry timeout in seconds.

Return Value:

    0 on success, < 0 on failure.

--*/

{
    //
    // Ensure the mount point exists.
    //

    if (UtilMkdirPath(Target, 0755) < 0)
    {
        return -1;
    }

    //
    // Mount the device to the mount point.
    //
    // N.B. The mount operation is only retried if the mount source does not yet exist,
    //      which can happen when hot-added devices are not yet available in the guest.
    //

    try
    {
        if (TimeoutSeconds.has_value())
        {
            wsl::shared::retry::RetryWithTimeout<void>(
                [&]() { THROW_LAST_ERROR_IF(mount(Source, Target, Type, MountFlags, Options) < 0); },
                c_defaultRetryPeriod,
                TimeoutSeconds.value(),
                [&]() {
                    errno = wil::ResultFromCaughtException();
                    return errno == ENOENT || errno == ENXIO || errno == EIO;
                });
        }
        else
        {
            THROW_LAST_ERROR_IF(mount(Source, Target, Type, MountFlags, Options) < 0);
        }
    }
    catch (...)
    {
        errno = wil::ResultFromCaughtException();
        LOG_ERROR("mount({}, {}, {}, 0x{}x, {}) failed {}", Source, Target, Type, MountFlags, Options, errno);
        return -errno;
    }

    return 0;
}

int UtilMountOverlayFs(const char* Target, const char* Lower, unsigned long MountFlags, std::optional<std::chrono::seconds> TimeoutSeconds)

/*++

Routine Description:

    This routine mounts an overlayfs at the specified location.

Arguments:

    Target - Supplies target for the overlayfs mount.

    Lower - Supplies the lower layer for the overlayfs mount. Multiple lower
        layers can be specified by a colon-separated list.

    MountFlags - Supplies mount flags for the operation.

    TimeoutSeconds - Supplies an optional timeout if the mount should be retried.

Return Value:

    0 on success, < 0 on failure.

--*/

try
{
    //
    // Set up the state required for overlayfs mount:
    //
    // <Target> - mount point for read/write overlayfs (this happens last)
    // <Target>/rw - tmpfs mount for upper and work dirs
    // <Target>/rw/upper - upper dir
    // <Target>/rw/work - work dir
    //

    if (UtilMkdirPath(Target, 0755) < 0)
    {
        return -1;
    }

    auto Path = std::format("{}/rw", Target);

    //
    // Create a tmpfs mount for the read/write layer
    //

    if (UtilMount(nullptr, Path.c_str(), "tmpfs", 0, nullptr) < 0)
    {
        return -1;
    }

    //
    // Create upper and work directories.
    //

    Path = std::format("{}/rw/upper", Target);
    if (UtilMkdir(Path.c_str(), 0755) < 0)
    {
        return -1;
    }

    auto MountOptions = std::format("lowerdir={},upperdir={},", Lower, Path);
    Path = std::format("{}/rw/work", Target);
    if (UtilMkdir(Path.c_str(), 0755) < 0)
    {
        return -1;
    }

    MountOptions += std::format("workdir={}", Path);
    if (UtilMount(nullptr, Target, "overlay", MountFlags, MountOptions.c_str(), TimeoutSeconds) < 0)
    {
        return -1;
    }

    return 0;
}
CATCH_RETURN_ERRNO()

int UtilOpenMountNamespace(void)

/*++

Routine Description:

    This routine opens a file descriptor to the current mount namespace.

Arguments:

    None.

Return Value:

    A file descriptor representing the current mount namespace, -1 on failure.

--*/

{
    int Fd;

    Fd = open("/proc/self/ns/mnt", (O_RDONLY | O_CLOEXEC));
    if (Fd < 0)
    {
        LOG_ERROR("open failed {}", errno);
    }

    return Fd;
}

int UtilParseCgroupsLine(char* Line, char** SubsystemName, bool* Enabled)

/*++

Routine Description:

    This routine parses a line from the /proc/cgroups file. The output
    buffers will be pointers into the provided line buffer and does not need to
    be freed.

    N.B. The Line buffer will be modified to insert NULL terminators.

Arguments:

    Line - Supplies the line to parse.

    SubsystemName - Supplies a buffer to receive the subsystem name.

    Enabled - Supplies a buffer to receive true if the subsystem is enabled,
        false otherwise.

Return Value:

    0 on success, -1 on failure.

--*/

{
    char* Current;
    int Field;
    int Result;

    //
    // Ignore comments.
    //

    Current = strchr(Line, '#');
    if (Current != nullptr)
    {
        *Current = '\0';
    }

    for (Field = 0, Current = Line; ((Current != nullptr) && (Field <= WSL_CGROUPS_FIELD_MAX));
         Field += 1, Current = strchr(Current, WSL_CGROUPS_FIELD_SEP))
    {
        //
        // Replace field separators with NULL characters and skip past them.
        //

        if (Field > 0)
        {
            *Current = '\0';
            Current += 1;
        }

        switch (Field)
        {
        case WSL_CGROUPS_FIELD_SUBSYSTEM:
            *SubsystemName = Current;
            break;

        case WSL_CGROUPS_FIELD_ENABLED:
            *Enabled = (*Current == '1');
            break;
        }
    }

    //
    // Check if all the fields were found. If not, this is a malformed line.
    //

    if (Field < WSL_CGROUPS_FIELD_MAX)
    {
        Result = -1;
        goto ParseCgroupsLineEnd;
    }

    Result = 0;

ParseCgroupsLineEnd:
    return Result;
}

std::string UtilParsePlan9MountSource(std::string_view MountOptions)

/*++

Routine Description:

    This routine parses the mount options to determine the actual source of a
    a Plan 9 mount.

Arguments:

    MountOptions - Supplies the mount options.

Return Value:

    The mount source, or NULL if no valid option could be found.

--*/

{
    //
    // Search each option.
    //
    // N.B. The first option is always "ro" or "rw" so doesn't need to be
    //      considered.
    //

    while (!MountOptions.empty())
    {
        auto Current = UtilStringNextToken(MountOptions, WSL_MOUNT_OPTION_SEP);
        if (wsl::shared::string::StartsWith(Current, PLAN9_ANAME_DRVFS))
        {
            //
            // Search for the sub path.
            //

            auto MountSource = Current.substr(PLAN9_ANAME_DRVFS_LENGTH);
            auto Index = Current.find(PLAN9_ANAME_PATH_OPTION);
            if (Index == std::string_view::npos)
            {
                break;
            }

            MountSource = Current.substr(Index + PLAN9_ANAME_PATH_OPTION_LENGTH);
            MountSource = UtilStringNextToken(MountSource, PLAN9_ANAME_OPTION_SEP);

            //
            // The value can only be used if it starts with a drive letter or
            // the UNC prefix "UNC\"
            //

            std::string Plan9Source;
            if (MountSource.length() > 1 && isalpha(MountSource[0]) && MountSource[1] == DRIVE_SEP_NT)
            {
                Plan9Source = MountSource;
            }
            else if (wsl::shared::string::StartsWith(MountSource, PLAN9_UNC_TRANSLATED_PREFIX))
            {
                Plan9Source = PLAN9_UNC_PREFIX;
                Plan9Source += MountSource.substr(PLAN9_UNC_TRANSLATED_PREFIX_LENGTH);
            }
            else
            {
                break;
            }

            //
            // Ensure the returned path uses Windows path separators.
            //

            UtilCanonicalisePathSeparator(Plan9Source, PATH_SEP_NT);
            return Plan9Source;
        }
    }

    return {};
}

std::string UtilParseVirtiofsMountSource(std::string_view Source)

/*++

Routine Description:

    This routine parses the mount source to determine the actual source of a
    a VirtioFs mount.

Arguments:

    Source - Supplies the source string.

Return Value:

    The mount source, or NULL if the source is not valid.

--*/

{
    std::string MountSource{};
    if (wsl::shared::string::StartsWith(Source, LX_INIT_DRVFS_ADMIN_VIRTIO_TAG) && (Source.size() >= sizeof(LX_INIT_DRVFS_ADMIN_VIRTIO_TAG)))
    {
        MountSource = Source[sizeof(LX_INIT_DRVFS_ADMIN_VIRTIO_TAG) - 1];
        MountSource += ":";
    }
    else if (wsl::shared::string::StartsWith(Source, LX_INIT_DRVFS_VIRTIO_TAG) && (Source.size() >= sizeof(LX_INIT_DRVFS_VIRTIO_TAG)))
    {
        MountSource = Source[sizeof(LX_INIT_DRVFS_VIRTIO_TAG) - 1];
        MountSource += ":";
    }

    return MountSource;
}

std::vector<char> UtilParseWslEnv(char* NtEnvironment)

/*++

Routine Description:

    This routine parses the WSLENV environment variable and constructs an
    environment block with the resulting values.

Arguments:

    NtEnvironment - Supplies an NT environment block. If NULL is provided,
        the current processes's environment block is used.

Return Value:

    The constructed env block.

--*/

{
    std::optional<std::string> EnvList;
    bool Reverse = false;

    std::vector<char> Output;

    auto Append = [&Output](const std::string_view& Content) {
        for (auto e : Content)
        {
            Output.push_back(e);
        }
    };

    Reverse = (NtEnvironment != nullptr);

    //
    // Always add WSLENV to the block.
    //

    Append(WSLENV_ENV "=");

    EnvList = UtilGetEnv(WSLENV_ENV, NtEnvironment);
    if (EnvList.has_value())
    {
        Append(EnvList.value());
    }

    Output.push_back('\0');
    if (EnvList.has_value())
    {
        //
        // Trim any whitespace from the end of the list.
        //

        while (!EnvList->empty() && isspace(EnvList->back()))
        {
            EnvList->pop_back();
        }

        for (char *Sp, *EnvName = strtok_r(EnvList->data(), ":", &Sp); EnvName != nullptr; EnvName = strtok_r(NULL, ":", &Sp))
        {
            char Mode = 0;
            bool SkipTranslation = false;
            char* Slash = strchr(EnvName, '/');
            if (Slash != nullptr)
            {
                *Slash = '\0';
                for (char* Flags = Slash + 1; *Flags != '\0'; Flags++)
                {
                    switch (*Flags)
                    {
                    case 'p': // path
                    case 'l': // path list
                        if (Mode != 0 && Mode != 'p' && Mode != 'l')
                        {
                            SkipTranslation = true;
                        }
                        else
                        {
                            Mode = *Flags;
                        }
                        break;

                    case 'u': // Win32 -> WSL translation only
                        if (Reverse == false)
                        {
                            SkipTranslation = true;
                        }

                        break;

                    case 'w': // WSL -> Win32 translation only
                        if (Reverse != false)
                        {
                            SkipTranslation = true;
                        }

                        break;

                    default:

                        //
                        // Ignore entries with an unknown flag to support future
                        // extensibility.
                        //

                        SkipTranslation = true;
                        break;
                    }
                }
            }

            auto EnvVal = UtilGetEnv(EnvName, NtEnvironment);
            if (!SkipTranslation && EnvVal.has_value())
            {
                switch (Mode)
                {
                case 'p':
                case 'l':
                {

                    //
                    // Translate the path or path list.
                    //

                    auto Result = UtilTranslatePathList(EnvVal->data(), Reverse);
                    if (Result.has_value())
                    {
                        EnvVal = std::move(Result.value());
                    }
                    else
                    {
                        SkipTranslation = true;
                    }

                    break;
                }

                default:
                    break;
                }
            }

            if (!SkipTranslation)
            {
                Append(std::format("{}={}", EnvName, EnvVal.value_or("")));
                Output.push_back('\0');
            }
        }
    }

    Output.push_back('\0');

    return Output;
}

int UtilProcessChildExitCode(int Status, const char* Name, int ExpectedStatus, bool PrintError)

/*++

Routine Description:

    Handles the exit status of a child process.

Arguments:

    Status - Supplies the exit status.

    Name - Supplies the process image name, for logging.

    ExpectedStatus - Supplies the expected exit status.

    PrintError - Supplies a boolean that specifies if an error should be printed if the process does not return the expected status.

Return Value:

    0 on success, -1 on failure.

--*/

{
    if (WIFEXITED(Status))
    {
        Status = WEXITSTATUS(Status);
        if (Status == ExpectedStatus)
        {
            return 0;
        }
    }
    else if (WIFSIGNALED(Status))
    {
        LOG_ERROR("{} killed by signal {}", Name, WTERMSIG(Status));
        return -1;
    }

    if (PrintError)
    {
        LOG_ERROR("{} returned {}", Name, Status);
    }

    return -1;
}

ssize_t UtilRead(int Fd, void* Buffer, size_t BufferSize, int Timeout)

/*++

Routine Description:

    This routine reads a message from the given file descriptor with an optional
    timeout.

Arguments:

    Fd - Supplies a file descriptor.

    Buffer - Supplies a buffer.

    BufferSize - Supplies the size of the buffer in bytes.

    Timeout - Supplies a timeout in milliseconds.

Return Value:

    The number of bytes read, -1 on failure.

--*/

{
    //
    // If a timeout was specified, use a pollfd.
    //

    ssize_t Result = 0;
    if (Timeout != -1)
    {
        pollfd PollDescriptor{Fd, POLLIN, 0};
        Result = poll(&PollDescriptor, 1, Timeout);
        if ((Result <= 0) || ((PollDescriptor.revents & POLLIN) == 0))
        {
            errno = ETIMEDOUT;
            Result = -1;
        }
    }

    if (Result != -1)
    {
        Result = TEMP_FAILURE_RETRY(read(Fd, Buffer, BufferSize));
    }

    return Result;
}

ssize_t UtilReadBuffer(int Fd, std::vector<gsl::byte>& Buffer, int Timeout)

/*++

Routine Description:

    This routine reads a message from the given file descriptor and
    automatically grows the buffer when needed.

Arguments:

    Fd - Supplies a file descriptor.

    Buffer - Supplies a buffer; this buffer will be resized if needed.

    Timeout - Supplies a timeout in milliseconds.

Return Value:

    The number of bytes read, -1 on failure.

--*/

try
{
    ssize_t Result;

    UtilInitializeMessageBuffer(Buffer);
    for (;;)
    {
        Result = UtilRead(Fd, Buffer.data(), Buffer.size(), Timeout);
        if (Result < 0)
        {
            //
            // When the message buffer is too small, EOVERFLOW is returned and
            // the buffer size is doubled.
            //

            if (errno == EOVERFLOW)
            {
                Buffer.resize(Buffer.size() * 2);
                continue;
            }
        }

        break;
    }

    return Result;
}
CATCH_RETURN_ERRNO()

std::string UtilReadFile(FILE* File)

/*++

Routine Description:

    This routine reads an entire file into a buffer.

Arguments:

    File - Supplies a open file stream.

Return Value:

    A std::string that contains the contents of the file.

--*/

{
    char* Line = nullptr;
    size_t LineLength;
    std::string output;

    //
    // Ensure the file is at the beginning of the stream.
    //

    rewind(File);

    //
    // Read the entire file into a buffer.
    //

    LineLength = 0;
    while (getline(&Line, &LineLength, File) != -1)
    {
        output += Line;
        output += '\n';
    }

    return output;
}

std::vector<gsl::byte> UtilReadFileRaw(const char* Path, size_t MaxSize)
{
    wil::unique_fd file{open(Path, O_RDONLY)};
    THROW_LAST_ERROR_IF(!file);

    constexpr auto bufferSize = 4096;

    size_t offset = 0;
    std::vector<gsl::byte> buffer;
    while (true)
    {
        buffer.resize(offset + bufferSize);

        int result = read(file.get(), buffer.data() + offset, bufferSize);
        THROW_LAST_ERROR_IF(result < 0);

        if (result == 0)
        {
            break;
        }

        offset += result;

        if (offset > MaxSize)
        {
            LOG_ERROR("File \"{}\" is too big. Maximum size: {}", Path, MaxSize);
            THROW_ERRNO(E2BIG);
        }
    }

    buffer.resize(offset);

    return buffer;
}

std::pair<std::optional<std::string>, std::optional<std::string>> UtilReadFlavorAndVersion(const char* Path)
try
{
    // See reference format: https://www.freedesktop.org/software/systemd/man/latest/os-release.html
    std::ifstream file;
    file.open(Path);

    std::optional<std::string> version;
    std::optional<std::string> flavor;
    std::regex versionPattern("^VERSION_ID=\"?([a-zA-Z0-9\\-_\\.,]*)\"?$");
    std::regex flavorPattern("^ID=\"?([a-zA-Z0-9\\-_\\.,]*)\"?$");

    std::string line;
    while (file && std::getline(file, line) && (!version.has_value() || !flavor.has_value()))
    {
        std::smatch match;
        if (std::regex_search(line, match, versionPattern))
        {
            version = match.str(1);
        }
        else if (std::regex_search(line, match, flavorPattern))
        {
            flavor = match.str(1);
        }
    }

    return std::make_pair(std::move(flavor), std::move(version));
}
catch (...)
{
    LOG_CAUGHT_EXCEPTION();

    return {};
}

ssize_t UtilReadMessageLxBus(int MessageFd, std::vector<gsl::byte>& Buffer, bool ShutdownOnDisconnect)

/*++

Routine Description:

    This routine reads a message from the server.

Arguments:

    MessageFd - Supplies a message port file descriptor.

    Buffer - Supplies a buffer; this buffer will be resized if needed.

    ShutdownOnDisconnect - Supplies true for shutdown on disconnect, false for
        _exit.

Return Value:

    The number of bytes read, -1 on failure.

--*/

try
{
    UtilInitializeMessageBuffer(Buffer);
    wil::unique_fd Epoll{epoll_create1(EPOLL_CLOEXEC)};
    if (!Epoll)
    {
        FATAL_ERROR("Failed to create epoll {}", errno);
    }

    epoll_event EpollEvent{};
    EpollEvent.events = EPOLLIN | EPOLLHUP;
    EpollEvent.data.fd = MessageFd;
    if (epoll_ctl(Epoll.get(), EPOLL_CTL_ADD, MessageFd, &EpollEvent) < 0)
    {
        FATAL_ERROR("Failed epoll_ctl {}", errno);
    }

    ssize_t BytesRead;
    for (;;)
    {
        //
        // Message port read/write operations are blocking, so use an epoll to
        // allow any incoming signals to be processed while waiting for the
        // message.
        //

        if (TEMP_FAILURE_RETRY(epoll_wait(Epoll.get(), &EpollEvent, 1, -1)) != 1)
        {
            FATAL_ERROR("Failed epoll_wait {}", errno);
        }

        BytesRead = TEMP_FAILURE_RETRY(read(MessageFd, Buffer.data(), Buffer.size()));
        if (BytesRead >= static_cast<ssize_t>(sizeof(MESSAGE_HEADER)))
        {
            break;
        }

        if (BytesRead < 0)
        {
            //
            // When the message buffer is too small, EOVERFLOW is returned and
            // the buffer is increased. When the Windows server disconnects
            // EPIPE is returned and the process handles the disconnect.
            //

            if (errno == EOVERFLOW)
            {
                auto BufferSizeNew = *(reinterpret_cast<size_t*>(Buffer.data()));
                if (BufferSizeNew <= Buffer.size())
                {
                    BufferSizeNew = Buffer.size() * 2;
                }

                Buffer.resize(BufferSizeNew);
                continue;
            }

            if (errno == EPIPE)
            {
                if (ShutdownOnDisconnect == false)
                {
                    _exit(0);
                }
            }

            FATAL_ERROR("Failed to read message {}", errno);
            break;
        }

        FATAL_ERROR("Unexpected message size {}", BytesRead);
        break;
    }

    return BytesRead;
}
CATCH_RETURN_ERRNO()

int UtilRestoreBlockedSignals()
{
    return sigprocmask(SIG_SETMASK, &g_originalSignals, nullptr);
}

int UtilSaveBlockedSignals(const sigset_t& SignalMask)
{
    return sigprocmask(SIG_BLOCK, &SignalMask, &g_originalSignals);
}

int UtilSaveSignalHandlers(struct sigaction* SavedSignalActions)

/*++

Routine Description:

    This routine saves all settable signal handlers except SIGHUP.

Arguments:

    SavedSignalActions - Supplies an array to save default signal actions.

Return Value:

    0 on success, -1 on failure.

--*/

{
    for (unsigned int Index = 1; Index < _NSIG; Index += 1)
    {
        switch (Index)
        {
        case SIGKILL:
        case SIGSTOP:
        case SIGCONT:
        case SIGHUP:
        case 32:
        case 33:
        case 34:
            continue;
        }

        if (sigaction(Index, NULL, &SavedSignalActions[Index]) < 0)
        {
            FATAL_ERROR("sigaction ({}) query failed {}", Index, errno);
        }
    }

    return 0;
}

int UtilSetSignalHandlers(struct sigaction* SavedSignalActions, bool Ignore)

/*++

Routine Description:

    This routine sets all settable signal handlers except SIGHUP to the given
    handler.

Arguments:

    SavedSignalActions - Supplies an array of signal handlers to set.

    Ignore - Supplies a boolean specifying if signals should be ignored.

Return Value:

    0 on success, -1 on failure.

--*/

{
    struct sigaction SignalAction;

    for (unsigned int Index = 1; Index < _NSIG; Index += 1)
    {
        switch (Index)
        {
        case SIGKILL:
        case SIGSTOP:
        case SIGCONT:
        case SIGHUP:
        case 32:
        case 33:
        case 34:
            continue;
        }

        memcpy(&SignalAction, &SavedSignalActions[Index], sizeof(SignalAction));
        if (Ignore != false)
        {
            SignalAction.sa_handler = SIG_IGN;
        }

        if (sigaction(Index, &SignalAction, NULL) < 0)
        {
            FATAL_ERROR("sigaction ({}) set failed {}", Index, errno);
        }
    }

    return 0;
}

void UtilSetThreadName(const char* Name)
{
    g_threadName = Name;

    if (prctl(PR_SET_NAME, reinterpret_cast<unsigned long>(Name), 0, 0, 0) < 0)
    {
        LOG_ERROR("prctl failed {}", errno);
    }
}

void UtilSocketShutdown(int Fd, int How)

/*++

Routine Description:

    This routine cleanly shuts down a socket.

Arguments:

    Fd - Supplies a socket file descriptor.

    How - Supplies the type of shutdown.

Return Value:

    None.

--*/

{
    if (shutdown(Fd, How) < 0)
    {
        LOG_ERROR("shutdown({}) failed {}", How, errno);
    }
}

bool UtilSizeTAdd(size_t Left, size_t Right, size_t* Out)

/*++

Routine Description:

    This routine checks if overflow will occur when adding two size_t values and
    if overflow is not possible, adds the values.

Arguments:

    Left - Supplies the first value.

    Right - Supplies the second value.

    Out - Supplies a pointer to receive sum on success.

Return Value:

    true if addition was done without overflow, false otherwise.

--*/

{
    bool Success = false;
    if (Right > (Right + Left))
    {
        goto SizeTAddExit;
    }

    *Out = Left + Right;
    Success = true;

SizeTAddExit:
    return Success;
}

std::string_view UtilStringNextToken(std::string_view& View, std::string_view Separators)

/*++

Routine Description:

    This routine extracts the next token identified by one of the specified
    separators.

Arguments:

    View - Supplies the string view to tokenize. On return, this parameter is
        modified to contain the remaining portion of the string after the
        separator, or an empty string if no separator was found.

    Separators - Supplies the separators that appear between the tokens.

Return Value:

    The contents of the string up to the next separator, or the entire string
    if no separator was found.

--*/

{
    std::string_view Result;
    auto Pos = View.find_first_of(Separators);
    if (Pos == std::string_view::npos)
    {
        Result = View;
        View = {};
    }
    else
    {
        Result = View.substr(0, Pos);
        View = View.substr(Pos + 1);
    }

    return Result;
}

std::string_view UtilStringNextToken(std::string_view& View, char Separator)

/*++

Routine Description:

    This routine extracts the next token identified by the specified separator.

Arguments:

    View - Supplies the string view to tokenize. On return, this parameter is
        modified to contain the remaining portion of the string after the
        separator, or an empty string if no separator was found.

    Separators - Supplies the separator that appears between the tokens.

Return Value:

    The contents of the string up to the next separator, or the entire string
    if no separator was found.

--*/

{
    std::string_view Result;
    auto Pos = View.find_first_of(Separator);
    if (Pos == std::string_view::npos)
    {
        Result = View;
        View = {};
    }
    else
    {
        Result = View.substr(0, Pos);
        View = View.substr(Pos + 1);
    }

    return Result;
}

std::optional<std::string> UtilTranslatePathList(char* PathList, bool IsNtPathList)

/*++

Routine Description:

    This routine translates a semicolon-separated list of NT paths into a
    colon-separated list of Linux paths.

Arguments:

    PathList - Supplies the semicolon-separated list of paths to translate.

    IsNtPathList - Supplies a boolean specifying if the list contains NT-style
        paths.

    LxPath - Supplies a buffer to receive an allocated string with the
        translated path list.

Return Value:

    0 on success, -error for failure.

--*/

{
    char Mode;
    const char* SourceSeparator;
    char TargetSeparator;
    std::string TranslatedList;

    if (IsNtPathList != false)
    {
        Mode = TRANSLATE_MODE_UNIX;
        SourceSeparator = ";";
        TargetSeparator = ':';
    }
    else
    {
        Mode = TRANSLATE_MODE_WINDOWS;
        SourceSeparator = ":";
        TargetSeparator = ';';
    }

    //
    // Translate each element in the list. If an element in the list fails to
    // translate, ignore it.
    //

    for (char *Sp, *Path = strtok_r(PathList, SourceSeparator, &Sp); Path != nullptr; Path = strtok_r(NULL, SourceSeparator, &Sp))
    {
        //
        // Skip relative Windows paths.
        //

        if ((Mode == TRANSLATE_MODE_UNIX) && (!UtilIsAbsoluteWindowsPath(Path)))
        {
            continue;
        }

        std::string TranslatedPath = WslPathTranslate(Path, 0, Mode);
        if (TranslatedPath.empty())
        {
            if (wil::ScopedWarningsCollector::CanCollectWarning())
            {
                EMIT_USER_WARNING(wsl::shared::Localization::MessageFailedToTranslate(Path));
            }
            else
            {
                LOG_ERROR("Failed to translate {}", Path);
            }
            continue;
        }

        if (!TranslatedList.empty())
        {
            TranslatedList += TargetSeparator;
        }

        TranslatedList += TranslatedPath;
    }

    if (TranslatedList.empty())
    {
        return {};
    }

    return TranslatedList;
}

std::string UtilWinPathTranslate(const char* Path, bool Reverse)

/*++

Routine Description:

    This routine translates an absolute Linux path or an absolute
    Windows path into the other.

Arguments:

    Path - Supplies the path to translate.

    Reverse - Supplies a bool, if set perform translation from Windows->Linux
        path; otherwise, translation from Linux->Windows path.

Return Value:

    0 on success, -error for general failure.

--*/

try
{
    size_t PathLength;
    size_t PrefixLength;
    char* Suffix;
    size_t SuffixLength;
    size_t TranslatedLength;
    size_t TranslatedSuffixLength;

    //
    // Find if there is a DrvFs or Plan 9 mount for the specified path.
    //

    auto PrefixReplacement = UtilFindMount(MOUNT_INFO_FILE, Path, Reverse, &PrefixLength);
    if (PrefixReplacement.empty())
    {
        //
        // The path is not part of a DrvFs or Plan 9 mount, so the path should
        // be translated to use the plan9 redirector.
        //

        return UtilWinPathTranslateInternal(Path, Reverse);
    }

    //
    // If translating Linux to Windows, see if any characters need to be
    // escaped.
    //

    PathLength = strlen(Path);
    SuffixLength = PathLength - PrefixLength;
    if (Reverse == false)
    {
        //
        // If the suffix is empty and the replacement prefix is just a drive
        // letter, make space to append a backslash.
        //

        if ((SuffixLength == 0) && (PrefixReplacement.length() == 2) && (PrefixReplacement[1] == DRIVE_SEP_NT))
        {
            TranslatedSuffixLength = 1;
        }
        else
        {
            TranslatedSuffixLength = EscapePathForNtLength(&Path[PrefixLength]);
        }
    }
    else
    {
        TranslatedSuffixLength = SuffixLength;
    }

    //
    // Construct the new path out of the replacement prefix and the remainder
    // of the path, escaping it if necessary.
    //

    std::string TranslatedPath{PrefixReplacement};
    TranslatedLength = PrefixReplacement.length() + TranslatedSuffixLength;
    TranslatedPath.resize(TranslatedLength, '\0');
    Suffix = &TranslatedPath[PrefixReplacement.length()];
    if (TranslatedSuffixLength != SuffixLength)
    {
        //
        // If the suffix is empty and the replacement prefix is just a drive
        // letter, append a backslash. Otherwise, escape and append the
        // suffix.
        //

        if ((SuffixLength == 0) && (TranslatedSuffixLength == 1))
        {
            *Suffix = PATH_SEP_NT;
        }
        else
        {
            EscapePathForNt(&Path[PrefixLength], Suffix);
        }
    }
    else
    {
        memcpy(Suffix, &Path[PrefixLength], SuffixLength);

        //
        // Make sure the translated path uses the correct separators.
        //
        // N.B. This is done by the escape method if escaping is necessary.
        //

        UtilCanonicalisePathSeparator(Suffix, Reverse ? PATH_SEP : PATH_SEP_NT);
    }

    return TranslatedPath;
}
catch (...)
{
    LOG_CAUGHT_EXCEPTION();
    return {};
}

std::string UtilWinPathTranslateInternal(const char* Path, bool Reverse)

/*++

Routine Description:

    This routine translates an absolute Linux path or an absolute
    Windows path into the other.

Arguments:

    Path - Supplies the path to translate.

    Reverse - Supplies a bool, if set perform translation from Windows->Linux
        path; otherwise, translation from Linux->Windows path.

Return Value:

    0 on success, -1 on failure.

--*/

try
{
    //
    // Get the distribution name from the environment variable.
    //

    const auto DistributionName = UtilGetEnvironmentVariable(WSL_DISTRO_NAME_ENV);
    if (DistributionName.empty())
    {
        return {};
    }

    //
    // Construct a prefix (\\wsl.localhost\DistributionName).
    //

    std::string Prefix{PLAN9_RDR_PREFIX};
    Prefix += DistributionName;

    //
    // For Windows to Linux translation, concatenate the prefix and the escaped
    // version of the path. For Linux to Windows translation, ensure the path
    // begins with the prefix, remove the prefix, and unescape the path.
    //

    std::string TranslatedPath{};
    if (Reverse == false)
    {
        TranslatedPath += Prefix;
        const size_t EscapedPathLength = EscapePathForNtLength(Path);
        std::string EscapedPath(EscapedPathLength, '\0');
        EscapePathForNt(Path, EscapedPath.data());
        TranslatedPath += EscapedPath;
    }
    else
    {
        auto PrefixLength = Prefix.length();
        if (!wsl::shared::string::StartsWith(Path, Prefix, true))
        {
            //
            // Check the old \\wsl$ prefix if it's not \\wsl.localhost.
            //

            std::string CompatPrefix{PLAN9_RDR_COMPAT_PREFIX};
            CompatPrefix += DistributionName;
            if (!wsl::shared::string::StartsWith(Path, CompatPrefix, true))
            {
                return {};
            }

            PrefixLength = CompatPrefix.length();
        }

        Path += PrefixLength;
        if (strlen(Path) == 0)
        {
            TranslatedPath += PATH_SEP;
        }
        else
        {
            TranslatedPath += Path;

            //
            // Canonicalize the path separators and unescape the string.
            //

            UtilCanonicalisePathSeparator(TranslatedPath.data(), PATH_SEP);
            UnescapePathInplace(TranslatedPath.data());
        }
    }

    return TranslatedPath;
}
catch (...)
{
    LOG_CAUGHT_EXCEPTION();
    return {};
}

ssize_t UtilWriteBuffer(int Fd, gsl::span<const gsl::byte> Buffer)

/*++

Routine Description:

    This routine writes an entire buffer to the given file descriptor.

Arguments:

    Fd - Supplies a file descriptor.

    Buffer - Supplies the buffer to write.

Return Value:

    The total number of bytes written, -1 on failure.

--*/

{
    return UtilWriteBuffer(Fd, Buffer.data(), Buffer.size());
}

ssize_t UtilWriteBuffer(int Fd, const void* Buffer, size_t BufferSize)

/*++

Routine Description:

    This routine writes an entire buffer to the given file descriptor.

Arguments:

    Fd - Supplies a file descriptor.

    Buffer - Supplies a buffer pointer.

    BufferSize - Supplies the buffer size.

Return Value:

    The total number of bytes written, -1 on failure.

--*/

{
    ssize_t BytesWritten;
    ssize_t Result;
    ssize_t TotalBytesWritten;
    auto* Offset = static_cast<const char*>(Buffer);

    Result = -1;
    TotalBytesWritten = 0;
    do
    {
        BytesWritten = TEMP_FAILURE_RETRY(write(Fd, Offset, BufferSize));
        if (BytesWritten < 0)
        {
            goto WriteBufferExit;
        }

        BufferSize -= BytesWritten;
        Offset += BytesWritten;
        TotalBytesWritten += BytesWritten;
    } while (BufferSize > 0);

    Result = TotalBytesWritten;

WriteBufferExit:
    return Result;
}

ssize_t UtilWriteStringView(int Fd, std::string_view StringView)

/*++

Routine Description:

    This routine writes a string view to the given file descriptor.

Arguments:

    Fd - Supplies a file descriptor.

    StringView - Supplies the string view to write.

Return Value:

    The total number of bytes written, -1 on failure.

--*/

{
    return UtilWriteBuffer(Fd, StringView.data(), StringView.size());
}

std::wstring UtilReadFileContentW(std::string_view path)
{
    std::wifstream file;
    file.exceptions(std::ifstream::failbit | std::ifstream::badbit);
    file.open(path);

    return {std::istreambuf_iterator<wchar_t>(file), {}};
}

std::string UtilReadFileContent(std::string_view path)
{
    std::ifstream file;
    file.exceptions(std::ifstream::failbit | std::ifstream::badbit);
    file.open(path);

    return {std::istreambuf_iterator<char>(file), {}};
}

uint16_t UtilWinAfToLinuxAf(uint16_t WinAddressFamily)
{
    uint16_t LinuxAddressFamily = AF_UNSPEC;

    switch (WinAddressFamily)
    {
    case 2:
        LinuxAddressFamily = AF_INET;
        break;
    case 23:
        LinuxAddressFamily = AF_INET6;
        break;
    }

    return LinuxAddressFamily;
}

int WriteToFile(const char* Path, const char* Content, int permissions)

/*++

Routine Description:

    Write content to the specified file.

Arguments:

    Path - Supplies the path to the file to write.

    Content - Supplies the content to be written to the file.

Return Value:

    0 on success, -1 on failure.

--*/

{
    wil::unique_fd Fd{open(Path, (O_WRONLY | O_CLOEXEC | O_CREAT), permissions)};
    if (!Fd)
    {
        int errnoPrev = errno;
        LOG_ERROR("open({}) failed {}", Path, errno);
        errno = errnoPrev;
        return -1;
    }

    std::string_view Buffer{Content};
    auto Result = UtilWriteStringView(Fd.get(), Buffer);
    if (Result != Buffer.size())
    {
        int errnoPrev = errno;
        LOG_ERROR("write({}, {}) failed {} {}", Path, Content, Result, errno);
        errno = errnoPrev;
        return -1;
    }

    return 0;
}

int ProcessCreateProcessMessage(wsl::shared::SocketChannel& channel, gsl::span<gsl::byte> Buffer)
{
    auto* Message = gslhelpers::try_get_struct<CREATE_PROCESS_MESSAGE>(Buffer);
    if (!Message)
    {
        LOG_ERROR("Unexpected message size {}", Buffer.size());
        return -1;
    }

    auto sendResult = [&](unsigned long Result) { channel.SendResultMessage<int32_t>(Result); };

    sockaddr_vm SocketAddress{};
    wil::unique_fd ListenSocket{UtilListenVsockAnyPort(&SocketAddress, 1, false)};
    THROW_LAST_ERROR_IF(!ListenSocket);

    sendResult(SocketAddress.svm_port);

    // Always return the execution result, since the service expects it
    int execResult = -1;
    auto sendExecResult = wil::scope_exit([&]() { sendResult(execResult); });

    const char* Path = wsl::shared::string::FromSpan(Buffer, Message->PathIndex);
    const char* Arguments = wsl::shared::string::FromSpan(Buffer, Message->CommandLineIndex);

    // Note: this makes the assumption that no empty arguments are in the message
    std::vector<const char*> ArgumentArray;
    while (*Arguments != '\0')
    {
        ArgumentArray.emplace_back(Arguments);
        Arguments += strlen(Arguments) + 1;
    }
    ArgumentArray.emplace_back(nullptr);

    auto ControlPipe = wil::unique_pipe::create(O_CLOEXEC);

    const int ChildPid = UtilCreateChildProcess("CreateChildProcess", [&]() {
        try
        {
            wil::unique_fd ProcessSocket{UtilAcceptVsock(ListenSocket.get(), SocketAddress, SESSION_LEADER_ACCEPT_TIMEOUT_MS)};
            THROW_LAST_ERROR_IF(!ProcessSocket);

            THROW_LAST_ERROR_IF(dup2(ProcessSocket.get(), STDIN_FILENO) < 0);
            THROW_LAST_ERROR_IF(dup2(ProcessSocket.get(), STDOUT_FILENO) < 0);
            execv(Path, (char* const*)(ArgumentArray.data()));

            // If this point is reached, an error needs to be reported back since execv() failed.
            THROW_LAST_ERROR();
        }
        catch (...)
        {
            auto error = wil::ResultFromCaughtException();
            LOG_ERROR("Command execution failed: {}", errno);

            if (write(ControlPipe.write().get(), &error, sizeof(error)) != sizeof(error))
            {
                LOG_ERROR("Failed to write command execution status: {}", errno);
            }
        }
    });

    THROW_LAST_ERROR_IF(ChildPid < 0);
    ControlPipe.write().reset();

    int ReadResult = TEMP_FAILURE_RETRY(read(ControlPipe.read().get(), &execResult, sizeof(execResult)));
    THROW_LAST_ERROR_IF(ReadResult < 0);

    // If the pipe closed without data, then exec() was successful
    if (ReadResult == 0)
    {
        execResult = 0;
    }
    else if (execResult == sizeof(execResult))
    {
        // Otherwise, return the error code to the service
        execResult = abs(execResult);
    }
    else
    {
        execResult = EINVAL;
    }

    return 0;
}