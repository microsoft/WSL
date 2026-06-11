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
#include <sys/sysinfo.h>
#include <grp.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <ctype.h>
#include <optional>
#include <fstream>
#include <algorithm>
#include <iostream>
#include <sstream>
#include <regex>
#include <thread>
#include <chrono>
#include <climits>
#include <pthread.h>
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
static std::optional<int> g_CachedFeatureFlags;
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
        return {};
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

int UtilAcceptVsock(int SocketFd, sockaddr_vm SocketAddress, int Timeout, int SocketFlags)

/*++

Routine Description:

    This routine accepts a socket connection.

Arguments:

    SocketFd - Supplies a socket file descriptor.

    SocketAddress - Supplies the socket address. This is passed by value instead
        of by reference because accept4 modifies the structure to contain the
        address of the peer socket.

    Timeout - Supplies a timeout.

    SocketFlags - Supplies the socket flags.

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
        Result = accept4(SocketFd, reinterpret_cast<sockaddr*>(&SocketAddress), &SocketAddressSize, SocketFlags);
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

int UtilCreateProcessAndWait(const char* const File, const char* const Argv[], int* Status, const std::map<std::string, std::string>& Env, bool DetachTerminal)

/*++

Routine Description:

    This routine creates a helper process from init and waits for it to exit.

Arguments:

    File - Supplies the file name to execute.

    Argv - Supplies the arguments for the command.

    Status - Supplies an optional pointer that receives the exit status of the
        process.

    DetachTerminal - Supplies a boolean that, when true, calls setsid() in the
        child process to detach it from the controlling terminal.

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
            _exit(-1);
        }

        //
        // Set environment variables.
        //

        for (const auto& e : Env)
        {
            setenv(e.first.c_str(), e.second.c_str(), 1);
        }

        //
        // Detach from the controlling terminal if requested.
        //

        if (DetachTerminal)
        {
            if (setsid() == -1)
            {
                LOG_ERROR("setsid failed {}", errno);
                _exit(-1);
            }
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
        _exit(-1);
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
            MountSource = QueryVirtiofsMountSource(MountEnum.Current().Source);
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

        auto transaction = channel.StartTransaction();
        transaction.Send<LX_INIT_QUERY_ENVIRONMENT_VARIABLE>(Message.Span());

        //
        // Read a response, this will contain the environment variable value if it exists.
        //

        Value = transaction.Receive<LX_INIT_QUERY_ENVIRONMENT_VARIABLE>().Buffer;

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

int UtilGetFeatureFlags()

/*++

Routine Description:

    This routine gets the feature flags, either directly, from an environment
    variable, or by querying it from the init process.

Arguments:

    None.

Return Value:

    The feature flags.

--*/

{
    //
    // If feature flags are already known, return them.
    //

    if (g_CachedFeatureFlags)
    {
        return *g_CachedFeatureFlags;
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
        // Query init for the value. If an error occurs, just return no features.
        //

        wsl::shared::SocketChannel channel{UtilConnectUnix(WSL_INIT_INTEROP_SOCKET), "wslinfo"};
        if (channel.Socket() < 0)
        {
            return FeatureFlags;
        }

        MESSAGE_HEADER Message;
        Message.MessageType = LxInitMessageQueryFeatureFlags;
        Message.MessageSize = sizeof(Message);

        auto transaction = channel.StartTransaction();
        transaction.Send(Message);
        FeatureFlags = transaction.Receive<RESULT_MESSAGE<int32_t>>().Result;
    }

    UtilSetFeatureFlags(FeatureFlags, FeatureFlagEnv == nullptr);
    return FeatureFlags;
}

void UtilSetFeatureFlags(int FeatureFlags, bool UpdateEnv)

/*++

Routine Description:

    This routine sets the feature flags and updates the cached value and environment variable.

Arguments:

    FeatureFlags - Supplies the feature flags to set.

    UpdateEnv - Supplies a boolean that indicates whether the environment variable should be updated.

Return Value:

    None.

--*/

