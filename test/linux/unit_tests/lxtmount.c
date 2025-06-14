/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    lxtmount.c

Abstract:

    This file contains mount primitive support.

--*/

#include "lxtlog.h"
#include <sys/mman.h>
#include "lxtcommon.h"
#include <sys/mount.h>
#include <linux/capability.h>
#include <sys/sysmacros.h>
#include <libgen.h>
#include <fcntl.h>
#include <stdlib.h>
#include <libmount/libmount.h>
#include <sys/mman.h>
#include "lxtmount.h"

#define PATH_MAX (4096)

void MountEscapeString(const char* Source, char* Dest, size_t Length);

int MountCheckIsMount(
    const char* Path,
    int ExpectedParentId,
    const char* ExpectedSource,
    const char* ExpectedFsType,
    const char* ExpectedRoot,
    const char* ExpectedMountOptions,
    const char* ExpectedFsOptions,
    const char* ExpectedCombinedOptions,
    int Flags)

/*++

Description:

    This routine checks if a path is a mount point.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns the mount ID on success, -1 on failure.

--*/

{

    int Direction;
    const char* ExpectedSourceActual;
    struct libmnt_fs* FileSystem;
    char LocalPath[PATH_MAX];
    int MountId;
    int Result;
    struct stat Stat;
    struct libmnt_table* Table;

    Table = NULL;
    if (strcmp(Path, "/") != 0)
    {
        LxtCheckResult(MountIsMount(AT_FDCWD, Path));
        if (Result == 0)
        {
            LxtLogError("%s is not a mount point.", Path);
            Result = LXT_RESULT_FAILURE;
            goto ErrorExit;
        }
    }

    LxtCheckErrnoZeroSuccess(stat(Path, &Stat));

    //
    // Verify the mount in the /proc/self/mountinfo file.
    //

    if ((Flags & MOUNT_FIRST_MOUNT) != 0)
    {
        Direction = MNT_ITER_FORWARD;
    }
    else
    {
        Direction = MNT_ITER_BACKWARD;
    }

    LxtCheckResult(MountFindMount(MOUNT_PROC_MOUNTINFO, Path, 0, &Table, &FileSystem, Direction));

    LxtCheckNotEqual(FileSystem, NULL, "%p");
    LxtLogInfo("%s on %s fstype %s (%s)", mnt_fs_get_source(FileSystem), mnt_fs_get_target(FileSystem), mnt_fs_get_fstype(FileSystem), mnt_fs_get_options(FileSystem));

    ExpectedSourceActual = ExpectedSource;

#if LIBMOUNT_MAJOR_VERSION >= 2

    if ((ExpectedSourceActual == NULL) && (strcmp(ExpectedFsType, "virtiofs") != 0))
    {
        ExpectedSourceActual = "none";
    }

#endif

    LxtCheckEqual(ExpectedParentId, mnt_fs_get_parent_id(FileSystem), "%d");
    if (ExpectedSourceActual)
    {
        LxtCheckStringEqual(ExpectedSourceActual, mnt_fs_get_source(FileSystem));
    }

    LxtCheckStringEqual(ExpectedFsType, mnt_fs_get_fstype(FileSystem));
    strcpy(LocalPath, ExpectedRoot);
    if ((Flags & MOUNT_SOURCE_DELETED) != 0)
    {
        strcat(LocalPath, "//deleted");
    }

    LxtCheckStringEqual(LocalPath, mnt_fs_get_root(FileSystem));
    LxtCheckStringEqual(ExpectedMountOptions, mnt_fs_get_vfs_options(FileSystem));
    if (ExpectedFsOptions != NULL)
    {
        LxtCheckStringEqual(ExpectedFsOptions, mnt_fs_get_fs_options(FileSystem));
    }

    LxtCheckEqual(Stat.st_dev, mnt_fs_get_devno(FileSystem), "%lu");
    MountId = mnt_fs_get_id(FileSystem);
    LxtCheckGreater(MountId, 0, "%d");
    LxtCheckNotEqual(MountId, ExpectedParentId, "%d");
    mnt_free_table(Table);
    Table = NULL;

    //
    // Verify the mount in the /proc/mounts file.
    //

    LxtCheckResult(MountFindMount(MOUNT_PROC_MOUNTS, Path, 0, &Table, &FileSystem, Direction));

    LxtCheckNotEqual(FileSystem, NULL, "%p");
    if (ExpectedSourceActual)
    {
        LxtCheckStringEqual(ExpectedSourceActual, mnt_fs_get_source(FileSystem));
    }

    LxtCheckStringEqual(ExpectedFsType, mnt_fs_get_fstype(FileSystem));
    if (ExpectedCombinedOptions != NULL)
    {
        LxtCheckStringEqual(ExpectedCombinedOptions, mnt_fs_get_options(FileSystem));
    }

    //
    // Verify the mount in the /proc/self/mountstats file.
    //

    if (ExpectedSourceActual)
    {
        LxtCheckResult(MountFindMountStats(ExpectedSource, Path, ExpectedFsType));
    }

    Result = MountId;

ErrorExit:
    if (Table != NULL)
    {
        mnt_free_table(Table);
    }

    return Result;
}

