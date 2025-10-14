/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    util.h

Abstract:

    This file contains utility function declarations.

--*/

#pragma once

#include <sys/socket.h>
#include <linux/vm_sockets.h>
#include <lxwil.h>
#include <array>
#include <gsl/gsl>
#include <gslhelpers.h>
#include <chrono>
#include <functional>
#include <optional>
#include <thread>
#include <map>
#include <future>
#include <filesystem>
#include <vector>
#include <source_location>
#include "lxinitshared.h"
#include "lxdef.h"
#include "common.h"

namespace wsl::shared {
class SocketChannel;
}

namespace wsl::linux {
struct WslDistributionConfig;
}

#define CGROUP_MOUNTPOINT "/sys/fs/cgroup"
#define CGROUP2_DEVICE "cgroup2"
#define MOUNT_COMMAND "/bin/mount"
#define MOUNT_FSTAB_ARG "-a"
#define MOUNT_INTERNAL_ONLY_ARG "-i"
#define MOUNT_OPTIONS_ARG "-o"
#define MOUNT_TYPES_ARG "-t"

#define LDCONFIG_COMMAND "/sbin/ldconfig"

#define PLAN9_ANAME_OPTION "aname="
#define PLAN9_ANAME_DRVFS PLAN9_ANAME_OPTION LX_INIT_UTILITY_VM_DRVFS_SHARE_NAME
#define PLAN9_ANAME_DRVFS_LENGTH (sizeof(PLAN9_ANAME_DRVFS) - 1)
#define PLAN9_ANAME_OPTION_SEP ';'
#define PLAN9_ANAME_PATH_OPTION "path="
#define PLAN9_ANAME_PATH_OPTION_LENGTH (sizeof(PLAN9_ANAME_PATH_OPTION) - 1)
#define PLAN9_UNC_PREFIX "\\\\"
#define PLAN9_UNC_TRANSLATED_PREFIX "UNC\\"
#define PLAN9_UNC_TRANSLATED_PREFIX_LENGTH (sizeof(PLAN9_UNC_TRANSLATED_PREFIX) - 1)

#define PLAN9_FS_TYPE "9p"
#define VIRTIO_FS_TYPE "virtiofs"

#define PATH_SEP '/'
#define PATH_SEP_NT '\\'
#define DRIVE_SEP_NT ':'

#define WSL_DISTRO_NAME_ENV "WSL_DISTRO_NAME"
#define WSL_INTEROP_ENV "WSL_INTEROP"
#define WSL_DRVFS_ELEVATED_ENV "WSL_DRVFS_ELEVATED"
#define WSL_FEATURE_FLAGS_ENV "WSL_FEATURE_FLAGS"
#define WSL_INTEROP_SOCKET "interop"
#define WSL_INTEROP_SOCKET_FORMAT "{}/{}_{}"
#define WSL_TEMP_FOLDER RUN_FOLDER "/WSL"
#define WSL_TEMP_FOLDER_MODE 0777
#define WSL_INIT_INTEROP_SOCKET WSL_TEMP_FOLDER "/1_" WSL_INTEROP_SOCKET

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

constexpr auto c_defaultRetryPeriod = std::chrono::milliseconds{10};
constexpr auto c_defaultRetryTimeout = std::chrono::seconds{15};

class InteropServer
{
public:
    InteropServer() = default;
    ~InteropServer();
    InteropServer(const InteropServer&) = delete;
    InteropServer& operator=(const InteropServer&) = delete;

    InteropServer(InteropServer&& other) noexcept :
        m_InteropSocketPath(std::move(other.m_InteropSocketPath)), m_InteropSocket(std::move(other.m_InteropSocket))
    {
    }

    int Create();

    wil::unique_fd Accept() const;

    int Socket() const
    {
        return m_InteropSocket.get();
    }

    const char* Path() const
    {
        return m_InteropSocketPath.c_str();
    }

    void Reset();

private:
    std::string m_InteropSocketPath{};
    wil::unique_fd m_InteropSocket;
};

int UtilAcceptVsock(int SocketFd, sockaddr_vm Address, int Timeout = -1);

int UtilBindVsockAnyPort(struct sockaddr_vm* SocketAddress, int Type);

size_t UtilCanonicalisePathSeparator(char* Path, char Separator);

void UtilCanonicalisePathSeparator(std::string& Path, char Separator);

wil::unique_fd UtilConnectToInteropServer(std::optional<pid_t> Pid = {});

wil::unique_fd UtilConnectUnix(const char* Path);

wil::unique_fd UtilConnectVsock(
    unsigned int Port, bool CloseOnExec, std::optional<int> SocketBuffer = {}, const std::source_location& Source = std::source_location::current()) noexcept;

// Needs to be declared before UtilCreateChildProcess().
void UtilSetThreadName(const char* Name);

template <typename TMethod>
int UtilCreateChildProcess(const char* ChildName, TMethod&& ChildFunction, std::optional<int> CloneFlags = {})

/*++

Routine Description:

    This routine create child process to run the specified function.

Arguments:

    ChildName - Supplies the child thread name.

    ChildFunction - Supplies a function to be executed in the child process.

    CloneFlags - Supplies an optional value containing flags to use for the clone syscall.
        If no flags are specified, fork is used instead.

Return Value:

    The pid of the child process on success, -1 on failure. The child process does not return.

--*/

