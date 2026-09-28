/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    lxtmount.h

Abstract:

    This file contains mount primitive support.

--*/

#define MOUNT_PROC_MOUNTS "/proc/mounts"
#define MOUNT_PROC_MOUNTINFO "/proc/self/mountinfo"
#define MOUNT_PROC_MOUNTSTATS "/proc/self/mountstats"
#define MOUNT_SOURCE_DELETED (0x1)
#define MOUNT_FIRST_MOUNT (0x2)

//
// Flags for the unshare helper.
//

#define MOUNT_NAMESPACE_USE_CLONE (0x1)

int MountCheckIsMount(
    const char* Path,
    int ExpectedParentId,
    const char* ExpectedSource,
    const char* ExpectedFsType,
    const char* ExpectedRoot,
    const char* ExpectedMountOptions,
    const char* ExpectedFsOptions,
    const char* ExpectedCombinedOptions,
    int Flags);

int MountCheckIsNotMount(const char* Path);

int MountFindMount(const char* MountsFile, const char* MountPoint, dev_t Device, struct libmnt_table** Table, struct libmnt_fs** FileSystem, int Direction);

int MountFindMountStats(const char* Device, const char* MountPoint, const char* FsType);

int MountFindMountStatsLine(char* ExpectedLine);

int MountGetFileSystem(const char* Path, char* FsType, int FsTypeLength, char* Options, int OptionsLength);

int MountGetMountId(const char* Path);

int MountGetMountOptions(const char* Path, char* Options);

int MountIsFileSystem(const char* Path, const char* FsType);

int MountIsMount(int DirFd, const char* Path);

int MountPrepareTmpfs(char* Path, char* Device, int ExpectedParentId);

int MountPrepareTmpfsEx(char* Path, char* Device, int ExpectedParentId, int Flags, const char* ExpectedOptions);