int MountCheckIsNotMount(const char* Path)

/*++

Description:

    This routine checks if a path is a mount point.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    struct libmnt_fs* FileSystem;
    int Result;
    struct libmnt_table* Table;

    Table = NULL;
    LxtCheckResult(MountIsMount(AT_FDCWD, Path));
    if (Result != 0)
    {
        LxtLogError("%s is a mount point.", Path);
        Result = LXT_RESULT_FAILURE;
        goto ErrorExit;
    }

    //
    // Verify the mount is not in the /proc/self/mountinfo file.
    //

    LxtCheckResult(MountFindMount(MOUNT_PROC_MOUNTINFO, Path, 0, &Table, &FileSystem, MNT_ITER_BACKWARD));

    LxtCheckEqual(FileSystem, NULL, "%p");
    mnt_free_table(Table);
    Table = NULL;

    //
    // Verify the mount in the /proc/mounts file.
    //

    LxtCheckResult(MountFindMount(MOUNT_PROC_MOUNTS, Path, 0, &Table, &FileSystem, MNT_ITER_BACKWARD));

    LxtCheckEqual(FileSystem, NULL, "%p");

ErrorExit:
    if (Table != NULL)
    {
        mnt_free_table(Table);
    }

    return Result;
}

void MountEscapeString(const char* Source, char* Dest, size_t Length)

/*++

Description:

    This routine escapes a string using procfs rules.

Arguments:

    Source - Supplies the string to escape.

    Dest - Supplies the buffer to write the escaped string to.

    Length - Supplies the length of the destination buffer.

Return Value:

    None.

--*/

{

    size_t Index;

    for (Index = 0; (Index < Length - 1) && (*Source != '\0'); Index += 1, Source += 1)
    {
        switch (*Source)
        {
        case ' ':
        case '\n':
        case '\t':
        case '\\':
            Index += snprintf(Dest + Index, Length - Index, "\\%03o", *Source) - 1;

            break;

        default:
            Dest[Index] = *Source;
            break;
        }
    }

    Dest[Index] = '\0';
    return;
}

int MountFindMount(const char* MountsFile, const char* MountPoint, dev_t Device, struct libmnt_table** Table, struct libmnt_fs** FileSystem, int Direction)