try
{
    g_CachedFeatureFlags = FeatureFlags;
    if (UpdateEnv)
    {
        auto FeatureFlagsString = std::format("{:x}", FeatureFlags);
        if (setenv(WSL_FEATURE_FLAGS_ENV, FeatureFlagsString.c_str(), 1) < 0)
        {
            LOG_ERROR("setenv({}, {}, 1) failed {}", WSL_FEATURE_FLAGS_ENV, FeatureFlagsString, errno);
        }
    }
}
CATCH_LOG()

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

    auto transaction = channel.StartTransaction();
    transaction.Send(Message);

    const auto& response = transaction.Receive<RESULT_MESSAGE<uint8_t>>();
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
    auto transaction = channel.StartTransaction();
    transaction.Send<LX_INIT_QUERY_VM_ID>(Message.Span());

    return transaction.Receive<LX_INIT_QUERY_VM_ID>().Buffer;
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

int UtilMountFile(const char* Source, const char* Destination)
try
{
    // Is the file is a symlink, delete it since that would break the mount.
    if (std::filesystem::is_symlink(Destination))
    {
        std::filesystem::remove(Destination);
    }

    wil::unique_fd Fd{open(Destination, (O_CREAT | O_WRONLY), 0755)};
    THROW_LAST_ERROR_IF(!Fd);

    THROW_LAST_ERROR_IF(mount(Source, Destination, nullptr, (MS_RDONLY | MS_BIND), nullptr) < 0);
    THROW_LAST_ERROR_IF(mount(nullptr, Destination, nullptr, (MS_RDONLY | MS_REMOUNT | MS_BIND), nullptr) < 0);

    return 0;
}
CATCH_RETURN_ERRNO();

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
    // N.B. The mount operation is retried if:
    //      - The mount source does not yet exist (hot-added devices)
    //      - For Plan9 (9p): device is busy or not found
    //      - For VirtioFS: invalid tag (device not ready)
    //
    // N.B. MS_SHARED must be applied in a separate mount() call, so it is
    //      stripped from the initial mount flags and applied after the mount.
    //

    const unsigned long initialFlags = MountFlags & ~MS_SHARED;

    try
    {
        if (TimeoutSeconds.has_value())
        {
            wsl::shared::retry::RetryWithTimeout<void>(
                [&]() { THROW_LAST_ERROR_IF(mount(Source, Target, Type, initialFlags, Options) < 0); },
                c_defaultRetryPeriod,
                TimeoutSeconds.value(),
                [&]() {
                    errno = wil::ResultFromCaughtException();

                    // Generic device not ready errors
                    if (errno == ENXIO || errno == EIO || errno == ENOENT)
                    {
                        return true;
                    }

                    // Filesystem-specific device readiness errors
                    if (Type != nullptr)
                    {
                        if ((strcmp(Type, PLAN9_FS_TYPE) == 0 && errno == EBUSY) || (strcmp(Type, VIRTIO_FS_TYPE) == 0 && errno == EINVAL))
                        {
                            return true;
                        }
                    }

                    return false;
                });
        }
        else
        {
            THROW_LAST_ERROR_IF(mount(Source, Target, Type, initialFlags, Options) < 0);
        }
    }
    catch (...)
    {
        errno = wil::ResultFromCaughtException();
        LOG_ERROR("mount({}, {}, {}, {:#x}, {}) failed {}", Source, Target, Type, MountFlags, Options, errno);
        return -errno;
    }

    // N.B. The shared flag must be applied in a separate mount() call.
    if (WI_IsFlagSet(MountFlags, MS_SHARED))
    {
        if (mount(nullptr, Target, nullptr, MS_SHARED, nullptr) < 0)
        {
            LOG_ERROR("Failed to make shared mount {} {}", Target, errno);
            return -errno;
        }
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

// Returns true for signals that should not be saved/restored:
// SIGKILL/SIGSTOP — not settable per POSIX.
// SIGCONT — left at default to allow process resumption.
// SIGHUP — handled separately by the caller.
// 32-34 — internal NPTL signals (__SIGRTMIN through __SIGRTMIN+2) reserved
//         by glibc for thread cancellation and other runtime use.
static bool SkipSignal(unsigned int Signal)
{
    switch (Signal)
    {
    case SIGKILL:
    case SIGSTOP:
    case SIGCONT:
    case SIGHUP:
    case 32:
    case 33:
    case 34:
        return true;

    default:
        return false;
    }
}

int UtilSaveSignalHandlers(struct sigaction* SavedSignalActions)

/*++

Routine Description:

    This routine saves all settable signal handlers, skipping signals
    listed in SkipSignal() (non-settable, SIGHUP, and internal NPTL signals).

Arguments:

    SavedSignalActions - Supplies an array to save default signal actions.

Return Value:

    0 on success, -1 on failure.

--*/

{
    for (unsigned int Index = 1; Index < _NSIG; Index += 1)
    {
        if (SkipSignal(Index))
        {
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

    This routine sets all settable signal handlers to the given handler,
    skipping signals listed in SkipSignal().

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
        if (SkipSignal(Index))
        {
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
            auto WarningMessage = wsl::shared::Localization::MessageFailedToTranslate(Path);
            if (wil::ScopedWarningsCollector::CanCollectWarning())
            {
                EMIT_USER_WARNING(std::move(WarningMessage));
            }
            else
            {
                LOG_WARNING("{}", WarningMessage);
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
        auto matchesPrefix = [Path](const std::string_view& prefix) {
            if (!wsl::shared::string::StartsWith(Path, prefix, true))
            {
                return false;
            }

            // Validate that the next character is a path separator or the end of the string to prevent matching other distribution paths like:
            // \\wsl.localhost\<distro-name>-<suffix>

            auto nextChar = Path[prefix.size()];
            return nextChar == '\0' || nextChar == PATH_SEP || nextChar == PATH_SEP_NT;
        };

        auto PrefixLength = Prefix.length();
        if (!matchesPrefix(Prefix))
        {
            //
            // Check the old \\wsl$ prefix if it's not \\wsl.localhost.
            //

            std::string CompatPrefix{PLAN9_RDR_COMPAT_PREFIX};
            CompatPrefix += DistributionName;
            if (!matchesPrefix(CompatPrefix))
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

HvPciSwiotlbPool UtilReadHvPciSwiotlbPool()
{
    HvPciSwiotlbPool pool{};
    try
    {
        pool.Base = std::stoull(UtilReadFileContent("/sys/bus/vmbus/drivers/hv_pci/swiotlb_base"), nullptr, 0);
        pool.Size = std::stoull(UtilReadFileContent("/sys/bus/vmbus/drivers/hv_pci/swiotlb_size"), nullptr, 0);
    }
    catch (...)
    {
        pool = {};
    }

    return pool;
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

int WriteToFile(const char* Path, const char* Content, int OpenFlags, int Permissions)

/*++

Routine Description:

    Write content to the specified file.

Arguments:

    Path - Supplies the path to the file to write.

    Content - Supplies the content to be written to the file.

    OpenFlags - Supplies the flags passed to open().

    Permissions - Supplies the file mode used when O_CREAT causes the file to be created.

Return Value:

    0 on success, -1 on failure.

--*/

{
    wil::unique_fd Fd{open(Path, OpenFlags, Permissions)};
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

int ProcessCreateProcessMessage(wsl::shared::Transaction& Transaction, gsl::span<gsl::byte> Buffer)
{
    auto* Message = gslhelpers::try_get_struct<CREATE_PROCESS_MESSAGE>(Buffer);
    if (!Message)
    {
        LOG_ERROR("Unexpected message size {}", Buffer.size());
        return -1;
    }

    auto sendResult = [&](unsigned long Result) { Transaction.SendResultMessage<int32_t>(Result); };

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

#define RECLAIM_PATH CGROUP_MOUNTPOINT "/memory.reclaim"

static bool ReadProcFile(const char* Path, char* Buffer, size_t Size, bool LogErrors = true)

/*++

Routine Description:

    This routine reads the full contents of a procfs file into a caller-supplied
    NUL-terminated buffer. Because procfs content is generated on read and a
    single read() may return only a partial snapshot, this loops until EOF (or
    the buffer fills), which keeps later-appearing fields (for example the
    workingset counters deep in /proc/vmstat) from being truncated away.

Arguments:

    Path - Supplies the procfs path to read.

    Buffer - Supplies the buffer to fill; always NUL-terminated on success.

    Size - Supplies the size of Buffer in bytes (must be at least 1).

    LogErrors - Supplies whether open/read failures are logged. Callers for
        which absence is normal (for example PSI) pass false.

Return Value:

    true on success (Buffer holds the content), false on failure.

--*/

{
    if (Size == 0)
    {
        return false;
    }

    wil::unique_fd Fd{open(Path, O_RDONLY | O_CLOEXEC)};
    if (!Fd)
    {
        if (LogErrors)
        {
            LOG_ERROR("open({}) failed {}", Path, errno);
        }

        return false;
    }

    size_t Total = 0;
    while (Total < (Size - 1))
    {
        const ssize_t Result = TEMP_FAILURE_RETRY(read(Fd.get(), Buffer + Total, (Size - 1) - Total));
        if (Result < 0)
        {
            if (LogErrors)
            {
                LOG_ERROR("read({}) failed {}", Path, errno);
            }

            return false;
        }

        if (Result == 0)
        {
            break;
        }

        Total += static_cast<size_t>(Result);
    }

    Buffer[Total] = '\0';
    return Total > 0;
}

static bool ReadAggregateCpuTimes(unsigned long long& Busy, unsigned long long& Idle)

/*++

Routine Description:

    This routine parses the aggregate "cpu" line of /proc/stat and splits the
    cumulative jiffies into busy and idle buckets.

    Unlike a naive "user time only" measurement, idle time is taken as idle +
    iowait and everything else (user, nice, system, irq, softirq, steal) counts
    as busy. This ensures kernel-bound work -- background daemons, I/O, niced
    builds -- correctly keeps the VM out of the idle state.

Arguments:

    Busy - Receives the cumulative busy jiffies across all cores.

    Idle - Receives the cumulative idle jiffies (idle + iowait) across all cores.

Return Value:

    true on success, false on failure.

--*/

{
    //
    // The aggregate cpu line easily fits in this buffer.
    //

    char Buffer[256];
    if (!ReadProcFile("/proc/stat", Buffer, sizeof(Buffer)))
    {
        return false;
    }

    //
    // Format: "cpu  user nice system idle iowait irq softirq steal guest guest_nice".
    // The guest fields are already accounted for in user/nice and are ignored here.
    //

    unsigned long long Fields[8] = {};
    const int Parsed = sscanf(
        Buffer, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &Fields[0], &Fields[1], &Fields[2], &Fields[3], &Fields[4], &Fields[5], &Fields[6], &Fields[7]);

    if (Parsed < 5)
    {
        LOG_ERROR("failed to parse /proc/stat cpu line (parsed {})", Parsed);
        return false;
    }

    Idle = Fields[3] + Fields[4];
    Busy = 0;
    for (int Index = 0; Index < Parsed; Index += 1)
    {
        if (Index != 3 && Index != 4)
        {
            Busy += Fields[Index];
        }
    }

    return true;
}

static long long GetReclaimableCacheBytes()

/*++

Routine Description:

    This routine returns the amount of reclaimable file-backed page cache (in
    bytes) by parsing /proc/meminfo.

    It deliberately counts only memory that cache reclaim can actually return to
    the host: file-backed page cache (Active(file) + Inactive(file)) plus
    reclaimable slab (SReclaimable). Anonymous memory is excluded because neither
    drop_caches nor cgroup reclaim of clean cache can free it, so it must not
    drive the reclaim trigger.

Arguments:

    None.

Return Value:

    Reclaimable cache in bytes, or -1 on failure.

--*/

{
    char Buffer[4096];
    if (!ReadProcFile("/proc/meminfo", Buffer, sizeof(Buffer)))
    {
        return -1;
    }

    //
    // All values in /proc/meminfo are reported in kB.
    //

    long long TotalKb = 0;
    char* Save = nullptr;
    for (char* Line = strtok_r(Buffer, "\n", &Save); Line != nullptr; Line = strtok_r(nullptr, "\n", &Save))
    {
        const char* Value = nullptr;
        if (strncmp(Line, "Active(file):", 13) == 0)
        {
            Value = Line + 13;
        }
        else if (strncmp(Line, "Inactive(file):", 15) == 0)
        {
            Value = Line + 15;
        }
        else if (strncmp(Line, "SReclaimable:", 13) == 0)
        {
            Value = Line + 13;
        }

        if (Value != nullptr)
        {
            TotalKb += strtoll(Value, nullptr, 10);
        }
    }

    return TotalKb * 1024;
}

static long long GetFreeMemoryBytes()

/*++

Routine Description:

    This routine returns the amount of free memory (in bytes) currently held in
    the buddy allocator, used to decide when there are newly-freed pages worth
    compacting -- whether they were freed by reclaim or by a process exiting.

Arguments:

    None.

Return Value:

    Free memory in bytes, or -1 on failure.

--*/

{
    struct sysinfo Info = {};
    if (sysinfo(&Info) < 0)
    {
        LOG_ERROR("sysinfo failed {}", errno);
        return -1;
    }

    return static_cast<long long>(Info.freeram) * Info.mem_unit;
}

static double GetMemoryPressureAvg10()

/*++

Routine Description:

    This routine returns the PSI "some" 10-second memory pressure average from
    /proc/pressure/memory. This is the fraction of time (0-100) that some task
    stalled waiting on memory in the last 10 seconds; ~0 means there is slack to
    reclaim cold pages without hurting the workload, regardless of CPU activity.

Arguments:

    None.

Return Value:

    The "some avg10" pressure value, or -1.0 if PSI is unavailable.

--*/

{
    //
    // PSI may be unavailable (kernel built without CONFIG_PSI), which is a normal
    // condition the caller handles, so failures are not logged.
    //

    char Buffer[256];
    if (!ReadProcFile("/proc/pressure/memory", Buffer, sizeof(Buffer), false))
    {
        return -1.0;
    }

    //
    // The first line is "some avg10=<value> avg60=<value> avg300=<value> total=<value>".
    //

    const char* Marker = strstr(Buffer, "avg10=");
    if (Marker == nullptr)
    {
        return -1.0;
    }

    return strtod(Marker + (sizeof("avg10=") - 1), nullptr);
}

namespace {

//
// Tunables for the memory reduction thread.
//

constexpr auto c_pollInterval = std::chrono::seconds(10);

// An interval is CPU-idle when less than this fraction (per-mille) of aggregate CPU time was spent on
// non-idle work.
constexpr unsigned long long c_busyThresholdPerMille = 5; // 0.5%

// DropCache: drop only after this many consecutive idle intervals, then re-drop once the reclaimable
// cache grows by at least the re-arm threshold.
constexpr int c_dropCacheIdleIntervals = 30; // 5 minutes
constexpr long long c_cacheGrowthRearmBytes = 256ll * 1024 * 1024;

// Gradual: reclaimable cache below this floor is always retained; only the excess above it (beyond a
// hysteresis margin) is reclaimed.
constexpr long long c_floorBaseBytes = 128ll * 1024 * 1024;
constexpr long long c_gradualHysteresisBytes = 128ll * 1024 * 1024;

// Gradual is driven by PSI: reclaim cold cache while the "some avg10" memory pressure is below this
// value (in percent), and back off above it.
constexpr double c_pressureReclaimMax = 1.0;

// While the VM is busy, reclaim at most this much per interval so a large backlog is drained gently; an
// idle interval drains the full excess at once.
constexpr long long c_gradualStepBusyBytes = 256ll * 1024 * 1024;

// Compaction runs once free memory grows by at least this much since the last compaction.
constexpr long long c_compactFreeGrowthBytes = 256ll * 1024 * 1024;

//
// Mutable state carried across polling intervals by the reduction thread.
//

struct MemoryReclaimState
{
    // CPU sampling.
    unsigned long long PreviousBusy = 0;
    unsigned long long PreviousIdle = 0;
    bool HavePreviousSample = false;

    // DropCache.
    int IdleStreak = 0;
    bool ReclaimedThisIdlePeriod = false;
    long long CacheAtLastDrop = 0;

    // Compaction.
    long long FreeAtLastCompaction = 0;
};

bool RequestCgroupReclaim(long long Bytes)

/*++

Routine Description:

    Best-effort write of a byte count to the cgroup memory.reclaim knob. EAGAIN is an expected outcome
    (the kernel freed some, but not all, of the requested pages this pass) and is treated as a successful
    reclaim *without* logging, so the long-lived reduction thread does not emit an error every interval.
    A transient write failure never throws and tears down the thread.

Arguments:

    Bytes - Supplies the number of bytes to request the kernel reclaim.

Return Value:

    true if pages were reclaimed (full success or EAGAIN), false on an unexpected error.

--*/

{
    const std::string Request = std::to_string(Bytes);

    wil::unique_fd Fd{open(RECLAIM_PATH, O_WRONLY | O_CLOEXEC)};
    if (!Fd)
    {
        LOG_ERROR("open({}) failed {}", RECLAIM_PATH, errno);
        return false;
    }

    const std::string_view Buffer{Request};
    if (UtilWriteStringView(Fd.get(), Buffer) == static_cast<ssize_t>(Buffer.size()))
    {
        return true;
    }

    //
    // EAGAIN means the kernel could not evict the full amount this pass (pages were still freed), which
    // is normal under reclaim, so it is not logged.
    //

    if (errno == EAGAIN)
    {
        return true;
    }

    LOG_ERROR("write({}, {}) failed {}", RECLAIM_PATH, Request, errno);
    return false;
}

bool RunGradualTick(MemoryReclaimState& State, bool IntervalIdle)

/*++

Routine Description:

    Runs one interval of pressure-driven gentle reclaim (cold-first via cgroup memory.reclaim) toward a
    fixed floor. While the kernel reports little/no memory pressure (PSI) there is cold cache the guest is
    not really using, so it is reclaimed -- even while the VM is busy. A busy interval reclaims at most a
    bounded step so a large backlog is drained gently; an idle interval drains the full excess. When PSI
    is unavailable, reclaim falls back to gating on CPU idle.

Return Value:

    true if memory was reclaimed this interval, false otherwise.

--*/

{
    (void)State;

    const double Pressure = GetMemoryPressureAvg10();

    bool MayReclaim;
    if (Pressure < 0.0)
    {
        //
        // No PSI brake available: gate reclaim on CPU idle.
        //

        MayReclaim = IntervalIdle;
    }
    else
    {
        //
        // Reclaim only while pressure is low; back off once the workload starts stalling on memory.
        //

        MayReclaim = Pressure < c_pressureReclaimMax;
    }

    if (!MayReclaim)
    {
        return false;
    }

    const long long Cache = GetReclaimableCacheBytes();
    if (Cache < 0)
    {
        return false;
    }

    const long long Excess = Cache - c_floorBaseBytes;
    if (Excess <= c_gradualHysteresisBytes)
    {
        return false;
    }

    const long long ToFree = IntervalIdle ? Excess : (std::min)(Excess, c_gradualStepBusyBytes);

    // Best-effort: RequestCgroupReclaim never throws and does not log the expected EAGAIN, so a transient
    // write error cannot tear down the long-lived reduction thread.
    return RequestCgroupReclaim(ToFree);
}

bool RunDropCacheTick(MemoryReclaimState& State, bool IntervalIdle)

/*++

Routine Description:

    Runs one interval of DropCache policy: gated on sustained CPU idle because drop_caches is
    indiscriminate (it evicts hot and cold pages alike). Drops once on becoming idle, then re-drops only
    after the reclaimable cache grows meaningfully.

Return Value:

    true if the cache was dropped this interval, false otherwise.

--*/

{
    if (!IntervalIdle)
    {
        State.IdleStreak = 0;
        State.ReclaimedThisIdlePeriod = false;
        return false;
    }

    if (++State.IdleStreak < c_dropCacheIdleIntervals)
    {
        return false;
    }

    const long long Cache = GetReclaimableCacheBytes();
    if (Cache >= 0 && (!State.ReclaimedThisIdlePeriod || Cache > State.CacheAtLastDrop + c_cacheGrowthRearmBytes))
    {
        // Best-effort; WriteToFile logs internally on failure. A failed drop must not tear down the
        // long-lived reduction thread.
        if (WriteToFile("/proc/sys/vm/drop_caches", "1\n") == 0)
        {
            const long long After = GetReclaimableCacheBytes();
            State.CacheAtLastDrop = (After < 0) ? 0 : After;
            State.ReclaimedThisIdlePeriod = true;
            return true;
        }
    }

    return false;
}

void RunCompactionTick(MemoryReclaimState& State, bool Reclaimed)

/*++

Routine Description:

    Compacts when there are newly-freed pages worth coalescing -- from our reclaim or from a process
    exiting -- so free-page reporting can hand back large blocks. Tracks downward movement of free memory
    so a later rise re-triggers compaction.

--*/

{
    const long long Free = GetFreeMemoryBytes();
    bool Compact = Reclaimed;
    if (Free >= 0)
    {
        if (Free > State.FreeAtLastCompaction + c_compactFreeGrowthBytes)
        {
            Compact = true;
        }

        if (Free < State.FreeAtLastCompaction)
        {
            State.FreeAtLastCompaction = Free;
        }
    }

    if (Compact)
    {
        // Best-effort; WriteToFile logs internally on failure. A failed compaction must not tear down the
        // long-lived reduction thread; leave FreeAtLastCompaction unchanged so the next tick retries.
        if (WriteToFile("/proc/sys/vm/compact_memory", "1\n") == 0 && Free >= 0)
        {
            State.FreeAtLastCompaction = Free;
        }
    }
}

} // namespace

void StartMemoryReductionThread(LX_MINI_INIT_MEMORY_RECLAIM_MODE Mode)

/*++

Routine Description:

    This routine starts a background thread that reclaims memory and compacts free pages so the maximum
    number of pages can be discarded back to the host.

    The policy is:

        1. Gradual mode (gentle, cold-first via cgroup memory.reclaim) is driven by *memory pressure*,
           not CPU idleness. While the kernel reports little/no memory pressure (PSI), there is cold
           memory the guest is not really using, so it is reclaimed toward a fixed floor -- even while
           the VM is busy. This is important because a CPU-bound workload can sit on gigabytes of cold
           cache that a CPU-idle check would never reclaim. A busy interval reclaims at most a bounded
           step so a large backlog is drained gently; an idle interval drains the full excess. When PSI
           is unavailable, Gradual falls back to reclaiming only while CPU-idle.

        2. DropCache mode (the indiscriminate sledgehammer: drop_caches evicts the entire page cache,
           hot and cold alike) cannot run safely under load, so it stays gated on sustained CPU idle. It
           drops once on becoming idle, then re-drops only after the cache grows meaningfully.

        3. Compaction is gated on free-memory *growth*: it runs when there are newly-freed pages worth
           coalescing -- whether they were freed by our reclaim or by a process exiting -- and is skipped
           on ticks where nothing new was freed. This both avoids the previous "compact every tick" waste
           and ensures naturally-freed memory still gets coalesced for efficient page reporting.

    CPU utilization is measured over each interval using all non-idle CPU time (user, system, irq,
    softirq, steal) rather than just user time, so kernel-bound work keeps the VM out of the idle state.

Arguments:

    Mode - Supplies the memory reclaim mode.

Return Value:

    None.

--*/

try
{
    if (Mode == LxMiniInitMemoryReclaimModeDisabled)
    {
        return;
    }

    std::thread([Mode = Mode]() mutable {
        try
        {
            //
            // Run at idle scheduling priority so reclaim never competes with real work.
            //

            sched_param Parameter{};
            Parameter.sched_priority = 0;
            const int Result = pthread_setschedparam(pthread_self(), SCHED_IDLE, &Parameter);
            THROW_ERRNO_IF(Result, Result != 0);

            //
            // Gradual mode needs the cgroup memory.reclaim knob; fall back to dropping caches if it is
            // not present.
            //

            if (Mode == LxMiniInitMemoryReclaimModeGradual && access(RECLAIM_PATH, W_OK) < 0)
            {
                LOG_WARNING("access({}, W_OK) failed {}, falling back to drop_caches", RECLAIM_PATH, errno);
                Mode = LxMiniInitMemoryReclaimModeDropCache;
            }

            MemoryReclaimState State;

            for (;;)
            {
                std::this_thread::sleep_for(c_pollInterval);

                unsigned long long Busy = 0;
                unsigned long long Idle = 0;
                if (!ReadAggregateCpuTimes(Busy, Idle))
                {
                    continue;
                }

                //
                // Two samples are required to compute utilization over the interval.
                //

                if (!State.HavePreviousSample)
                {
                    State.PreviousBusy = Busy;
                    State.PreviousIdle = Idle;
                    State.HavePreviousSample = true;
                    continue;
                }

                const unsigned long long BusyDelta = Busy - State.PreviousBusy;
                const unsigned long long TotalDelta = (Busy + Idle) - (State.PreviousBusy + State.PreviousIdle);
                State.PreviousBusy = Busy;
                State.PreviousIdle = Idle;

                const bool IntervalIdle = (TotalDelta == 0) || (BusyDelta * 1000 <= TotalDelta * c_busyThresholdPerMille);

                const bool Reclaimed = (Mode == LxMiniInitMemoryReclaimModeGradual) ? RunGradualTick(State, IntervalIdle)
                                                                                    : RunDropCacheTick(State, IntervalIdle);

                RunCompactionTick(State, Reclaimed);
            }
        }
        CATCH_LOG()
    }).detach();
}
CATCH_LOG()
