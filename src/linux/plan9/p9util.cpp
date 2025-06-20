// Copyright (C) Microsoft Corporation. All rights reserved.
#include "precomp.h"
#include "p9util.h"
#include <pwd.h>
#include <grp.h>
#include <syscall.h>

#define _LINUX_CAPABILITY_VERSION_3 0x20080522
#define CAP_FOWNER 3
#define CAP_TO_INDEX(Cap) ((Cap) >> 5)
#define CAP_TO_MASK(Cap) (1 << ((Cap) & 31))

struct cap_user_header_t
{
    std::uint32_t version;
    pid_t pid;
};

struct cap_user_data_t
{
    std::uint32_t effective;
    std::uint32_t permitted;
    std::uint32_t inheritable;
};

inline int sys_setresuid(uid_t ruid, uid_t euid, uid_t suid)
{
    return syscall(SYS_setresuid, ruid, euid, suid);
}

inline int sys_setresgid(gid_t rgid, gid_t egid, gid_t sgid)
{
    return syscall(SYS_setresgid, rgid, egid, sgid);
}

inline int sys_faccessat(int dirFd, const char* pathName, int mode, int flags)
{
    return syscall(SYS_faccessat, dirFd, pathName, mode, flags);
}

inline int sys_setgroups(size_t size, const gid_t* list)
{
    return syscall(SYS_setgroups, size, list);
}

constexpr long c_PasswordFileBufferSize = 1024;

namespace p9fs::util {

Expected<wil::unique_fd> OpenAt(int dirfd, const std::string& name, int openFlags, mode_t mode)
{
    if (name.length() == 0)
    {
        return Reopen(dirfd, openFlags);
    }

    int fd = openat(dirfd, name.c_str(), openFlags | O_CLOEXEC, mode);
    if (fd < 0)
    {
        return LxError{-errno};
    }

    return wil::unique_fd{fd};
}

Expected<wil::unique_fd> Reopen(int fd, int openFlags)
{
    const char* pathToOpen;
    std::string targetPath;
    char fdPath[PATH_MAX];

    // If O_NOFOLLOW is set, open the target of the link in proc directly,
    // otherwise the call will always fail.
    if (WI_IsFlagSet(openFlags, O_NOFOLLOW))
    {
        targetPath = GetFdPath(fd);
        pathToOpen = targetPath.c_str();
    }
    else
    {
        snprintf(fdPath, sizeof(fdPath), "/proc/self/fd/%d", fd);
        pathToOpen = fdPath;
    }

    int newFd = open(pathToOpen, openFlags | O_CLOEXEC);
    if (newFd < 0)
    {
        return LxError{-errno};
    }

    return wil::unique_fd{newFd};
}

std::string GetFdPath(int fd)
{
    char fdPath[PATH_MAX]{};
    snprintf(fdPath, sizeof(fdPath), "/proc/self/fd/%d", fd);
    char target[PATH_MAX]{};
    const int result = readlink(fdPath, target, sizeof(target));
    THROW_LAST_ERROR_IF(result < 0);
    return {target, static_cast<size_t>(result)};
}

LX_INT LinuxErrorFromCaughtException()
{
    return -wil::ResultFromCaughtException();
}

LX_INT AccessHelper(int fd, const std::string& path, int mode)
{
    const char* pathToCheck = path.c_str();
    std::string fdPath;
    if (path.length() == 0)
    {
        // AT_EMPTY_PATH is not supported by faccessat, so get the full path
        // to the target if an access check is to be performed on the specified
        // directory.
        fdPath = GetFdPath(fd);
        pathToCheck = fdPath.c_str();
    }

    // The musl wrapper incorrectly blocks AT_SYMLINK_NOFOLLOW, so call the syscall directly.
    int result = sys_faccessat(fd, pathToCheck, mode, AT_SYMLINK_NOFOLLOW | AT_EACCESS);
    if (result < 0)
    {
        return -errno;
    }

    return {};
}

LX_INT CheckFOwnerCapability()
{
    cap_user_header_t header{};
    cap_user_data_t data[2]{};
    header.version = _LINUX_CAPABILITY_VERSION_3;
    int result = syscall(SYS_capget, &header, data);
    if (result < 0)
    {
        return -errno;
    }

    if (WI_IsFlagSet(data[CAP_TO_INDEX(CAP_FOWNER)].effective, CAP_TO_MASK(CAP_FOWNER)))
    {
        return {};
    }

    return LX_EPERM;
}

gid_t GetUserGroupId(uid_t uid)
{
    long size = sysconf(_SC_GETPW_R_SIZE_MAX);
    if (size < 0)
    {
        size = c_PasswordFileBufferSize;
    }

    std::vector<char> buffer;
    struct passwd pwd;
    struct passwd* result;
    for (;;)
    {
        buffer.resize(size);
        if (getpwuid_r(uid, &pwd, buffer.data(), size, &result) < 0)
        {
            if (errno != ERANGE)
            {
                return c_InvalidGid;
            }

            size += c_PasswordFileBufferSize;
            continue;
        }

        break;
    }

    if (result == nullptr)
    {
        return c_InvalidGid;
    }

    return result->pw_gid;
}

gid_t GetGroupIdByName(const char* name)
{
    long size = sysconf(_SC_GETGR_R_SIZE_MAX);
    if (size < 0)
    {
        size = c_PasswordFileBufferSize;
    }

    std::vector<char> buffer;
    struct group grp;
    struct group* result;
    for (;;)
    {
        buffer.resize(size);
        if (getgrnam_r(name, &grp, buffer.data(), size, &result) < 0)
        {
            if (errno != ERANGE)
            {
                return c_InvalidGid;
            }

            size += c_PasswordFileBufferSize;
            continue;
        }

        break;
    }

    if (result == nullptr)
    {
        return c_InvalidGid;
    }

    return result->gr_gid;
}

// Sets the effective uid and gid of the thread to the specified values.
FsUserContext::FsUserContext(uid_t uid, gid_t gid, const std::vector<gid_t>& groups)
{
    if (!groups.empty())
    {
        THROW_LAST_ERROR_IF(sys_setgroups(groups.size(), groups.data()) < 0);
        m_restoreGroups = true;
    }

    if (uid != c_InvalidUid)
    {
        m_Restore = true;
        // Use the syscall directly since the wrappers change the value on all threads.
        // Set the GID first since the capability to do that is lost once the UID changes to non-root.
        THROW_LAST_ERROR_IF(sys_setresgid(c_InvalidGid, gid, c_InvalidGid) < 0);
        THROW_LAST_ERROR_IF(sys_setresuid(c_InvalidUid, uid, c_InvalidUid) < 0);
    }
}

// Restores the effective uid and gid to root.
FsUserContext::~FsUserContext()
{
    if (m_Restore)
    {
        // Use the syscall directly since the wrappers change the value on all threads.
        THROW_LAST_ERROR_IF(sys_setresuid(-1, 0, -1) < 0);
        THROW_LAST_ERROR_IF(sys_setresgid(c_InvalidGid, 0, c_InvalidGid) < 0);
    }

    if (m_restoreGroups)
    {
        THROW_LAST_ERROR_IF(sys_setgroups(0, nullptr) < 0);
    }
}

} // namespace p9fs::util