/*++

Description:

    This routine finds a mount in the specified file by either mount point
    or device.

Arguments:

    MountsFile - Supplies the file to use (e.g. /proc/self/mountinfo).

    MountPoint - Supplies the mount point to search for. If NULL, device is
        used instead.

    Device - Supplies the device to search for if mount point is NULL.

    Table - Supplies a pointer that receives the parsed table. The caller
        must use mnt_free_table to free the table.

    FileSystem - Supplies a pointer that receives the file system, or NULL
        if none was found.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    struct libmnt_iter* Iterator;
    struct libmnt_fs* LocalFileSystem;
    struct libmnt_table* LocalTable;
    int Result;

    LocalFileSystem = NULL;
    Iterator = NULL;
    LocalTable = mnt_new_table_from_file(MountsFile);
    LxtCheckNotEqual(LocalTable, NULL, "%p");

    //
    // A backwards iterator is used to find the most recent mount.
    //

    if (MountPoint != NULL)
    {
        *FileSystem = mnt_table_find_target(LocalTable, MountPoint, Direction);
    }
    else
    {
        *FileSystem = NULL;
        Iterator = mnt_new_iter(MNT_ITER_FORWARD);
        LxtCheckNotEqual(Iterator, NULL, "%p");
        while (mnt_table_next_fs(LocalTable, Iterator, &LocalFileSystem) == 0)
        {
            if (mnt_fs_get_devno(LocalFileSystem) == Device)
            {
                *FileSystem = LocalFileSystem;
                break;
            }
        }
    }

    //
    // Return the table so the memory for file system remains valid.
    //

    if (*FileSystem != NULL)
    {
        *Table = LocalTable;
        LocalTable = NULL;
    }

    Result = 0;

ErrorExit:
    if (Iterator != NULL)
    {
        mnt_free_iter(Iterator);
    }

    if (LocalTable != NULL)
    {
        mnt_free_table(LocalTable);
    }

    return Result;
}

int MountFindMountStats(const char* Device, const char* MountPoint, const char* FsType)

/*++

Description:

    This routine finds a mount with the specified options in the
    /proc/self/mountstats file.

    N.B. libmount does not support parsing the mountstats file so this is done
         manually.

Arguments:

    Device - Supplies the mount source.

    MountPoint - Supplies the mount point.

    FsType - Supplies the file system type.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    char EscapedDevice[100];
    char EscapedMountPoint[100];
    char ExpectedLine[256];
    int Result;

    //
    // Format the expected mountstats line.
    //

    MountEscapeString(MountPoint, EscapedMountPoint, LXT_COUNT_OF(EscapedMountPoint));
    if (Device == NULL)
    {
        snprintf(ExpectedLine, sizeof(ExpectedLine), "no device mounted on %s with fstype %s", EscapedMountPoint, FsType);
    }
    else
    {
        MountEscapeString(Device, EscapedDevice, LXT_COUNT_OF(EscapedDevice));
        snprintf(ExpectedLine, sizeof(ExpectedLine), "device %s mounted on %s with fstype %s", EscapedDevice, EscapedMountPoint, FsType);
    }

    LxtCheckResult(MountFindMountStatsLine(ExpectedLine));

ErrorExit:
    return Result;
}

int MountFindMountStatsLine(char* ExpectedLine)

/*++

Description:

    This routine checks if a specific line exists in the mountstats file.

Arguments:

    ExpectedLine - Supplies the line to find.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    bool Found;
    FILE* File;
    char Line[256];
    int Result;

    File = fopen(MOUNT_PROC_MOUNTSTATS, "r");
    LxtCheckNotEqual(File, NULL, "%p");
    Found = FALSE;
    while (feof(File) == 0)
    {
        if (fgets(Line, sizeof(Line), File) == NULL)
        {
            break;
        }

        //
        // Strip the trailing \n and compare.
        //

        Line[strlen(Line) - 1] = '\0';
        if (strncmp(ExpectedLine, Line, sizeof(Line)) == 0)
        {
            Found = TRUE;
            break;
        }
    }

    if (Found == false)
    {
        LxtLogError("'%s' not found in " MOUNT_PROC_MOUNTSTATS, ExpectedLine);
        Result = LXT_RESULT_FAILURE;
        goto ErrorExit;
    }

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    if (File != NULL)
    {
        fclose(File);
    }

    return Result;
}

int MountGetMountId(const char* Path)

/*++

Description:

    This routine gets the mount ID for the specified path.

Arguments:

    Path - Supplies the path. Does not have to be a mount point.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    struct libmnt_fs* FileSystem;
    int Result;
    struct stat Stat;
    struct libmnt_table* Table;

    //
    // Find the mount ID of the  directory. This is done by device because
    // it may not be a mount point.
    //

    FileSystem = NULL;
    Table = NULL;
    LxtCheckErrnoZeroSuccess(stat(Path, &Stat));
    LxtCheckResult(MountFindMount(MOUNT_PROC_MOUNTINFO, NULL, Stat.st_dev, &Table, &FileSystem, MNT_ITER_BACKWARD));

    LxtCheckNotEqual(FileSystem, NULL, "%p");
    Result = mnt_fs_get_id(FileSystem);

ErrorExit:
    if (Table != NULL)
    {
        mnt_free_table(Table);
    }

    return Result;
}

int MountGetFileSystem(const char* Path, char* FsType, int FsTypeLength, char* Options, int OptionsLength)

/*++

Description:

    This routine gets the file system type of the mount containing the
    specified path.

Arguments:

    Path - Supplies the path.

    FsType - Supplies a buffer that receives the name of the file system type.

    Length - Supplies the size of the buffer.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    struct libmnt_fs* FileSystem;
    const char* LocalFsType;
    const char* LocalOptions;
    int Result;
    struct stat Stat;
    struct libmnt_table* Table;

    //
    // Find the mount ID of the  directory. This is done by device because
    // it may not be a mount point.
    //

    FileSystem = NULL;
    Table = NULL;
    LxtCheckErrnoZeroSuccess(stat(Path, &Stat));
    LxtCheckResult(MountFindMount(MOUNT_PROC_MOUNTINFO, NULL, Stat.st_dev, &Table, &FileSystem, MNT_ITER_BACKWARD));

    LxtCheckNotEqual(FileSystem, NULL, "%p");
    LocalFsType = mnt_fs_get_fstype(FileSystem);
    LocalOptions = mnt_fs_get_options(FileSystem);
    LxtLogInfo("File system at %s uses fstype %s, options %s.", Path, LocalFsType, LocalOptions);

    if (strlen(LocalFsType) >= FsTypeLength)
    {
        LxtLogError("Buffer too small for file system name");
        Result = LXT_RESULT_FAILURE;
        goto ErrorExit;
    }

    if (strlen(LocalOptions) >= OptionsLength)
    {
        LxtLogError("Buffer too small for options.");
        Result = LXT_RESULT_FAILURE;
        goto ErrorExit;
    }

    strcpy(FsType, LocalFsType);
    strcpy(Options, LocalOptions);

ErrorExit:
    if (Table != NULL)
    {
        mnt_free_table(Table);
    }

    return Result;
}

int MountGetMountOptions(const char* Path, char* Options)

/*++

Description:

    This routine gets mount options, which generally describe the mount peer
    group information for the specified path.

Arguments:

    Path - Supplies the path. Does not have to be a mount point.

    Options - Supplies a string buffer to receive the options

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    struct libmnt_fs* FileSystem;
    const char* LocalOptions;
    int Result;
    struct libmnt_table* Table;

    //
    // Find the mount ID of the directory.
    //

    FileSystem = NULL;
    Table = NULL;
    LxtCheckResult(MountFindMount(MOUNT_PROC_MOUNTINFO, Path, 0, &Table, &FileSystem, MNT_ITER_BACKWARD));

    //
    // shared:X, master:X, propagate_from:X, unbindable
    //

    LocalOptions = mnt_fs_get_optional_fields(FileSystem);
    if (LocalOptions != NULL)
    {
        strcpy(Options, LocalOptions);
    }
    else
    {
        Options[0] = '\0';
    }

ErrorExit:
    if (Table != NULL)
    {
        mnt_free_table(Table);
    }

    return Result;
}

int MountIsFileSystem(const char* Path, const char* FsType)

/*++

Description:

    This routine checks the file system type of the mount containing the
    specified path.

Arguments:

    Path - Supplies the path.

    FsType - Supplies the name of the file system type.

Return Value:

    Returns 1 if the path uses the specified file system, 0 if not, and -1 on
    failure.

--*/