{
    int ChildPid;

    if (CloneFlags)
    {
        ChildPid = CLONE(CloneFlags.value());
    }
    else
    {
        ChildPid = fork();
    }

    if (ChildPid < 0)
    {
        LOG_ERROR("{} for {} failed {}", CloneFlags ? "clone" : "fork", ChildName, errno);
        return -1;
    }
    else if (ChildPid > 0)
    {
        return ChildPid;
    }

    try
    {
        UtilSetThreadName(ChildName);
        ChildFunction();
    }
    CATCH_LOG()

    _exit(1);
}

int UtilCreateProcessAndWait(const char* File, const char* const Argv[], int* Status = nullptr, const std::map<std::string, std::string>& Env = {});

template <typename TMethod>
void UtilCreateWorkerThread(const char* Name, TMethod&& ThreadFunction)
{
    std::promise<void> Promise;
    std::thread([ThreadFunction = std::move(ThreadFunction), &Promise, Name]() mutable {
        try
        {
            UtilSetThreadName(Name);

            int Result = unshare(CLONE_FS);
            Promise.set_value();
            THROW_LAST_ERROR_IF(Result < 0);

            ThreadFunction();
        }
        CATCH_LOG()
    }).detach();

    // Wait for the thread to unshare the filesystem so the next call to setns can succeed.
    Promise.get_future().wait();
}

int UtilExecCommandLine(const char* CommandLine, std::string* Output = nullptr, int ExpectedStatus = 0, bool PrintError = true);

std::string UtilFindMount(const char* MountInfoFile, const char* Path, bool WinPath, size_t* PrefixLength);

std::optional<std::string> UtilGetEnv(const char* Name, char* Environment);

std::string UtilGetEnvironmentVariable(const char* Name);

int UtilGetFeatureFlags(const wsl::linux::WslDistributionConfig& Config);

std::optional<LX_MINI_INIT_NETWORKING_MODE> UtilGetNetworkingMode(void);

pid_t UtilGetPpid(pid_t Pid);

std::string UtilGetVmId(void);

void UtilInitGroups(const char* User, gid_t Gid);

void UtilInitializeMessageBuffer(std::vector<gsl::byte>& Buffer);

bool UtilIsAbsoluteWindowsPath(const char* Path);

size_t UtilIsPathPrefix(const char* Path, const char* Prefix, bool WinPath);

bool UtilIsUtilityVm(void);

int UtilListenVsockAnyPort(struct sockaddr_vm* Address, int Backlog, bool CloseOnExec = true);

int UtilMkdir(const char* Path, mode_t Mode);

int UtilMkdirPath(const char* Path, mode_t Mode, bool SkipLast = false);

int UtilMount(const char* Source, const char* Target, const char* Type, unsigned long MountFlags, const char* Options, std::optional<std::chrono::seconds> TimeoutSeconds = {});

int UtilMountOverlayFs(const char* Target, const char* Lower, unsigned long MountFlags = 0, std::optional<std::chrono::seconds> TimeoutSeconds = {});

int UtilOpenMountNamespace(void);

int UtilParseCgroupsLine(char* Line, char** SubsystemName, bool* Enabled);

std::string UtilParsePlan9MountSource(std::string_view MountOptions);

std::string UtilParseVirtiofsMountSource(std::string_view MountOptions);

std::vector<char> UtilParseWslEnv(char* NtEnvironment);

int UtilProcessChildExitCode(int Status, const char* Name, int ExpectedStatus = 0, bool PrintError = true);

ssize_t UtilRead(int Fd, void* Buffer, size_t BufferSize, int Timeout = -1);

ssize_t UtilReadBuffer(int Fd, std::vector<gsl::byte>& Buffer, int Timeout = -1);

std::string UtilReadFile(FILE* File);

std::vector<gsl::byte> UtilReadFileRaw(const char* Path, size_t MaxSize);

std::pair<std::optional<std::string>, std::optional<std::string>> UtilReadFlavorAndVersion(const char* Path);

ssize_t UtilReadMessageLxBus(int MessageFd, std::vector<gsl::byte>& Buffer, bool ShutdownOnDisconnect);

int UtilRestoreBlockedSignals();

int UtilSaveBlockedSignals(const sigset_t& NewMask);

int UtilSaveSignalHandlers(struct sigaction* SavedSignalActions);

int UtilSetSignalHandlers(struct sigaction* SavedSignalActions, bool Ignore);

void UtilSocketShutdown(int Fd, int How);

bool UtilSizeTAdd(size_t Left, size_t Right, size_t* Out);

std::string_view UtilStringNextToken(std::string_view& View, std::string_view Separators);

std::string_view UtilStringNextToken(std::string_view& View, char Separator);

std::optional<std::string> UtilTranslatePathList(char* PathList, bool IsNtPathList);

std::string UtilWinPathTranslate(const char* Path, bool Reverse);

std::string UtilWinPathTranslateInternal(const char* Path, bool Reverse);

ssize_t UtilWriteBuffer(int Fd, gsl::span<const gsl::byte> Buffer);

ssize_t UtilWriteBuffer(int Fd, const void* Buffer, size_t BufferSize);

ssize_t UtilWriteStringView(int Fd, std::string_view StringView);

std::wstring UtilReadFileContentW(std::string_view path);

std::string UtilReadFileContent(std::string_view path);

uint16_t UtilWinAfToLinuxAf(uint16_t AddressFamily);

int WriteToFile(const char* Path, const char* Content, int permissions = 0644);

int ProcessCreateProcessMessage(wsl::shared::SocketChannel& channel, gsl::span<gsl::byte> Buffer);