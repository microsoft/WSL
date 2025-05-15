// Copyright (C) Microsoft Corporation. All rights reserved.
#include "precomp.h"
#include "p9errors.h"
#include "p9file.h"
#include "p9util.h"
#include "p9commonutil.h"
#include "p9xattr.h"
#include <mountutilcpp.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>

using namespace std::string_view_literals;

namespace p9fs {

constexpr std::string_view c_drvfsFsType = "drvfs"sv;
constexpr std::string_view c_p9FsType = "9p"sv;
constexpr std::string_view c_virtioFsType = "virtiofs"sv;

struct OpenFlagMapping
{
    OpenFlags P9Flag;
    int LinuxFlag;
};

const OpenFlagMapping c_openFlagsMapping[] = {
    {OpenFlags::WriteOnly, O_WRONLY},
    {OpenFlags::ReadWrite, O_RDWR},
    {OpenFlags::Create, O_CREAT},
    {OpenFlags::Exclusive, O_EXCL},
    {OpenFlags::NoCTTY, O_NOCTTY},
    {OpenFlags::Truncate, O_TRUNC},
    {OpenFlags::Append, O_APPEND},
    {OpenFlags::NonBlock, O_NONBLOCK},
    {OpenFlags::DSync, O_DSYNC},
    {OpenFlags::FAsync, O_ASYNC},
    {OpenFlags::Direct, O_DIRECT},
    {OpenFlags::LargeFile, O_LARGEFILE},
    {OpenFlags::Directory, O_DIRECTORY},
    {OpenFlags::NoFollow, O_NOFOLLOW},
    {OpenFlags::NoAccessTime, O_NOATIME},
    {OpenFlags::CloseOnExec, O_CLOEXEC},
    {OpenFlags::Sync, O_SYNC}};

Expected<std::tuple<std::shared_ptr<Fid>, Qid>> CreateFile(std::shared_ptr<const IRoot> root, LX_UID_T uid)
{
    auto realRoot = std::static_pointer_cast<const Root>(root);
    auto file = std::make_shared<File>(realRoot);
    auto qid = file->Initialize();
    if (!qid)
    {
        return qid.Unexpected();
    }

    return std::tuple<std::shared_ptr<Fid>, Qid>{std::move(file), qid.Get()};
}

QidType ModeToQidType(mode_t mode)
{
    if (S_ISLNK(mode))
    {
        return QidType::Symlink;
    }

    if (S_ISDIR(mode))
    {
        return QidType::Directory;
    }

    return QidType::File;
}

// Converts the result of a stat system call to a qid value.
Qid StatToQid(const struct stat& st)
{
    return {st.st_ino, 0, ModeToQidType(st.st_mode)};
}

// Get the qid for a file.
// N.B. The caller is responsible for setting the right thread uid/gid before calling this.
Expected<Qid> GetFileQidByPath(int fd, const std::string& path)
{
    struct stat st;
    int result = fstatat(fd, path.c_str(), &st, AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH);
    if (result < 0)
    {
        return LxError{-errno};
    }

    return StatToQid(st);
}

// Appends a valid Linux path segment to a Win32 path. It's assumed that the path has already been
// scanned for internal NUL and / characters.
void AppendPath(std::string& Base, std::string_view Name)
{
    // No need for a delimiter if the base path is empty or already ends in one.
    if (!Base.empty() && Base.back() != '/')
    {
        Base += '/';
    }

    Base += Name;
}

// Converts 9P2000.L open flags to Linux open flags.
// N.B. 9P2000.L and Linux flag values may be identical on some platforms, but not all.
int OpenFlagsToLinuxFlags(OpenFlags flags)
{
    // Since OpenFlags::ReadOnly is zero, it's omitted from the mapping array. This is safe as long as O_RDONLY is also
    // zero. If it's not, it would have to be handled separately.
    static_assert(O_RDONLY == 0);

    int result = 0;
    for (const auto& flag : c_openFlagsMapping)
    {
        if (WI_AreAllFlagsSet(flags, flag.P9Flag))
        {
            WI_SetAllFlags(result, flag.LinuxFlag);
        }
    }

    return result;
}

// Get the stat information of this file.
// N.B. The caller is responsible for setting the right thread uid/gid before calling this.
Expected<struct stat> File::Stat()
{
    struct stat st;
    // Acquire the lock to prevent the file name from changing.
    std::shared_lock<std::shared_mutex> lock{m_Lock};
    int result = fstatat(m_Root->RootFd, m_FileName.c_str(), &st, AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH);
    if (result < 0)
    {
        return LxError{-errno};
    }

    return st;
}

bool File::IsOnRoot(const std::shared_ptr<const IRoot>& root)
{
    return m_Root == root;
}

// Opens the file.
// N.B. The caller is responsible for setting the right thread uid/gid before calling this.
Expected<wil::unique_fd> File::OpenFile(int openFlags)
{
    // Acquire the lock to prevent the file name from changing.
    std::shared_lock<std::shared_mutex> lock{m_Lock};
    return util::OpenAt(m_Root->RootFd, m_FileName, openFlags | O_NOFOLLOW);
}

// Validates that this file exists and sets the m_Qid member.
// N.B. The caller is responsible for setting the right thread uid/gid before calling this.
LX_INT File::ValidateExists()
{
    auto st = Stat();
    if (!st)
    {
        return st.Error();
    }

    m_Qid = StatToQid(st.Get());
    m_Device = st->st_dev;
    return {};
}

// Initializes a file to a Win32 path.
Expected<Qid> File::Initialize()
{
    util::FsUserContext userContext{m_Root->Uid, m_Root->Gid, m_Root->Groups};
    LX_INT error = ValidateExists();
    if (error < 0)
    {
        return LxError{error};
    }

    // No locking needed because initialize is only called on fids not yet
    // reachable by other threads.
    return m_Qid;
}

File::File(std::shared_ptr<const Root> root) : m_Root{root}
{
}

// Copies a file. This does not clone the open file state, just the name and qid.
File::File(const File& file) : m_FileName{file.m_FileName}, m_Root{file.m_Root}, m_Qid{file.m_Qid}
{
}

// Updates the path to a child file entry in a directory. Must be called with a newly
// constructed file, not one that has been opened.
Expected<Qid> File::Walk(std::string_view name)
{
    // TODO: This is not safe if walk is called multiple times. While
    // we verify that the item is not a symlink in this step, the file could've
    // been replaced with a symlink since the qid was determined.
    // The only way to make this foolproof is to open an fd for every file, and
    // use fstatat for the next level. A chroot environment can be used to
    // prevent the links from escaping the share root, but it can't avoid
    // accidentally following links at all.
    if (!WI_IsFlagSet(m_Qid.Type, QidType::Directory))
    {
        return LxError{LX_ENOTDIR};
    }

    // No lock is taken here; this function is only called on fid's that have
    // not yet been inserted in the list and are therefore not reachable from
    // other threads.
    AppendPath(m_FileName, name);

    // Revert to the old info on error.
    const auto oldQid = m_Qid;
    const auto oldDevice = m_Device;
    auto restoreName = wil::scope_exit([&]() {
        m_Qid = oldQid;
        m_Device = oldDevice;
        const auto index = m_FileName.find_last_of('/');
        if (index == std::string::npos)
        {
            m_FileName.resize(0);
        }
        else
        {
            m_FileName.resize(index);
        }
    });

    // TODO: Maybe handle multiple items in a single walk call so changing ids is done only once.
    util::FsUserContext userContext{m_Root->Uid, m_Root->Gid, m_Root->Groups};
    const auto parentDevice = m_Device;
    LX_INT err = ValidateExists();
    if (err != 0)
    {
        return LxError{err};
    }

    // Check if this is a mount point, and if so if it's a drvfs or 9p mount.
    if (parentDevice != m_Device)
    {
        try
        {
            // Because this thread might not be in the same mount namespace than the rest of the process,
            // look at /proc/<tid>/mountinfo instead of /proc/self/
            const std::string mountInfoPath = std::format("/proc/{}/mountinfo", gettid());
            mountutil::MountEnum mountEnum(mountInfoPath.c_str());
            bool found = mountEnum.FindMount([this](auto entry) { return entry.Device == m_Device; });

            // If the mount was found and it's a drvfs mount, deny access.
            if (found && (mountEnum.Current().FileSystemType == c_drvfsFsType || mountEnum.Current().FileSystemType == c_p9FsType ||
                          mountEnum.Current().FileSystemType == c_virtioFsType))
            {
                return LxError{LX_EACCES};
            }
        }
        CATCH_LOG()
    }

    restoreName.release();
    return m_Qid;
}

// Reads the attributes of a file or directory.
Expected<std::tuple<UINT64, Qid, StatResult>> File::GetAttr(UINT64 mask)
{
    std::string fileName;
    Qid qid;
    {
        // Retrieve the qid and open a handle under lock.
        std::shared_lock<std::shared_mutex> lock{m_Lock};
        qid = m_Qid;
        fileName = m_FileName;
    }

    util::FsUserContext userContext{m_Root->Uid, m_Root->Gid, m_Root->Groups};
    struct stat stat;
    int error = fstatat(m_Root->RootFd, fileName.c_str(), &stat, AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH);
    if (error < 0)
    {
        return LxError{-errno};
    }

    StatResult result{};
    UINT64 valid = GetAttrIno;
    if (WI_IsFlagSet(mask, GetAttrMode))
    {
        result.Mode = stat.st_mode;
        WI_SetFlag(valid, GetAttrMode);
    }

    if (WI_IsFlagSet(mask, GetAttrNlink))
    {
        result.NLink = stat.st_nlink;
        WI_SetFlag(valid, GetAttrNlink);
    }

    if (WI_IsFlagSet(mask, GetAttrRdev))
    {
        result.RDev = stat.st_rdev;
        WI_SetFlag(valid, GetAttrRdev);
    }

    if (WI_IsFlagSet(mask, GetAttrSize))
    {
        result.Size = stat.st_size;
        WI_SetFlag(valid, GetAttrSize);
    }

    if (WI_IsFlagSet(mask, GetAttrBlocks))
    {
        result.BlockSize = stat.st_blksize;
        result.Blocks = stat.st_blocks;
        WI_SetFlag(valid, GetAttrBlocks);
    }

    if (WI_IsFlagSet(mask, GetAttrAtime))
    {
        result.AtimeSec = stat.st_atim.tv_sec;
        result.AtimeNsec = stat.st_atim.tv_nsec;
        WI_SetFlag(valid, GetAttrAtime);
    }

    if (WI_IsFlagSet(mask, GetAttrMtime))
    {
        result.MtimeSec = stat.st_mtim.tv_sec;
        result.MtimeNsec = stat.st_mtim.tv_nsec;
        WI_SetFlag(valid, GetAttrMtime);
    }

    if (WI_IsFlagSet(mask, GetAttrCtime))
    {
        result.CtimeSec = stat.st_ctim.tv_sec;
        result.CtimeNsec = stat.st_ctim.tv_nsec;
        WI_SetFlag(valid, GetAttrCtime);
    }

    if (WI_IsFlagSet(mask, GetAttrUid))
    {
        result.Uid = stat.st_uid;
        WI_SetFlag(valid, GetAttrUid);
    }

    if (WI_IsFlagSet(mask, GetAttrGid))
    {
        result.Gid = stat.st_gid;
        WI_SetFlag(valid, GetAttrGid);
    }

    return std::make_tuple(valid, qid, result);
}

// Sets the attributes for a file or directory.
LX_INT File::SetAttr(UINT32 valid, const StatResult& stat)
{
    if (m_Root->ReadOnly())
    {
        return LX_EROFS;
    }

    util::FsUserContext userContext{m_Root->Uid, m_Root->Gid, m_Root->Groups};

    // Multiple operations may be performed, so it would be preferable to open the file. However,
    // most operations don't support O_PATH and any other flags will check for permissions that the
    // operation may not need.
    const auto fileName = GetFileName();

    // Ctime is updated by most of the operations below, so don't explicitly
    // update it if not needed.
    bool needCTimeUpdate = WI_IsFlagSet(valid, SetAttrCtime);

    if (WI_IsFlagSet(valid, SetAttrSize))
    {
        // Open the file to truncate because truncate will always follow symlinks and there is no
        // ftruncateat.
        auto file = OpenFile(O_WRONLY);
        if (!file)
        {
            return file.Error();
        }

        int error = ftruncate(file->get(), stat.Size);
        if (error < 0)
        {
            return -errno;
        }

        needCTimeUpdate = false;
    }

    if (WI_IsFlagSet(valid, SetAttrMode))
    {
        int error = fchmodat(m_Root->RootFd, fileName.c_str(), stat.Mode, AT_SYMLINK_NOFOLLOW);
        if (error < 0)
        {
            return -errno;
        }

        needCTimeUpdate = false;
    }

    if (WI_IsAnyFlagSet(valid, SetAttrUid | SetAttrGid))
    {
        uid_t uid = WI_IsFlagSet(valid, SetAttrUid) ? stat.Uid : -1;
        uid_t gid = WI_IsFlagSet(valid, SetAttrGid) ? stat.Gid : -1;
        int error = fchownat(m_Root->RootFd, fileName.c_str(), uid, gid, AT_SYMLINK_NOFOLLOW);
        if (error < 0)
        {
            return -errno;
        }

        needCTimeUpdate = false;
    }

    if (WI_IsAnyFlagSet(valid, SetAttrAtime | SetAttrMtime))
    {
        struct timespec times[2]{{0, UTIME_OMIT}, {0, UTIME_OMIT}};

        //
        // For atime and mtime, the time is set to the current time unless the
        // respective "set" flag is set.
        //

        if (WI_IsFlagSet(valid, SetAttrAtime))
        {
            if (WI_IsFlagSet(valid, SetAttrAtimeSet))
            {
                times[0].tv_sec = stat.AtimeSec;
                times[0].tv_nsec = stat.AtimeNsec;
            }
            else
            {
                times[0].tv_nsec = UTIME_NOW;
            }
        }

        if (WI_IsFlagSet(valid, SetAttrMtime))
        {
            if (WI_IsFlagSet(valid, SetAttrMtimeSet))
            {
                times[1].tv_sec = stat.MtimeSec;
                times[1].tv_nsec = stat.MtimeNsec;
            }
            else
            {
                times[1].tv_nsec = UTIME_NOW;
            }
        }

        int error = utimensat(m_Root->RootFd, fileName.c_str(), times, AT_SYMLINK_NOFOLLOW);
        if (error < 0)
        {
            return -errno;
        }

        needCTimeUpdate = false;
    }

    // If a ctime update was requested but didn't already happen, perform a no-op
    // operation that has a ctime update as a side-effect.
    if (needCTimeUpdate)
    {
        int error = fchownat(m_Root->RootFd, fileName.c_str(), -1, -1, AT_SYMLINK_NOFOLLOW);
        if (error < 0)
        {
            return -errno;
        }
    }

    return {};
}

// Opens a file or directory for read/write access.
Expected<Qid> File::Open(OpenFlags flags)
{
    // Acquire the lock to protect the file name and to guard against
    // concurrent open attempts.
    std::lock_guard<std::shared_mutex> lock{m_Lock};
    if (IsOpen())
    {
        return LxError{LX_EINVAL};
    }

    WI_ClearFlag(flags, OpenFlags::Create);
    util::FsUserContext userContext{m_Root->Uid, m_Root->Gid, m_Root->Groups};
    // Don't use OpenHandle because the lock is already held.
    auto file{util::OpenAt(m_Root->RootFd, m_FileName, OpenFlagsToLinuxFlags(flags) | O_NOFOLLOW)};
    if (!file)
    {
        return file.Unexpected();
    }

    m_Io = CoroutineIoIssuer(file->get());
    m_File = std::move(file.Get());
    return m_Qid;
}

// Creates a file in a directory, updating this object to point to the new file.
Expected<Qid> File::Create(std::string_view name, OpenFlags flags, UINT32 mode, UINT32 /* gid */)
{
    // Acquire the lock exclusive because the file name will be modified,
    // and to protect against concurrent opens and creates.
    std::lock_guard<std::shared_mutex> lock{m_Lock};
    if (IsOpen())
    {
        return LxError{LX_EINVAL};
    }

    if (m_Root->ReadOnly())
    {
        return LxError{LX_EROFS};
    }

    // The specified gid is currently ignored. Supporting it would be possible, but it would be
    // necessary to make sure that the user is a member of the specified group.
    util::FsUserContext userContext{m_Root->Uid, m_Root->Gid, m_Root->Groups};
    auto newFileName = ChildPathWithLockHeld(name);
    auto file{util::OpenAt(m_Root->RootFd, newFileName, OpenFlagsToLinuxFlags(flags) | O_CREAT | O_NOFOLLOW, mode)};
    if (!file)
    {
        return file.Unexpected();
    }

    struct stat st;
    int result = fstat(file->get(), &st);
    if (result < 0)
    {
        return LxError{-errno};
    }

    m_FileName = std::move(newFileName);
    m_Io = CoroutineIoIssuer(file->get());
    m_File = std::move(file.Get());
    m_Qid = StatToQid(st);
    m_Device = st.st_dev;
    return m_Qid;
}

// Creates a subdirectory.
Expected<Qid> File::MkDir(std::string_view name, UINT32 mode, UINT32 /* gid */)
{
    const auto newFileName = ChildPath(name);

    // The specified gid is currently ignored. Supporting it would be possible, but it would be
    // necessary to make sure that the user is a member of the specified group.
    util::FsUserContext userContext{m_Root->Uid, m_Root->Gid, m_Root->Groups};
    int result = mkdirat(m_Root->RootFd, newFileName.c_str(), mode);
    if (result < 0)
    {
        return LxError{-errno};
    }

    return GetFileQidByPath(m_Root->RootFd, newFileName);
}

// Reads the contents of a directory, starting at the specified offset.
LX_INT File::ReadDir(UINT64 offset, SpanWriter& writer, bool includeAttributes)
{
    if (!IsOpen())
    {
        return LX_EBADF;
    }

    // Acquire an exclusive lock to protect enumerator state.
    std::lock_guard<std::shared_mutex> lock{m_Lock};
    if (!m_Enumerator)
    {
        m_Enumerator.reset(new DirectoryEnumerator(m_File.get()));
        // The fd is now owned by the enumerator.
        m_File.release();
    }

    m_Enumerator->Seek(offset);

    bool dirEntriesWritten = false;
    for (;;)
    {
        auto entry = m_Enumerator->Next();
        if (entry == nullptr)
        {
            break;
        }

        StatResult attributes;
        StatResult* attributesToUse = nullptr;
        if (includeAttributes)
        {
            attributesToUse = &attributes;
            struct stat st;
            const char* name = entry->d_name;

            // Return attributes of the directory for both . and ..
            if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
            {
                name = "";
            }

            int result = fstatat(m_Enumerator->Fd(), name, &st, AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH);
            if (result < 0)
            {
                // Fill out basic attributes if real attributes can't be determined.
                attributes = {};
                attributes.Mode = util::DirEntryTypeToMode(entry->d_type);
                attributes.NLink = 1;
            }
            else
            {
                attributes.Mode = st.st_mode;
                attributes.Uid = st.st_uid;
                attributes.Gid = st.st_gid;
                attributes.NLink = st.st_nlink;
                attributes.RDev = st.st_rdev;
                attributes.Size = st.st_size;
                attributes.BlockSize = st.st_blksize;
                attributes.Blocks = st.st_blocks;
                attributes.AtimeSec = st.st_atim.tv_sec;
                attributes.AtimeNsec = st.st_atim.tv_nsec;
                attributes.MtimeSec = st.st_mtim.tv_sec;
                attributes.MtimeNsec = st.st_mtim.tv_nsec;
                attributes.CtimeSec = st.st_ctim.tv_sec;
                attributes.CtimeNsec = st.st_ctim.tv_nsec;
            }
        }

        Qid qid{};
        qid.Path = entry->d_ino;
        qid.Type = util::DirEntryTypeToQidType(entry->d_type);
        if (!util::SpanWriteDirectoryEntry(writer, entry->d_name, qid, entry->d_off, entry->d_type, attributesToUse))
        {
            if (!dirEntriesWritten)
            {
                return LX_EINVAL;
            }

            break;
        }

        dirEntriesWritten = true;
    }

    return {};
}

// Reads the contents of an open file.
Task<Expected<UINT32>> File::Read(UINT64 offset, gsl::span<gsl::byte> buffer)
{
    // No locking needed; once open, the file will not be closed until the
    // object is destructed, and the caller holds a reference.
    if (!m_File)
    {
        co_return LxError{LX_EBADF};
    }

    CancelToken token;
    auto result = co_await ReadAsync(m_Io, offset, buffer, token);
    if (result.Error != 0 && result.Error != LX_EOVERFLOW)
    {
        co_return LxError{result.Error};
    }

    co_return static_cast<UINT32>(result.BytesTransferred);
}

// Writes to an open file.
Task<Expected<UINT32>> File::Write(UINT64 offset, gsl::span<const gsl::byte> buffer)
{
    // Since the file could not have been opened for write on a read-only file
    // system, there is no reason to check that here.

    // No locking needed; once open, the file will not be closed until the
    // object is destructed, and the caller holds a reference.
    if (!m_File)
    {
        co_return LxError{LX_EBADF};
    }

    CancelToken token;
    auto result = co_await WriteAsync(m_Io, offset, buffer, token);
    if (result.Error != 0)
    {
        co_return LxError{result.Error};
    }

    co_return result.BytesTransferred;
}

// Unlinks a directory entry.
LX_INT File::UnlinkAt(std::string_view name, UINT32 flags)
{
    if (m_Root->ReadOnly())
    {
        return LX_EROFS;
    }

    const auto fileName = ChildPath(name);
    util::FsUserContext userContext{m_Root->Uid, m_Root->Gid, m_Root->Groups};

    // TODO: it's unclear whether this is the correct usage of the
    // flags field. The Windows implementation unlinks either directory or
    // file regardless of flags.
    int result = unlinkat(m_Root->RootFd, fileName.c_str(), flags);
    if (result < 0)
    {
        return -errno;
    }

    return {};
}

// Removes the directory entry represented by the current fid.
LX_INT File::Remove()
{
    if (m_Root->ReadOnly())
    {
        return LX_EROFS;
    }

    int flags = 0;
    WI_SetFlagIf(flags, AT_REMOVEDIR, WI_IsFlagSet(m_Qid.Type, QidType::Directory));
    const std::string fileName = GetFileName();
    if (fileName.length() == 0)
    {
        // Can't unlink the root.
        return LX_EPERM;
    }

    util::FsUserContext userContext{m_Root->Uid, m_Root->Gid, m_Root->Groups};
    const int result = unlinkat(m_Root->RootFd, fileName.c_str(), flags);
    if (result < 0)
    {
        return -errno;
    }

    return {};
}

// Gets a copy of the file name, taking the lock to retrieve it.
std::string File::GetFileName() const
{
    std::shared_lock<std::shared_mutex> lock{m_Lock};
    return m_FileName;
}

// Constructs a child path of the current path from a valid Linux path segment.
std::string File::ChildPath(std::string_view name)
{
    auto result = GetFileName();
    AppendPath(result, name);
    return result;
}

std::string File::ChildPathWithLockHeld(std::string_view name)
{
    std::string result{m_FileName};
    AppendPath(result, name);
    return result;
}

// Renames a directory entry.
LX_INT File::RenameAt(std::string_view oldName, Fid& newParent, std::string_view newName)
{
    if (!newParent.IsFile() || !newParent.IsOnRoot(m_Root))
    {
        return LX_EINVAL;
    }

    if (m_Root->ReadOnly())
    {
        return LX_EROFS;
    }

    auto newParentFile = static_cast<File&>(newParent);
    const auto oldPath = ChildPath(oldName);
    const auto newPath = newParentFile.ChildPath(newName);
    util::FsUserContext userContext{m_Root->Uid, m_Root->Gid, m_Root->Groups};
    int result = renameat(m_Root->RootFd, oldPath.c_str(), m_Root->RootFd, newPath.c_str());
    if (result < 0)
    {
        return -errno;
    }

    return {};
}

// Renames the current directory entry.
LX_INT File::Rename(Fid& newParent, std::string_view newName)
{
    if (!newParent.IsFile() || !newParent.IsOnRoot(m_Root))
    {
        return LX_EINVAL;
    }

    if (m_Root->ReadOnly())
    {
        return LX_EROFS;
    }

    auto newParentFile = static_cast<File&>(newParent);
    // Take an exclusive lock because the file name will be changed.
    std::lock_guard<std::shared_mutex> lock{m_Lock};
    if (m_FileName.length() == 0)
    {
        // Can't rename the root.
        return LX_EPERM;
    }

    const auto oldPath = m_FileName;
    auto newPath = newParentFile.ChildPath(newName);
    util::FsUserContext userContext{m_Root->Uid, m_Root->Gid, m_Root->Groups};
    int result = renameat(m_Root->RootFd, oldPath.c_str(), m_Root->RootFd, newPath.c_str());
    if (result < 0)
    {
        return -errno;
    }

    m_FileName = newPath;
    return {};
}

// Creates a symbolic link in a directory.
Expected<Qid> File::SymLink(std::string_view name, std::string_view target, UINT32 /* gid */)
{
    if (m_Root->ReadOnly())
    {
        return LxError{LX_EROFS};
    }

    // TODO: Gid is being ignored.
    const auto linkName = ChildPath(name);
    // Need a null-terminated string:
    const std::string linkTarget{target.data(), target.size()};

    // The specified gid is currently ignored. Supporting it would be possible, but it would be
    // necessary to make sure that the user is a member of the specified group.
    util::FsUserContext userContext{m_Root->Uid, m_Root->Gid, m_Root->Groups};
    int result = symlinkat(linkTarget.c_str(), m_Root->RootFd, linkName.c_str());
    if (result < 0)
    {
        return LxError{-errno};
    }

    return GetFileQidByPath(m_Root->RootFd, linkName);
}

// Reads the target of a symbolic link.
Expected<UINT32> File::ReadLink(gsl::span<char> name)
{
    const auto fileName = GetFileName();
    util::FsUserContext userContext{m_Root->Uid, m_Root->Gid, m_Root->Groups};
    int result = readlinkat(m_Root->RootFd, fileName.c_str(), name.data(), name.size());
    if (result < 0)
    {
        return LxError{-errno};
    }

    return result;
}

// Creates a hard link in a directory to another file.
LX_INT File::Link(std::string_view newName, Fid& target)
{
    if (!target.IsFile() || !target.IsOnRoot(m_Root))
    {
        return LX_EINVAL;
    }

    if (m_Root->ReadOnly())
    {
        return LX_EROFS;
    }

    const auto targetFile = static_cast<File&>(target);

    // Construct the new name relative to the share root.
    const auto newLinkName = ChildPath(newName);
    const auto targetName = targetFile.GetFileName();
    util::FsUserContext userContext{m_Root->Uid, m_Root->Gid, m_Root->Groups};
    int result = linkat(m_Root->RootFd, targetName.c_str(), m_Root->RootFd, newLinkName.c_str(), 0);
    if (result < 0)
    {
        return -errno;
    }

    return {};
}

// Creates a device object in a directory.
Expected<Qid> File::MkNod(std::string_view name, UINT32 mode, UINT32 major, UINT32 minor, UINT32 gid)
{
    if (m_Root->ReadOnly())
    {
        return LxError{LX_EROFS};
    }

    const auto path = ChildPath(name);

    // The specified gid is currently ignored. Supporting it would be possible, but it would be
    // necessary to make sure that the user is a member of the specified group.
    util::FsUserContext userContext{m_Root->Uid, m_Root->Gid, m_Root->Groups};
    int result = mknodat(m_Root->RootFd, path.c_str(), mode, makedev(major, minor));
    if (result < 0)
    {
        return LxError{-errno};
    }

    return GetFileQidByPath(m_Root->RootFd, path);
}

// Flushes a file's buffers.
LX_INT File::Fsync()
{
    if (!m_File)
    {
        return LX_EINVAL;
    }

    int result = fsync(m_File.get());
    if (result < 0)
    {
        return -errno;
    }

    return {};
}

// Retrieves the file system attributes.
Expected<StatFsResult> File::StatFs()
{
    // Open the file because there is no statfsat.
    auto file{OpenFile(O_PATH)};
    if (!file)
    {
        return file.Unexpected();
    }

    struct statfs statFs;
    util::FsUserContext userContext{m_Root->Uid, m_Root->Gid, m_Root->Groups};
    int result = fstatfs(file->get(), &statFs);
    if (result < 0)
    {
        return LxError{-errno};
    }

    StatFsResult statFsResult;
    statFsResult.Type = static_cast<UINT32>(statFs.f_type);
    statFsResult.BlockSize = static_cast<UINT32>(statFs.f_bsize);
    statFsResult.Blocks = statFs.f_blocks;
    statFsResult.BlocksFree = statFs.f_bfree;
    statFsResult.BlocksAvailable = statFs.f_bavail;
    statFsResult.Files = statFs.f_files;
    statFsResult.FilesFree = statFs.f_ffree;
    statFsResult.NameLength = static_cast<UINT32>(statFs.f_namelen);

    static_assert(sizeof(statFsResult.FsId) == sizeof(statFs.f_fsid));
    // These two fields should be the same size (asserted above).
    memcpy(&statFsResult.FsId, &statFs.f_fsid, sizeof(statFsResult.FsId));
    return statFsResult;
}

// Locks a range of the file.
Expected<LockStatus> File::Lock(LockType, UINT32, UINT64, UINT64, UINT32, const std::string_view)
{
    // The file has to be open for lock to work.
    if (!IsOpen())
    {
        return LxError{LX_EBADF};
    }

    // This implementation always returns success. The Linux kernel still
    // provides proper file locking, and this call seems to only be used
    // to check for server locking between multiple clients. That means a
    // no-op implementation works for a single client.
    //
    // TODO: Implement server-side locks.
    return LockStatus::Success;
}

// Gets information about the current lock on the file.
Expected<std::tuple<LockType, UINT64, UINT64, UINT32, std::string_view>> File::GetLock(
    LockType, UINT64 Start, UINT64 Length, UINT32 ProcId, const std::string_view ClientId)
{
    // The file has to be open for getlock to work.
    if (!IsOpen())
    {
        return LxError{LX_EBADF};
    }

    // This implementation always returns unlocked, and echoes the rest of
    // the values back to the client. The Linux kernel still provides
    // proper file locking, and returns the correct information even if the
    // server says unlocked. That means a no-op implementation works for a
    // single client.
    //
    // TODO: Implement server-side locks.
    return std::make_tuple(LockType::Unlock, Start, Length, ProcId, ClientId);
}

// Created a new Fid representing an extended attribute.
Expected<std::shared_ptr<XAttrBase>> File::XattrWalk(const std::string& name)
{
    // N.B. There is no *xattrat or equivalent, so f*xattr must be used
    // to avoid constructing the full file name. However, f*xattr doesn't work
    // on file descriptors opened with O_PATH, so they can't be used on symlinks,
    // even though the various l*xattr functions do allow manipulating xattrs
    // on symlinks. This means there's no way to support xattrs on symlinks
    // without using the full file name, which is less than ideal.
    // TODO: Use a chroot environment to make this safer.
    auto path = util::GetFdPath(m_Root->RootFd);
    AppendPath(path, GetFileName());
    std::shared_ptr<XAttrBase> xattr = std::make_shared<XAttr>(m_Root, path, name, XAttr::Access::Read);
    return xattr;
}

Expected<std::shared_ptr<XAttrBase>> File::XattrCreate(const std::string& name, UINT64 size, UINT32 flags)
{
    if (m_Root->ReadOnly())
    {
        return LxError{LX_EROFS};
    }

    // Since the caller will end up replacing the original fid with the one
    // returned, make sure this wasn't an open fid.
    if (IsOpen())
    {
        return LxError{LX_EINVAL};
    }

    // See above for the reason for doing this.
    auto path = util::GetFdPath(m_Root->RootFd);
    AppendPath(path, GetFileName());
    std::shared_ptr<XAttrBase> xattr = std::make_shared<XAttr>(m_Root, path, name, XAttr::Access::Write, size, flags);
    return xattr;
}

LX_INT File::Access(AccessFlags flags)
{
    AccessFlags flagsWithoutDelete = flags;
    WI_ClearFlag(flagsWithoutDelete, AccessFlags::Delete);
    const auto name = GetFileName();
    util::FsUserContext userContext{m_Root->Uid, m_Root->Gid, m_Root->Groups};
    LX_INT result = util::AccessHelper(m_Root->RootFd, name, static_cast<int>(flagsWithoutDelete));
    if (result < 0)
    {
        return result;
    }

    // No delete check requested? Done!
    if (!WI_IsFlagSet(flags, AccessFlags::Delete))
    {
        return {};
    }

    if (name.length() == 0)
    {
        // Can't delete the root.
        return LX_EACCES;
    }

    std::string parentPath;
    const int index = name.find_last_of('/');
    if (index != std::string::npos)
    {
        parentPath = name.substr(0, index);
    }

    // Check for write access to the parent.
    result = util::AccessHelper(m_Root->RootFd, parentPath, W_OK);
    if (result < 0)
    {
        return result;
    }

    // Get the parent's attributes.
    struct stat st;
    result = fstatat(m_Root->RootFd, parentPath.c_str(), &st, AT_EMPTY_PATH);
    if (result < 0)
    {
        return -errno;
    }

    // No sticky bit? Done!
    if (!WI_IsFlagSet(st.st_mode, S_ISVTX))
    {
        return {};
    }

    // Check if this process has CAP_FOWNER, which means it can bypass the
    // sticky bit.
    result = util::CheckFOwnerCapability();
    if (result == 0)
    {
        return {};
    }
    else if (result != LX_EPERM)
    {
        return result;
    }

    // Check for ownership of the parent directory.
    uid_t uid = geteuid();
    if (uid == st.st_uid)
    {
        return {};
    }

    // Check for ownership of the child.
    result = fstatat(m_Root->RootFd, name.c_str(), &st, AT_EMPTY_PATH);
    if (result < 0)
    {
        return -errno;
    }

    if (uid == st.st_uid)
    {
        return {};
    }

    // Stick bit checks failed.
    return LX_EACCES;
}

std::shared_ptr<Fid> File::Clone() const
{
    // Requires the lock to protect the file name.
    std::shared_lock<std::shared_mutex> lock{m_Lock};
    return std::make_shared<File>(*this);
}

bool File::IsOpen() const
{
    return bool{m_File} || m_Enumerator;
}

bool File::IsFile() const
{
    return true;
}

Qid File::GetQid() const
{
    return m_Qid;
}

} // namespace p9fs