{

    const char* ActualFsType;
    struct libmnt_fs* FileSystem;
    int Result;
    struct stat Stat;
    struct libmnt_table* Table;

    //
    // Find the mount ID of the  directory. This is done by device because
    // it may not be a mount point.
    //

    FileSystem = NULL;
    Table = NULL;
    LxtCheckErrnoZeroSuccess(stat(Path, &Stat));
    LxtCheckResult(MountFindMount(MOUNT_PROC_MOUNTINFO, NULL, Stat.st_dev, &Table, &FileSystem, MNT_ITER_BACKWARD));

    LxtCheckNotEqual(FileSystem, NULL, "%p");
    ActualFsType = mnt_fs_get_fstype(FileSystem);
    LxtLogInfo("File system at %s uses fstype %s.", Path, ActualFsType);
    if (strcmp(ActualFsType, FsType) == 0)
    {
        Result = 1;
    }
    else
    {
        Result = 0;
    }

ErrorExit:
    if (Table != NULL)
    {
        mnt_free_table(Table);
    }

    return Result;
}

int MountIsMount(int DirFd, const char* Path)

/*++

Description:

    This routine checks if a path is a mount point.

Arguments:

    Fd - Supplies the file descriptor to start resolving the path.

    Path - Supplies the path.

Return Value:

    Returns 0 if the path is not a mount point, 1 if it is a mount point, or
    -1 on failure.

--*/

{

    char LocalPath[PATH_MAX];
    char* ParentPath;
    struct stat ParentStat;
    int Result;
    struct stat Stat;

    LxtCheckErrnoZeroSuccess(fstatat(DirFd, Path, &Stat, AT_SYMLINK_NOFOLLOW));
    strncpy(LocalPath, Path, sizeof(LocalPath) - 1);
    ParentPath = dirname(LocalPath);
    LxtCheckErrnoZeroSuccess(fstatat(DirFd, ParentPath, &ParentStat, AT_SYMLINK_NOFOLLOW));

    LxtLogInfo(
        "%s device: %u,%u; %s device: %u,%u", ParentPath, major(ParentStat.st_dev), minor(ParentStat.st_dev), Path, major(Stat.st_dev), minor(Stat.st_dev));

    if (Stat.st_dev == ParentStat.st_dev)
    {
        Result = 0;
    }
    else
    {
        Result = 1;
    }

ErrorExit:
    return Result;
}

int MountPrepareTmpfs(char* Path, char* Device, int ExpectedParentId)

/*++

Description:

    This routine creates a tmpfs mount for testing.

Arguments:

    Path - Supplies the mount point path.

    Device - Supplies the device name to use.

    ExpectedParentId - Supplies the expected parent mount ID.

Return Value:

    Returns the mount ID on success, -1 on failure.

--*/

{

    return MountPrepareTmpfsEx(Path, Device, ExpectedParentId, 0, "rw,relatime");
}

int MountPrepareTmpfsEx(char* Path, char* Device, int ExpectedParentId, int Flags, const char* ExpectedOptions)

/*++

Description:

    This routine creates a tmpfs mount for testing.

Arguments:

    Path - Supplies the mount point path.

    Device - Supplies the device name to use.

    ExpectedParentId - Supplies the expected parent mount ID.

    Flags - Supplies the mount flags.

    ExpectedOptions - Supplies the expected mount option string.

Return Value:

    Returns the mount ID on success, -1 on failure.

--*/

{

    int MountId;
    int Result;

    LxtCheckErrnoZeroSuccess(mkdir(Path, 0700));
    LxtCheckErrnoZeroSuccess(mount(Device, Path, "tmpfs", Flags, NULL));
    LxtCheckResult(MountId = MountCheckIsMount(Path, ExpectedParentId, Device, "tmpfs", "/", ExpectedOptions, "rw", ExpectedOptions, 0));

    Result = MountId;

ErrorExit:
    return Result;
}
