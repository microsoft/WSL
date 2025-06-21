/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    OverlayFs.c

Abstract:

    This file is a overlayfs test.

    N.B. This test depends on libmount, which is part of the libmount-dev
         apt package.

    N.B. In addition to this unit test, the official overlay test on github
         should be run when changes are made.

--*/

#include "lxtcommon.h"
#include "unittests.h"
#include <sys/mount.h>
#include <linux/capability.h>
#include <libgen.h>
#include <fcntl.h>
#include <stdlib.h>
#include <libmount/libmount.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/prctl.h>
#include <sys/xattr.h>
#include "lxtmount.h"
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
#include <string.h>
#include <utime.h>
#include <sys/cdefs.h>
#include <linux/capability.h>

#define LXT_NAME "OverlayFs"

#define OVFS_TEST_PATH "/data"

#define OVFS_TEST_MOUNT_PATH OVFS_TEST_PATH "/" OVFS_TEST_MERGED_DIR

#define OVFS_TEST_MOUNT_NAME "overlay"

#define OVFS_TEST_LOWER_DIR "ovfs_test_lower"
#define OVFS_TEST_LOWER2_DIR "ovfs_test_lower2"
#define OVFS_TEST_LOWER3_DIR "ovfs_test_lower3"
#define OVFS_TEST_UPPER_DIR "ovfs_test_upper"
#define OVFS_TEST_WORK_DIR "ovfs_test_work"
#define OVFS_TEST_MERGED_DIR "ovfs_test_merged"

#define OVFS_TEST_MOUNT_DEFAULT "lowerdir=" OVFS_TEST_LOWER_DIR ",upperdir=" OVFS_TEST_UPPER_DIR ",workdir=" OVFS_TEST_WORK_DIR

#define OVFS_TEST_MOUNT_MULTI_LOWER \
    "lowerdir=" OVFS_TEST_LOWER_DIR ":" OVFS_TEST_LOWER2_DIR ":" OVFS_TEST_LOWER3_DIR ",upperdir=" OVFS_TEST_UPPER_DIR \
    ",workdir=" OVFS_TEST_WORK_DIR

#define OVFS_TEST_MOUNT_FS_OPTS "rw," OVFS_TEST_MOUNT_DEFAULT
#define OVFS_TEST_MOUNT_COMBINED_OPTS "rw,relatime," OVFS_TEST_MOUNT_DEFAULT

const char* g_OvFsTestDirs[] = {
    OVFS_TEST_LOWER_DIR, OVFS_TEST_LOWER2_DIR, OVFS_TEST_LOWER3_DIR, OVFS_TEST_UPPER_DIR, OVFS_TEST_WORK_DIR, OVFS_TEST_MOUNT_PATH};

//
// N.B. This data must be kept in sync with OvFsTestDirsPopulate.
//

struct
{
    char* Path;
    char* Name;
    mode_t Mode;
    int Hydrates;
} g_OvFsMergedContents[] = {
    {OVFS_TEST_MOUNT_PATH "/OnlyInLowerDir", "OnlyInLowerDir", S_IFDIR | 0222, 1},
    {OVFS_TEST_MOUNT_PATH "/OnlyInLowerFile", "OnlyInLowerFile", S_IFREG | 0222, 1},
    {OVFS_TEST_MOUNT_PATH "/OnlyInLowerSym", "OnlyInLowerSym", S_IFLNK | 0222, 1},
    {OVFS_TEST_MOUNT_PATH "/OnlyInUpperDir", "OnlyInUpperDir", S_IFDIR | 0777, 0},
    {OVFS_TEST_MOUNT_PATH "/OnlyInUpperFile", "OnlyInUpperFile", S_IFREG | 0777, 0},
    {OVFS_TEST_MOUNT_PATH "/OnlyInUpperSym", "OnlyInUpperSym", S_IFLNK | 0777, 0},
    {OVFS_TEST_MOUNT_PATH "/InBothDir", "InBothDir", S_IFDIR | 0777, 0},
    {OVFS_TEST_MOUNT_PATH "/InBothFile", "InBothFile", S_IFREG | 0777, 0},
    {OVFS_TEST_MOUNT_PATH "/InBothSym", "InBothSym", S_IFLNK | 0777, 0}};

struct
{
    char* Path;
    char* Name;
    mode_t Mode;
    int Hydrates;
} g_OvFsMergedMultiContents[] = {
    {OVFS_TEST_MOUNT_PATH "/OnlyInLowerDir", "OnlyInLowerDir", S_IFDIR | 0222, 1},
    {OVFS_TEST_MOUNT_PATH "/OnlyInLowerFile", "OnlyInLowerFile", S_IFREG | 0222, 1},
    {OVFS_TEST_MOUNT_PATH "/OnlyInLowerSym", "OnlyInLowerSym", S_IFLNK | 0222, 1},
    {OVFS_TEST_MOUNT_PATH "/OnlyInLower2Dir", "OnlyInLower2Dir", S_IFDIR | 0222, 1},
    {OVFS_TEST_MOUNT_PATH "/OnlyInLower2File", "OnlyInLower2File", S_IFREG | 0222, 1},
    {OVFS_TEST_MOUNT_PATH "/OnlyInLower23File", "OnlyInLower23File", S_IFREG | 0222, 1},
    {OVFS_TEST_MOUNT_PATH "/OnlyInLower3Dir", "OnlyInLower3Dir", S_IFDIR | 0222, 1},
    {OVFS_TEST_MOUNT_PATH "/OnlyInLower3File", "OnlyInLower3File", S_IFREG | 0222, 1},
    {OVFS_TEST_MOUNT_PATH "/OnlyInUpperDir", "OnlyInUpperDir", S_IFDIR | 0777, 0},
    {OVFS_TEST_MOUNT_PATH "/OnlyInUpperFile", "OnlyInUpperFile", S_IFREG | 0777, 0},
    {OVFS_TEST_MOUNT_PATH "/OnlyInUpperSym", "OnlyInUpperSym", S_IFLNK | 0777, 0},
    {OVFS_TEST_MOUNT_PATH "/InBothDir", "InBothDir", S_IFDIR | 0777, 0},
    {OVFS_TEST_MOUNT_PATH "/InBothFile", "InBothFile", S_IFREG | 0777, 0},
    {OVFS_TEST_MOUNT_PATH "/InBothSym", "InBothSym", S_IFLNK | 0777, 0}};

LXT_VARIATION_HANDLER OvFsTestBasicMount;
LXT_VARIATION_HANDLER OvFsTestFileObjectReadOps;
LXT_VARIATION_HANDLER OvFsTestFileObjectWriteOpsUpper;
LXT_VARIATION_HANDLER OvFsTestInodeOpaque;
LXT_VARIATION_HANDLER OvFsTestInodeReadOps;
LXT_VARIATION_HANDLER OvFsTestInodeRename;
LXT_VARIATION_HANDLER OvFsTestInodeWhiteout;
LXT_VARIATION_HANDLER OvFsTestInodeWriteOps;
LXT_VARIATION_HANDLER OvFsTestInodeWriteOpsUpper;
LXT_VARIATION_HANDLER OvFsTestInodeUnlink;
LXT_VARIATION_HANDLER OvFsTestInodeXattr;
LXT_VARIATION_HANDLER OvFsTestLowerWhiteout;
LXT_VARIATION_HANDLER OvFsTestMultipleLower;

int OvFsTestDirsPopulate(void);

int OvFsTestDirsSetup(void);

//
// Global constants
//

static const LXT_VARIATION g_LxtVariations[] = {
    {"OverlayFs - basic mount", OvFsTestBasicMount},
    {"OverlayFs - inode read ops", OvFsTestInodeReadOps},
    {"OverlayFs - file object read ops", OvFsTestFileObjectReadOps},
    {"OverlayFs - inode write ops upper", OvFsTestInodeWriteOpsUpper},
    {"OverlayFs - file object write ops upper", OvFsTestFileObjectWriteOpsUpper},
    {"OverlayFs - inode write ops", OvFsTestInodeWriteOps},
    {"OverlayFs - inode unlink", OvFsTestInodeUnlink},
    {"OverlayFs - whiteout", OvFsTestInodeWhiteout},
    {"OverlayFs - opaque", OvFsTestInodeOpaque},
    {"OverlayFs - rename", OvFsTestInodeRename},
    {"OverlayFs - xattr", OvFsTestInodeXattr},
    {"OverlayFs - multiple lower layers", OvFsTestMultipleLower},
    {"OverlayFs - lower layer whiteouts", OvFsTestLowerWhiteout}};

static int g_TestPathMountId;

const int g_LxtUnstableInodes = 0;

int OverlayFsTestEntry(int Argc, char* Argv[])

/*++
--*/

{

    LXT_ARGS Args;
    int Result;

    //
    //  TODO_LX: Support other filesystems than volfs.
    //

    LxtCheckResult(g_TestPathMountId = MountGetMountId(OVFS_TEST_PATH));
    LxtCheckResult(LxtInitialize(Argc, Argv, &Args, LXT_NAME));
    LxtCheckErrno(chdir(OVFS_TEST_PATH));
    LxtCheckResult(LxtRunVariations(&Args, g_LxtVariations, LXT_COUNT_OF(g_LxtVariations)));

ErrorExit:

    char DeleteCmd[128];

    for (int Index = 0; Index < LXT_COUNT_OF(g_OvFsTestDirs); ++Index)
    {
        sprintf(DeleteCmd, "rm -rf %s", g_OvFsTestDirs[Index]);
        if (system(DeleteCmd) < 0)
        {
            LxtLogError("Failed to delete %s", g_OvFsTestDirs[Index]);
        }
    }

    LxtUninitialize();
    return !LXT_SUCCESS(Result);
}

int OvFsTestBasicMount(PLXT_ARGS Args)

/*++

Description:

    This routine tests the mount and umount system calls for overlayfs.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int Index;
    struct
    {
        char* Options;
        int Errno;
    } InvalidOpts[] = {
        {NULL, EINVAL},
        {"", EINVAL},
        {"lowerdir=doesNotExist"
         ",upperdir=" OVFS_TEST_UPPER_DIR ",workdir=" OVFS_TEST_WORK_DIR,
         ENOENT},
        {"lowerdir=" OVFS_TEST_LOWER_DIR ",lowerdir=" OVFS_TEST_UPPER_DIR ",workdir=" OVFS_TEST_WORK_DIR, EINVAL},
        {"lowerdir=" OVFS_TEST_LOWER_DIR ",workdir=" OVFS_TEST_WORK_DIR, EINVAL},
        {"lowerdir=" OVFS_TEST_LOWER_DIR ",upperdir=" OVFS_TEST_UPPER_DIR ",workdir=" OVFS_TEST_UPPER_DIR, EINVAL},
        {"lowerdir=" OVFS_TEST_LOWER_DIR ",upperdir=" OVFS_TEST_UPPER_DIR ",workdir=" OVFS_TEST_UPPER_DIR "/" OVFS_TEST_WORK_DIR, EINVAL},
        {"lowerdir=:"
         ",upperdir=" OVFS_TEST_UPPER_DIR ",workdir=" OVFS_TEST_WORK_DIR,
         EINVAL},
        {"lowerdir=" OVFS_TEST_LOWER_DIR ":"
         ",upperdir=" OVFS_TEST_UPPER_DIR ",workdir=" OVFS_TEST_WORK_DIR,
         EINVAL},
        {"lowerdir=" OVFS_TEST_LOWER_DIR ":" OVFS_TEST_LOWER_DIR ":" OVFS_TEST_LOWER_DIR ":"
         ",upperdir=" OVFS_TEST_UPPER_DIR ",workdir=" OVFS_TEST_WORK_DIR,
         EINVAL},
        {"lowerdir=" OVFS_TEST_LOWER_DIR ":" OVFS_TEST_LOWER_DIR ":"
         "doesNotExist"
         ",upperdir=" OVFS_TEST_UPPER_DIR ",workdir=" OVFS_TEST_WORK_DIR,
         ENOENT}};
    int MountId;
    int Result;

    //
    // Set up the directories and ensure it's not a mount point yet.
    //

    LxtCheckResult(OvFsTestDirsSetup());
    LxtCheckResult(MountCheckIsNotMount(OVFS_TEST_MOUNT_PATH));

    //
    // Mount an overlayfs instance and check it was mounted.
    //

    LxtCheckErrnoZeroSuccess(mount("myovfsnew", OVFS_TEST_MOUNT_PATH, OVFS_TEST_MOUNT_NAME, 0, OVFS_TEST_MOUNT_DEFAULT));

    LxtCheckResult(
        MountId = MountCheckIsMount(
            OVFS_TEST_MOUNT_PATH, g_TestPathMountId, "myovfsnew", OVFS_TEST_MOUNT_NAME, "/", "rw,relatime", OVFS_TEST_MOUNT_FS_OPTS, OVFS_TEST_MOUNT_COMBINED_OPTS, 0));

    //
    // Mounting again should succeed.
    //

    LxtCheckErrnoZeroSuccess(mount("myovfsnew", OVFS_TEST_MOUNT_PATH, OVFS_TEST_MOUNT_NAME, 0, OVFS_TEST_MOUNT_DEFAULT));

    LxtCheckResult(
        MountId = MountCheckIsMount(
            OVFS_TEST_MOUNT_PATH, MountId, "myovfsnew", OVFS_TEST_MOUNT_NAME, "/", "rw,relatime", OVFS_TEST_MOUNT_FS_OPTS, OVFS_TEST_MOUNT_COMBINED_OPTS, 0));

    LxtCheckErrnoZeroSuccess(umount(OVFS_TEST_MOUNT_PATH));
    LxtCheckResult(
        MountId = MountCheckIsMount(
            OVFS_TEST_MOUNT_PATH, g_TestPathMountId, "myovfsnew", OVFS_TEST_MOUNT_NAME, "/", "rw,relatime", OVFS_TEST_MOUNT_FS_OPTS, OVFS_TEST_MOUNT_COMBINED_OPTS, 0));

    //
    // Unmount and check it was unmounted.
    //

    LxtCheckErrnoZeroSuccess(umount(OVFS_TEST_MOUNT_PATH));
    LxtCheckResult(MountCheckIsNotMount(OVFS_TEST_MOUNT_PATH));

    //
    // Check invalid mount parameters.
    //

    LxtLogInfo("Checking invalid options...");
    mkdir(OVFS_TEST_UPPER_DIR "/" OVFS_TEST_WORK_DIR, 0777);
    for (Index = 0; Index < LXT_COUNT_OF(InvalidOpts); ++Index)
    {
        LxtCheckErrnoFailure(
            mount("myovfsnew", OVFS_TEST_MOUNT_PATH, OVFS_TEST_MOUNT_NAME, 0, InvalidOpts[Index].Options), InvalidOpts[Index].Errno);
    }

    Result = 0;

ErrorExit:
    umount(OVFS_TEST_MOUNT_PATH);
    return Result;
}

int OvFsTestDirsPopulate(void)

/*++

Description:

    This routine populates the mount directories.

    N.B. This data must be kept in sync with g_OvFsMergedContents.

Arguments:

    None.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    char XattrName[64];
    int Fd;
    char* FileName;
    int Index;
    struct
    {
        char* Name;
        int Mode;
    } Paths[] = {
        {OVFS_TEST_LOWER_DIR "/OnlyInLowerDir", S_IFDIR | 0666},
        {OVFS_TEST_LOWER_DIR "/InBothDir", S_IFDIR | 0666},
        {OVFS_TEST_LOWER_DIR "/OnlyInLowerFile", S_IFREG | 0666},
        {OVFS_TEST_LOWER_DIR "/InBothFile", S_IFREG | 0666},

        {OVFS_TEST_LOWER2_DIR "/OnlyInLower2Dir", S_IFDIR | 0444},
        {OVFS_TEST_LOWER2_DIR "/InBothDir", S_IFDIR | 0444},
        {OVFS_TEST_LOWER2_DIR "/OnlyInLower2File", S_IFREG | 0444},
        {OVFS_TEST_LOWER2_DIR "/OnlyInLower23File", S_IFREG | 0444},
        {OVFS_TEST_LOWER2_DIR "/InBothFile", S_IFREG | 0444},

        {OVFS_TEST_LOWER3_DIR "/OnlyInLower3Dir", S_IFDIR | 0111},
        {OVFS_TEST_LOWER3_DIR "/InBothDir", S_IFDIR | 0111},
        {OVFS_TEST_LOWER3_DIR "/OnlyInLower3File", S_IFREG | 0111},
        {OVFS_TEST_LOWER3_DIR "/OnlyInLower23File", S_IFREG | 0111},
        {OVFS_TEST_LOWER3_DIR "/InBothFile", S_IFREG | 0111},

        {OVFS_TEST_UPPER_DIR "/OnlyInUpperDir", S_IFDIR | 0777},
        {OVFS_TEST_UPPER_DIR "/InBothDir", S_IFDIR | 0777},
        {OVFS_TEST_UPPER_DIR "/OnlyInUpperFile", S_IFREG | 0777},
        {OVFS_TEST_UPPER_DIR "/InBothFile", S_IFREG | 0777}};
    int Result;

    Fd = -1;

    for (Index = 0; Index < LXT_COUNT_OF(Paths); ++Index)
    {
        FileName = strrchr(Paths[Index].Name, '/') + 1;
        if (S_ISREG(Paths[Index].Mode))
        {
            LxtCheckErrno(Fd = creat(Paths[Index].Name, Paths[Index].Mode));
            LxtCheckErrno(write(Fd, FileName, strlen(FileName) + 1));
            LxtClose(Fd);
            Fd = -1;
        }
        else
        {
            LxtCheckErrno(mkdir(Paths[Index].Name, Paths[Index].Mode));
        }

        LxtCheckErrno(lsetxattr(Paths[Index].Name, "trusted.overlaytest", FileName, strlen(FileName) + 1, XATTR_CREATE));

        sprintf(XattrName, "trusted.%s", FileName);
        LxtCheckErrno(lsetxattr(Paths[Index].Name, XattrName, FileName, strlen(FileName) + 1, XATTR_CREATE));
    }

    //
    // N.B. xattrs cannot be set on symbolic links on all filesystems.
    //

    LxtCheckErrno(symlink(OVFS_TEST_LOWER_DIR "/OnlyInLowerFile", OVFS_TEST_LOWER_DIR "/OnlyInLowerSym"));

    LxtCheckErrno(symlink(OVFS_TEST_LOWER_DIR "/InBothFile", OVFS_TEST_LOWER_DIR "/InBothSym"));

    LxtCheckErrno(symlink(OVFS_TEST_UPPER_DIR "/OnlyInUpperFile", OVFS_TEST_UPPER_DIR "/OnlyInUpperSym"));

    LxtCheckErrno(symlink(OVFS_TEST_UPPER_DIR "/InBothFile", OVFS_TEST_UPPER_DIR "/InBothSym"));

ErrorExit:
    if (Fd != -1)
    {
        LxtClose(Fd);
    }

    return Result;
}

int OvFsTestDirsSetup(void)

/*++

Description:

    This routine prepares the mount directories.

Arguments:

    None.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    char DeleteCmd[128];
    int Index;
    int Result;

    umount(OVFS_TEST_MOUNT_PATH);
    sprintf(DeleteCmd, "rm -rf %s", OVFS_TEST_MOUNT_PATH);
    system(DeleteCmd);
    LxtCheckErrno(mkdir(OVFS_TEST_MOUNT_PATH, 0777));
    for (Index = 0; Index < LXT_COUNT_OF(g_OvFsTestDirs); ++Index)
    {
        sprintf(DeleteCmd, "rm -rf %s", g_OvFsTestDirs[Index]);
        system(DeleteCmd);
        LxtCheckErrno(mkdir(g_OvFsTestDirs[Index], 0777));
    }

    Result = 0;

ErrorExit:
    return Result;
}

int OvFsTestFileObjectReadOps(PLXT_ARGS Args)

/*++

Description:

    This routine tests file object operations that do not modify state.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    char Buffer[100];
    int BytesRead;
    int Count;
    struct dirent* Entry;
    int EntryPosition;
    int Fd;
    int Found[LXT_COUNT_OF(g_OvFsMergedContents) + 2];
    int FoundIndex;
    int Index;
    void* Mapping;
    void* MapResult;
    int Result;
    struct stat StatBuffer;
    struct stat StatMergedBuffer;

    Fd = -1;
    Mapping = NULL;

    //
    // Set up the directories and populate some state.
    //

    LxtCheckResult(OvFsTestDirsSetup());
    LxtCheckResult(OvFsTestDirsPopulate());

    //
    // Mount an overlayfs instance and check inode operations that do not
    // hydrate files.
    //

    LxtCheckResult(MountCheckIsNotMount(OVFS_TEST_MOUNT_PATH));
    LxtCheckErrnoZeroSuccess(mount("myovfsnew", OVFS_TEST_MOUNT_PATH, OVFS_TEST_MOUNT_NAME, 0, OVFS_TEST_MOUNT_DEFAULT));

    //
    // Check the behavior for VFS file object operations that support file
    // descriptors opened for read only.
    //
    // N.B. All other file operations will fail the request and are verified
    //      in the VFS access test.
    //

    //
    // Check the behavior for read directory on the root.
    //

    memset(Found, 0, sizeof(Found));
    LxtCheckErrno(Fd = open(OVFS_TEST_MOUNT_PATH, O_RDONLY));
    LxtCheckErrno(BytesRead = LxtGetdents64(Fd, (struct dirent*)Buffer, sizeof(Buffer)));

    while (BytesRead > 0)
    {
        for (EntryPosition = 0; EntryPosition < BytesRead; EntryPosition += Entry->d_reclen)
        {

            Entry = (struct dirent*)&Buffer[EntryPosition];
            if (strcmp(Entry->d_name, ".") == 0)
            {
                FoundIndex = 0;
            }
            else if (strcmp(Entry->d_name, "..") == 0)
            {
                FoundIndex = 1;
            }
            else
            {
                for (Index = 0; Index < LXT_COUNT_OF(g_OvFsMergedContents); ++Index)
                {
                    if (strcmp(g_OvFsMergedContents[Index].Name, Entry->d_name) == 0)
                    {
                        FoundIndex = Index + 2;
                        break;
                    }
                }

                if (Index == LXT_COUNT_OF(g_OvFsMergedContents))
                {
                    LxtLogError("Unexpected entry %s", Entry->d_name);
                    LxtCheckNotEqual(Index, LXT_COUNT_OF(g_OvFsMergedContents), "%d");
                }
            }

            LxtCheckEqual(Found[FoundIndex], 0, "%d");
            Found[FoundIndex] = 1;
        }

        Entry = (struct dirent*)Buffer;
        LxtCheckErrno(BytesRead = LxtGetdents64(Fd, (struct dirent*)Buffer, sizeof(Buffer)));
    }

    for (Index = 0; Index < LXT_COUNT_OF(Found); ++Index)
    {
        LxtCheckEqual(Found[FoundIndex], 1, "%d");
    }

    LxtClose(Fd);
    Fd = -1;

    //
    // Check the behavior for read directory on sub directories.
    //

    for (Index = 0; Index < LXT_COUNT_OF(g_OvFsMergedContents); ++Index)
    {
        if (S_ISDIR(g_OvFsMergedContents[Index].Mode) == FALSE)
        {
            continue;
        }

        LxtCheckErrno(Fd = open(g_OvFsMergedContents[Index].Path, O_RDONLY));
        LxtCheckErrno(BytesRead = LxtGetdents64(Fd, (struct dirent*)Buffer, sizeof(Buffer)))

            Count = 0;
        for (EntryPosition = 0; EntryPosition < BytesRead; EntryPosition += Entry->d_reclen)
        {

            Entry = (struct dirent*)&Buffer[EntryPosition];
            Count += 1;
        }

        LxtCheckEqual(Count, 2, "%d");
        LxtClose(Fd);
        Fd = -1;
    }

    //
    // Check the behavior for map.
    //

    for (Index = 0; Index < LXT_COUNT_OF(g_OvFsMergedContents); ++Index)
    {
        if (S_ISREG(g_OvFsMergedContents[Index].Mode) == FALSE)
        {
            continue;
        }

        LxtCheckErrno(Fd = open(g_OvFsMergedContents[Index].Path, O_RDONLY));
        LxtCheckMapErrno(Mapping = mmap(NULL, PAGE_SIZE, PROT_READ, MAP_SHARED, Fd, 0));
        LxtCheckStringEqual(Mapping, g_OvFsMergedContents[Index].Name);
        munmap(Mapping, PAGE_SIZE);
        Mapping = MAP_FAILED;
        LxtClose(Fd);
        Fd = -1;
    }

    //
    // Check the behavior for ioctl.
    //

    for (Index = 0; Index < LXT_COUNT_OF(g_OvFsMergedContents); ++Index)
    {
        if (S_ISREG(g_OvFsMergedContents[Index].Mode) == FALSE)
        {
            continue;
        }

        LxtCheckErrno(Fd = open(g_OvFsMergedContents[Index].Path, O_RDONLY));
        LxtCheckErrno(ioctl(Fd, FIONREAD, &BytesRead));
        LxtCheckEqual(BytesRead, strlen(g_OvFsMergedContents[Index].Name) + 1, "%d");
        LxtClose(Fd);
        Fd = -1;
    }

    //
    // Check the behavior for sync.
    //

    for (Index = 0; Index < LXT_COUNT_OF(g_OvFsMergedContents); ++Index)
    {
        if ((S_ISDIR(g_OvFsMergedContents[Index].Mode) == FALSE) && (S_ISREG(g_OvFsMergedContents[Index].Mode) == FALSE))
        {

            continue;
        }

        LxtCheckErrno(Fd = open(g_OvFsMergedContents[Index].Path, O_RDONLY));
        LxtCheckErrno(fsync(Fd));
        LxtClose(Fd);
        Fd = -1;
    }

    //
    // Check the behavior for read file.
    //

    for (Index = 0; Index < LXT_COUNT_OF(g_OvFsMergedContents); ++Index)
    {
        if (S_ISREG(g_OvFsMergedContents[Index].Mode) == FALSE)
        {
            continue;
        }

        LxtCheckErrno(Fd = open(g_OvFsMergedContents[Index].Path, O_RDONLY));
        LxtCheckErrno(BytesRead = read(Fd, Buffer, sizeof(Buffer) - 1));
        Buffer[BytesRead] = 0;
        LxtCheckStringEqual(Buffer, g_OvFsMergedContents[Index].Name);
        LxtClose(Fd);
        Fd = -1;
    }

    //
    // Check the behavior for seek.
    //

    for (Index = 0; Index < LXT_COUNT_OF(g_OvFsMergedContents); ++Index)
    {
        if ((S_ISDIR(g_OvFsMergedContents[Index].Mode) == FALSE) && (S_ISREG(g_OvFsMergedContents[Index].Mode) == FALSE))
        {

            continue;
        }

        LxtLogInfo("%s", g_OvFsMergedContents[Index].Path);
        LxtCheckErrno(Fd = open(g_OvFsMergedContents[Index].Path, O_RDONLY));
        LxtCheckErrno(lseek(Fd, SEEK_SET, 1));
        LxtClose(Fd);
        Fd = -1;
    }

    //
    // Check that none of the operations hydrated files from the lower
    // directory.
    //

    LxtCheckErrnoFailure(stat(OVFS_TEST_UPPER_DIR "/OnlyInLowerDir", &StatBuffer), ENOENT);
    LxtCheckErrnoFailure(stat(OVFS_TEST_UPPER_DIR "/OnlyInLowerFile", &StatBuffer), ENOENT);
    LxtCheckErrnoFailure(stat(OVFS_TEST_UPPER_DIR "/OnlyInLowerSym", &StatBuffer), ENOENT);

    //
    // Unmount and check it was unmounted.
    //

    if (Fd != -1)
    {
        LxtClose(Fd);
        Fd = -1;
    }

    LxtCheckErrnoZeroSuccess(umount(OVFS_TEST_MOUNT_PATH));
    LxtCheckResult(MountCheckIsNotMount(OVFS_TEST_MOUNT_PATH));
    Result = 0;

ErrorExit:
    if (Mapping != MAP_FAILED)
    {
        munmap(Mapping, PAGE_SIZE);
    }

    if (Fd != -1)
    {
        LxtClose(Fd);
    }

    umount(OVFS_TEST_MOUNT_PATH);
    return Result;
}

int OvFsTestFileObjectWriteOpsUpper(PLXT_ARGS Args)

/*++

Description:

    This routine tests file object write operations that do not modify the
    lower.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    char Buffer[100];
    char BufferExpected[100];
    int BytesRead;
    int BytesWritten;
    int Count;
    struct dirent* Entry;
    int EntryPosition;
    int Fd;
    int Found[LXT_COUNT_OF(g_OvFsMergedContents) + 2];
    int FoundIndex;
    int Index;
    void* Mapping;
    void* MapResult;
    int Result;
    struct stat StatBuffer;
    struct stat StatMergedBuffer;

    Fd = -1;
    Mapping = NULL;

    //
    // Set up the directories and populate some state.
    //

    LxtCheckResult(OvFsTestDirsSetup());
    LxtCheckResult(OvFsTestDirsPopulate());

    //
    // Mount an overlayfs instance and check inode operations that modify the
    // upper but do not hydrate files.
    //
    // N.B. The overlay fs mount does not need to be recreated after each
    //      variation since only the upper is modified.
    //

    LxtCheckResult(MountCheckIsNotMount(OVFS_TEST_MOUNT_PATH));
    LxtCheckErrnoZeroSuccess(mount("myovfsnew", OVFS_TEST_MOUNT_PATH, OVFS_TEST_MOUNT_NAME, 0, OVFS_TEST_MOUNT_DEFAULT));

    //
    // Check the behavior for map.
    //

    for (Index = 0; Index < LXT_COUNT_OF(g_OvFsMergedContents); ++Index)
    {
        if ((S_ISREG(g_OvFsMergedContents[Index].Mode) == FALSE) || (g_OvFsMergedContents[Index].Hydrates != 0))
        {

            continue;
        }

        LxtCheckErrno(Fd = open(g_OvFsMergedContents[Index].Path, O_RDWR));
        LxtCheckMapErrno(Mapping = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, Fd, 0));
        LxtCheckStringEqual(Mapping, g_OvFsMergedContents[Index].Name);
        munmap(Mapping, PAGE_SIZE);
        Mapping = MAP_FAILED;
        LxtClose(Fd);
        Fd = -1;
    }

    //
    // Check the behavior for truncate.
    //

    for (Index = 0; Index < LXT_COUNT_OF(g_OvFsMergedContents); ++Index)
    {
        if ((S_ISREG(g_OvFsMergedContents[Index].Mode) == FALSE) || (g_OvFsMergedContents[Index].Hydrates != 0))
        {

            continue;
        }

        LxtCheckErrno(Fd = open(g_OvFsMergedContents[Index].Path, O_RDWR));
        LxtCheckErrno(ftruncate(Fd, 0));
        LxtClose(Fd);
        Fd = -1;
    }

    //
    // Check the behavior for fallocate.
    //

    for (Index = 0; Index < LXT_COUNT_OF(g_OvFsMergedContents); ++Index)
    {
        if ((S_ISREG(g_OvFsMergedContents[Index].Mode) == FALSE) || (g_OvFsMergedContents[Index].Hydrates != 0))
        {

            continue;
        }

        LxtCheckErrno(Fd = open(g_OvFsMergedContents[Index].Path, O_RDWR));
        LxtCheckErrno(fallocate(Fd, 0, 0, strlen(g_OvFsMergedContents[Index].Name) + 1));
        LxtClose(Fd);
        Fd = -1;
    }

    //
    // Check the behavior for write file.
    //

    memset(BufferExpected, 0, sizeof(BufferExpected));
    for (Index = 0; Index < LXT_COUNT_OF(g_OvFsMergedContents); ++Index)
    {
        if ((S_ISREG(g_OvFsMergedContents[Index].Mode) == FALSE) || (g_OvFsMergedContents[Index].Hydrates != 0))
        {

            continue;
        }

        LxtCheckErrno(Fd = open(g_OvFsMergedContents[Index].Path, O_RDWR));
        LxtCheckErrno(BytesRead = read(Fd, Buffer, sizeof(Buffer) - 1));
        Buffer[BytesRead] = 0;
        LxtCheckEqual(BytesRead, strlen(g_OvFsMergedContents[Index].Name) + 1, "%d");
        LxtCheckMemoryEqual(Buffer, BufferExpected, BytesRead);
        LxtCheckErrno(lseek(Fd, SEEK_SET, 0));
        LxtCheckErrno(BytesWritten = write(Fd, g_OvFsMergedContents[Index].Name, strlen(g_OvFsMergedContents[Index].Name) + 1));
        LxtCheckEqual(BytesWritten, strlen(g_OvFsMergedContents[Index].Name) + 1, "%d");
        LxtCheckErrno(lseek(Fd, SEEK_SET, 0));
        LxtCheckErrno(BytesRead = read(Fd, Buffer, sizeof(Buffer) - 1));
        Buffer[BytesRead] = 0;
        LxtCheckStringEqual(Buffer, g_OvFsMergedContents[Index].Name);
        LxtClose(Fd);
        Fd = -1;
    }

    //
    // Check that none of the operations hydrated files from the lower
    // directory.
    //

    LxtCheckErrnoFailure(stat(OVFS_TEST_UPPER_DIR "/OnlyInLowerDir", &StatBuffer), ENOENT);
    LxtCheckErrnoFailure(stat(OVFS_TEST_UPPER_DIR "/OnlyInLowerFile", &StatBuffer), ENOENT);
    LxtCheckErrnoFailure(stat(OVFS_TEST_UPPER_DIR "/OnlyInLowerSym", &StatBuffer), ENOENT);

    //
    // Unmount and check it was unmounted.
    //

    if (Fd != -1)
    {
        LxtClose(Fd);
        Fd = -1;
    }

    LxtCheckErrnoZeroSuccess(umount(OVFS_TEST_MOUNT_PATH));
    LxtCheckResult(MountCheckIsNotMount(OVFS_TEST_MOUNT_PATH));
    Result = 0;

ErrorExit:
    if (Mapping != MAP_FAILED)
    {
        munmap(Mapping, PAGE_SIZE);
    }

    if (Fd != -1)
    {
        LxtClose(Fd);
    }

    umount(OVFS_TEST_MOUNT_PATH);
    return Result;
}

int OvFsTestInodeOpaque(PLXT_ARGS Args)

/*++

Description:

    This routine tests inode opaque operations.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    char Buffer[100];
    int BytesRead;
    int Count;
    struct dirent* Entry;
    int EntryPosition;
    int Fd;
    int Result;
    struct stat StatBuffer;

    Fd = -1;

    //
    // Set up the directories and populate some state.
    //

    LxtCheckResult(OvFsTestDirsSetup());
    LxtCheckResult(OvFsTestDirsPopulate());

    //
    // Create a file in each directory.
    //

    LxtCheckErrno(Fd = creat(OVFS_TEST_LOWER_DIR "/OnlyInLowerDir/OnlyInLowerDirFile", 0777));
    LxtClose(Fd);
    Fd = -1;
    LxtCheckErrno(Fd = creat(OVFS_TEST_LOWER_DIR "/InBothDir/InBothDirLowerFile", 0777));
    LxtClose(Fd);
    Fd = -1;
    LxtCheckErrno(Fd = creat(OVFS_TEST_UPPER_DIR "/OnlyInUpperDir/OnlyInUpperDirFile", 0777));
    LxtClose(Fd);
    Fd = -1;
    LxtCheckErrno(Fd = creat(OVFS_TEST_UPPER_DIR "/InBothDir/InBothDirUpperFile", 0777));
    LxtClose(Fd);
    Fd = -1;

    //
    // Mount an overlayfs instance and check for the expected file state.
    //

    LxtCheckResult(MountCheckIsNotMount(OVFS_TEST_MOUNT_PATH));
    LxtCheckErrnoZeroSuccess(mount("myovfsnew", OVFS_TEST_MOUNT_PATH, OVFS_TEST_MOUNT_NAME, 0, OVFS_TEST_MOUNT_DEFAULT));

    LxtCheckErrno(stat(OVFS_TEST_MERGED_DIR "/InBothDir/InBothDirLowerFile", &StatBuffer));
    LxtCheckTrue(S_ISREG(StatBuffer.st_mode));
    LxtCheckErrno(stat(OVFS_TEST_MERGED_DIR "/InBothDir/InBothDirUpperFile", &StatBuffer));
    LxtCheckTrue(S_ISREG(StatBuffer.st_mode));

    LxtCheckErrnoFailure(stat(OVFS_TEST_UPPER_DIR "/OnlyInLowerDir", &StatBuffer), ENOENT);
    LxtCheckErrnoFailure(stat(OVFS_TEST_UPPER_DIR "/OnlyInLowerFile", &StatBuffer), ENOENT);
    LxtCheckErrnoFailure(stat(OVFS_TEST_UPPER_DIR "/OnlyInLowerSym", &StatBuffer), ENOENT);
    LxtCheckErrno(stat(OVFS_TEST_UPPER_DIR "/OnlyInUpperDir", &StatBuffer));
    LxtCheckTrue(S_ISDIR(StatBuffer.st_mode));
    LxtCheckErrno(stat(OVFS_TEST_UPPER_DIR "/OnlyInUpperFile", &StatBuffer));
    LxtCheckTrue(S_ISREG(StatBuffer.st_mode));
    LxtCheckErrno(lstat(OVFS_TEST_UPPER_DIR "/OnlyInUpperSym", &StatBuffer));
    LxtCheckTrue(S_ISLNK(StatBuffer.st_mode));
    LxtCheckErrno(stat(OVFS_TEST_UPPER_DIR "/InBothDir", &StatBuffer));
    LxtCheckTrue(S_ISDIR(StatBuffer.st_mode));
    LxtCheckErrno(stat(OVFS_TEST_UPPER_DIR "/InBothFile", &StatBuffer));
    LxtCheckTrue(S_ISREG(StatBuffer.st_mode));
    LxtCheckErrno(lstat(OVFS_TEST_UPPER_DIR "/InBothSym", &StatBuffer));
    LxtCheckTrue(S_ISLNK(StatBuffer.st_mode));

    //
    // Remove each directory and check that it is removed, first checking for
    // the expected failure code.
    //

    LxtCheckErrnoFailure(rmdir(OVFS_TEST_MERGED_DIR "/OnlyInLowerDir"), ENOTEMPTY);
    LxtCheckErrno(unlink(OVFS_TEST_MERGED_DIR "/OnlyInLowerDir/OnlyInLowerDirFile"));
    LxtCheckErrno(stat(OVFS_TEST_UPPER_DIR "/OnlyInLowerDir/OnlyInLowerDirFile", &StatBuffer));
    LxtCheckTrue(S_ISCHR(StatBuffer.st_mode));
    LxtCheckErrno(rmdir(OVFS_TEST_MERGED_DIR "/OnlyInLowerDir"));
    LxtCheckErrno(stat(OVFS_TEST_UPPER_DIR "/OnlyInLowerDir", &StatBuffer));
    LxtCheckTrue(S_ISCHR(StatBuffer.st_mode));

    LxtCheckErrnoFailure(rmdir(OVFS_TEST_MERGED_DIR "/OnlyInUpperDir"), ENOTEMPTY);
    LxtCheckErrno(unlink(OVFS_TEST_MERGED_DIR "/OnlyInUpperDir/OnlyInUpperDirFile"));
    LxtCheckErrnoFailure(stat(OVFS_TEST_MERGED_DIR "/OnlyInUpperDir/OnlyInUpperDirFile", &StatBuffer), ENOENT);
    LxtCheckErrno(rmdir(OVFS_TEST_MERGED_DIR "/OnlyInUpperDir"));
    LxtCheckErrnoFailure(stat(OVFS_TEST_UPPER_DIR "/OnlyInUpperDir", &StatBuffer), ENOENT);

    LxtCheckErrnoFailure(rmdir(OVFS_TEST_MERGED_DIR "/InBothDir"), ENOTEMPTY);
    LxtCheckErrno(unlink(OVFS_TEST_MERGED_DIR "/InBothDir/InBothDirUpperFile"));
    LxtCheckErrnoFailure(stat(OVFS_TEST_MERGED_DIR "/OnlyInUpperDir/OnlyInUpperDirFile", &StatBuffer), ENOENT);
    LxtCheckErrnoFailure(rmdir(OVFS_TEST_MERGED_DIR "/InBothDir"), ENOTEMPTY);
    LxtCheckErrno(unlink(OVFS_TEST_MERGED_DIR "/InBothDir/InBothDirLowerFile"));
    LxtCheckErrnoFailure(stat(OVFS_TEST_MERGED_DIR "/OnlyInLowerDir/OnlyInLowerDirFile", &StatBuffer), ENOENT);
    LxtCheckErrno(rmdir(OVFS_TEST_MERGED_DIR "/InBothDir"));
    LxtCheckErrno(stat(OVFS_TEST_UPPER_DIR "/InBothDir", &StatBuffer));
    LxtCheckTrue(S_ISCHR(StatBuffer.st_mode));

    //
    // Enumerate the top level and check that the three directories have been
    // removed.
    //

    Count = 0;
    LxtCheckErrno(Fd = open(OVFS_TEST_MOUNT_PATH, O_RDONLY));
    LxtCheckErrno(BytesRead = LxtGetdents64(Fd, (struct dirent*)Buffer, sizeof(Buffer)));

    while (BytesRead > 0)
    {
        for (EntryPosition = 0; EntryPosition < BytesRead; EntryPosition += Entry->d_reclen)
        {

            Entry = (struct dirent*)&Buffer[EntryPosition];
            LxtLogInfo("%s", Entry->d_name);
            Count += 1;
        }

        LxtCheckErrno(BytesRead = LxtGetdents64(Fd, (struct dirent*)Buffer, sizeof(Buffer)));
    }

    LxtCheckEqual(Count - 2, LXT_COUNT_OF(g_OvFsMergedContents) - 3, "%d");
    LxtClose(Fd);
    Fd = -1;

    //
    // Create entries over the whiteouts and check for the expected state.
    //
    //

    LxtCheckErrno(mkdir(OVFS_TEST_MERGED_DIR "/OnlyInLowerDir", 0777));
    LxtCheckErrno(stat(OVFS_TEST_MERGED_DIR "/OnlyInLowerDir", &StatBuffer));
    LxtCheckTrue(S_ISDIR(StatBuffer.st_mode));
    LxtCheckErrno(stat(OVFS_TEST_UPPER_DIR "/OnlyInLowerDir", &StatBuffer));
    LxtCheckTrue(S_ISDIR(StatBuffer.st_mode));
    LxtCheckErrno(Fd = open(OVFS_TEST_MERGED_DIR "/OnlyInLowerDir", O_RDONLY));
    LxtCheckErrno(BytesRead = LxtGetdents64(Fd, (struct dirent*)Buffer, sizeof(Buffer)))

        Count = 0;
    for (EntryPosition = 0; EntryPosition < BytesRead; EntryPosition += Entry->d_reclen)
    {

        Entry = (struct dirent*)&Buffer[EntryPosition];
        LxtLogInfo("%s", Entry->d_name);
        Count += 1;
    }

    LxtCheckEqual(Count, 2, "%d");
    LxtClose(Fd);
    Fd = -1;
    LxtCheckErrnoFailure(stat(OVFS_TEST_MERGED_DIR "/OnlyInLowerDir/OnlyInLowerDirFile", &StatBuffer), ENOENT);

    LxtCheckErrno(mkdir(OVFS_TEST_MERGED_DIR "/OnlyInUpperDir", 0777));
    LxtCheckErrno(stat(OVFS_TEST_MERGED_DIR "/OnlyInUpperDir", &StatBuffer));
    LxtCheckTrue(S_ISDIR(StatBuffer.st_mode));
    LxtCheckErrno(stat(OVFS_TEST_UPPER_DIR "/OnlyInUpperDir", &StatBuffer));
    LxtCheckTrue(S_ISDIR(StatBuffer.st_mode));
    LxtCheckErrno(Fd = open(OVFS_TEST_MERGED_DIR "/OnlyInUpperDir", O_RDONLY));
    LxtCheckErrno(BytesRead = LxtGetdents64(Fd, (struct dirent*)Buffer, sizeof(Buffer)))

        Count = 0;
    for (EntryPosition = 0; EntryPosition < BytesRead; EntryPosition += Entry->d_reclen)
    {

        Entry = (struct dirent*)&Buffer[EntryPosition];
        Count += 1;
    }

    LxtCheckEqual(Count, 2, "%d");
    LxtClose(Fd);
    Fd = -1;
    LxtCheckErrnoFailure(stat(OVFS_TEST_MERGED_DIR "/OnlyInUpperDir/OnlyInUpperDirFile", &StatBuffer), ENOENT);

    LxtCheckErrno(Fd = mkdir(OVFS_TEST_MERGED_DIR "/InBothDir", 0777));
    LxtCheckErrno(stat(OVFS_TEST_MERGED_DIR "/InBothDir", &StatBuffer));
    LxtCheckTrue(S_ISDIR(StatBuffer.st_mode));
    LxtCheckErrno(stat(OVFS_TEST_UPPER_DIR "/InBothDir", &StatBuffer));
    LxtCheckTrue(S_ISDIR(StatBuffer.st_mode));
    LxtCheckErrno(Fd = open(OVFS_TEST_MERGED_DIR "/InBothDir", O_RDONLY));
    LxtCheckErrno(BytesRead = LxtGetdents64(Fd, (struct dirent*)Buffer, sizeof(Buffer)))

        Count = 0;
    for (EntryPosition = 0; EntryPosition < BytesRead; EntryPosition += Entry->d_reclen)
    {

        Entry = (struct dirent*)&Buffer[EntryPosition];
        Count += 1;
    }

    LxtCheckEqual(Count, 2, "%d");
    LxtClose(Fd);
    Fd = -1;
    LxtCheckErrnoFailure(stat(OVFS_TEST_MERGED_DIR "/InBothDir/InBothDirLowerFile", &StatBuffer), ENOENT);
    LxtCheckErrnoFailure(stat(OVFS_TEST_MERGED_DIR "/InBothDir/InBothDirUpperFile", &StatBuffer), ENOENT);

    //
    // Enumerate the top level and check that the three directories have been
    // replaced.
    //

    Count = 0;
    LxtCheckErrno(Fd = open(OVFS_TEST_MOUNT_PATH, O_RDONLY));
    LxtCheckErrno(BytesRead = LxtGetdents64(Fd, (struct dirent*)Buffer, sizeof(Buffer)));

    while (BytesRead > 0)
    {
        for (EntryPosition = 0; EntryPosition < BytesRead; EntryPosition += Entry->d_reclen)
        {

            Entry = (struct dirent*)&Buffer[EntryPosition];
            Count += 1;
        }

        Entry = (struct dirent*)Buffer;
        LxtCheckErrno(BytesRead = LxtGetdents64(Fd, (struct dirent*)Buffer, sizeof(Buffer)));
    }

    LxtCheckEqual(Count - 2, LXT_COUNT_OF(g_OvFsMergedContents), "%d");
    LxtClose(Fd);
    Fd = -1;

    //
    // Replace a directory with a file and back again to a directory.
    //

    LxtCheckErrnoZeroSuccess(umount(OVFS_TEST_MOUNT_PATH));
    LxtCheckResult(MountCheckIsNotMount(OVFS_TEST_MOUNT_PATH));
    LxtCheckResult(OvFsTestDirsSetup());
    LxtCheckResult(OvFsTestDirsPopulate());
    LxtCheckErrnoZeroSuccess(mount("myovfsnew", OVFS_TEST_MOUNT_PATH, OVFS_TEST_MOUNT_NAME, 0, OVFS_TEST_MOUNT_DEFAULT));

    LxtCheckErrno(rmdir(OVFS_TEST_MERGED_DIR "/InBothDir"));
    LxtCheckErrnoFailure(stat(OVFS_TEST_MERGED_DIR "/InBothDir", &StatBuffer), ENOENT);
    LxtCheckErrno(stat(OVFS_TEST_UPPER_DIR "/InBothDir", &StatBuffer));
    LxtCheckTrue(S_ISCHR(StatBuffer.st_mode));
    LxtCheckErrno(Fd = creat(OVFS_TEST_MERGED_DIR "/InBothDir", 0777));
    LxtClose(Fd);
    Fd = -1;
    LxtCheckErrno(stat(OVFS_TEST_MERGED_DIR "/InBothDir", &StatBuffer));
    LxtCheckTrue(S_ISREG(StatBuffer.st_mode));
    LxtCheckErrno(stat(OVFS_TEST_UPPER_DIR "/InBothDir", &StatBuffer));
    LxtCheckTrue(S_ISREG(StatBuffer.st_mode));
    LxtCheckErrno(unlink(OVFS_TEST_MERGED_DIR "/InBothDir"));
    LxtCheckErrnoFailure(stat(OVFS_TEST_MERGED_DIR "/InBothDir", &StatBuffer), ENOENT);
    LxtCheckErrno(stat(OVFS_TEST_UPPER_DIR "/InBothDir", &StatBuffer));
    LxtCheckTrue(S_ISCHR(StatBuffer.st_mode));
    LxtCheckErrno(mkdir(OVFS_TEST_MERGED_DIR "/InBothDir", 0777));
    LxtCheckErrno(stat(OVFS_TEST_MERGED_DIR "/InBothDir", &StatBuffer));
    LxtCheckTrue(S_ISDIR(StatBuffer.st_mode));
    LxtCheckErrno(stat(OVFS_TEST_UPPER_DIR "/InBothDir", &StatBuffer));
    LxtCheckTrue(S_ISDIR(StatBuffer.st_mode));

    LxtCheckErrnoZeroSuccess(umount(OVFS_TEST_MOUNT_PATH));
    LxtCheckResult(MountCheckIsNotMount(OVFS_TEST_MOUNT_PATH));

    Result = 0;

ErrorExit:
    if (Fd != -1)
    {
        LxtClose(Fd);
    }

    umount(OVFS_TEST_MOUNT_PATH);
    return Result;
}

int OvFsTestInodeReadOps(PLXT_ARGS Args)

/*++

Description:

    This routine tests inode operations that do not modify state.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int Fd;
    char Path[128];
    ssize_t PathSize;
    int Result;
    struct stat StatBuffer;
    struct stat StatMergedBuffer;

    Fd = -1;

    //
    // Set up the directories and populate some state.
    //

    LxtCheckResult(OvFsTestDirsSetup());
    LxtCheckResult(OvFsTestDirsPopulate());

    //
    // Mount an overlayfs instance and check inode operations that do not
    // hydrate files.
    //

    LxtCheckResult(MountCheckIsNotMount(OVFS_TEST_MOUNT_PATH));
    LxtCheckErrnoZeroSuccess(mount("myovfsnew", OVFS_TEST_MOUNT_PATH, OVFS_TEST_MOUNT_NAME, 0, OVFS_TEST_MOUNT_DEFAULT));

    //
    // Check the behavior for open, lookup, and fstat. The inode numbers in the
    // merged folder should be unique for directories but match the files.
    //

    if (g_LxtUnstableInodes != 0)
    {
        LxtCheckResult(Fd = open(OVFS_TEST_MOUNT_PATH "/OnlyInLowerDir", O_RDONLY, 0));
        LxtCheckResult(fstat(Fd, &StatMergedBuffer));
        LxtClose(Fd);
        Fd = -1;
        LxtCheckResult(stat(OVFS_TEST_LOWER_DIR "/OnlyInLowerDir", &StatBuffer));
        LxtCheckNotEqual(StatBuffer.st_ino, StatMergedBuffer.st_ino, "%d");

        LxtCheckResult(Fd = open(OVFS_TEST_MOUNT_PATH "/InBothDir", O_RDONLY, 0));
        LxtCheckResult(fstat(Fd, &StatMergedBuffer));
        LxtClose(Fd);
        Fd = -1;
        LxtCheckResult(stat(OVFS_TEST_LOWER_DIR "/InBothDir", &StatBuffer));
        LxtCheckNotEqual(StatBuffer.st_ino, StatMergedBuffer.st_ino, "%d");
        LxtCheckResult(stat(OVFS_TEST_UPPER_DIR "/InBothDir", &StatBuffer));
        LxtCheckNotEqual(StatBuffer.st_ino, StatMergedBuffer.st_ino, "%d");

        LxtCheckResult(Fd = open(OVFS_TEST_MOUNT_PATH "/OnlyInLowerFile", O_RDONLY, 0));
        LxtCheckResult(fstat(Fd, &StatMergedBuffer));
        LxtClose(Fd);
        Fd = -1;
        LxtCheckResult(stat(OVFS_TEST_LOWER_DIR "/OnlyInLowerFile", &StatBuffer));
        LxtCheckEqual(StatBuffer.st_ino, StatMergedBuffer.st_ino, "%d");

        LxtCheckResult(Fd = open(OVFS_TEST_MOUNT_PATH "/InBothFile", O_RDONLY, 0));
        LxtCheckResult(fstat(Fd, &StatMergedBuffer));
        LxtClose(Fd);
        Fd = -1;
        LxtCheckResult(stat(OVFS_TEST_LOWER_DIR "/InBothFile", &StatBuffer));
        LxtCheckNotEqual(StatBuffer.st_ino, StatMergedBuffer.st_ino, "%d");
        LxtCheckResult(stat(OVFS_TEST_UPPER_DIR "/InBothFile", &StatBuffer));
        LxtCheckEqual(StatBuffer.st_ino, StatMergedBuffer.st_ino, "%d");

        LxtCheckResult(Fd = open(OVFS_TEST_MOUNT_PATH "/OnlyInUpperDir", O_RDONLY, 0));
        LxtCheckResult(fstat(Fd, &StatMergedBuffer));
        LxtClose(Fd);
        Fd = -1;
        LxtCheckResult(stat(OVFS_TEST_UPPER_DIR "/OnlyInUpperDir", &StatBuffer));
        LxtCheckNotEqual(StatBuffer.st_ino, StatMergedBuffer.st_ino, "%d");

        LxtCheckResult(Fd = open(OVFS_TEST_MOUNT_PATH "/OnlyInUpperFile", O_RDONLY, 0));
        LxtCheckResult(fstat(Fd, &StatMergedBuffer));
        LxtClose(Fd);
        Fd = -1;
        LxtCheckResult(stat(OVFS_TEST_UPPER_DIR "/OnlyInUpperFile", &StatBuffer));
        LxtCheckEqual(StatBuffer.st_ino, StatMergedBuffer.st_ino, "%d");
    }

    //
    // Check the behavior for readlink.
    //

    LxtCheckErrno(PathSize = readlink(OVFS_TEST_MOUNT_PATH "/OnlyInLowerSym", Path, sizeof(Path) - 1));

    Path[PathSize] = 0;
    LxtCheckStringEqual(Path, OVFS_TEST_LOWER_DIR "/OnlyInLowerFile");
    LxtCheckErrno(PathSize = readlink(OVFS_TEST_MOUNT_PATH "/InBothSym", Path, sizeof(Path) - 1));

    Path[PathSize] = 0;
    LxtCheckStringEqual(Path, OVFS_TEST_UPPER_DIR "/InBothFile");
    LxtCheckErrno(PathSize = readlink(OVFS_TEST_MOUNT_PATH "/OnlyInUpperSym", Path, sizeof(Path) - 1));

    Path[PathSize] = 0;
    LxtCheckStringEqual(Path, OVFS_TEST_UPPER_DIR "/OnlyInUpperFile");

    //
    //
    // Check the behavior for stat. The inode numbers in the merged folder
    // should be unique for directories but match the files.
    //

    if (g_LxtUnstableInodes != 0)
    {
        LxtCheckResult(stat(OVFS_TEST_MOUNT_PATH "/OnlyInLowerDir", &StatMergedBuffer));
        LxtCheckResult(stat(OVFS_TEST_LOWER_DIR "/OnlyInLowerDir", &StatBuffer));
        LxtCheckNotEqual(StatBuffer.st_ino, StatMergedBuffer.st_ino, "%d");

        LxtCheckResult(stat(OVFS_TEST_MOUNT_PATH "/InBothDir", &StatMergedBuffer));
        LxtCheckResult(stat(OVFS_TEST_LOWER_DIR "/InBothDir", &StatBuffer));
        LxtCheckNotEqual(StatBuffer.st_ino, StatMergedBuffer.st_ino, "%d");
        LxtCheckResult(stat(OVFS_TEST_UPPER_DIR "/InBothDir", &StatBuffer));
        LxtCheckNotEqual(StatBuffer.st_ino, StatMergedBuffer.st_ino, "%d");

        LxtCheckResult(stat(OVFS_TEST_MOUNT_PATH "/OnlyInLowerFile", &StatMergedBuffer));
        LxtCheckResult(stat(OVFS_TEST_LOWER_DIR "/OnlyInLowerFile", &StatBuffer));
        LxtCheckEqual(StatBuffer.st_ino, StatMergedBuffer.st_ino, "%d");

        LxtCheckResult(stat(OVFS_TEST_MOUNT_PATH "/InBothFile", &StatMergedBuffer));
        LxtCheckResult(stat(OVFS_TEST_LOWER_DIR "/InBothFile", &StatBuffer));
        LxtCheckNotEqual(StatBuffer.st_ino, StatMergedBuffer.st_ino, "%d");
        LxtCheckResult(stat(OVFS_TEST_UPPER_DIR "/InBothFile", &StatBuffer));
        LxtCheckEqual(StatBuffer.st_ino, StatMergedBuffer.st_ino, "%d");

        LxtCheckResult(stat(OVFS_TEST_MOUNT_PATH "/OnlyInUpperDir", &StatMergedBuffer));
        LxtCheckResult(stat(OVFS_TEST_UPPER_DIR "/OnlyInUpperDir", &StatBuffer));
        LxtCheckNotEqual(StatBuffer.st_ino, StatMergedBuffer.st_ino, "%d");

        LxtCheckResult(stat(OVFS_TEST_MOUNT_PATH "/OnlyInUpperFile", &StatMergedBuffer));
        LxtCheckResult(stat(OVFS_TEST_UPPER_DIR "/OnlyInUpperFile", &StatBuffer));
        LxtCheckEqual(StatBuffer.st_ino, StatMergedBuffer.st_ino, "%d");
    }

    //
    // Check that none of the operations hydrated files from the lower
    // directory.
    //

    LxtCheckErrnoFailure(stat(OVFS_TEST_UPPER_DIR "/OnlyInLowerDir", &StatBuffer), ENOENT);
    LxtCheckErrnoFailure(stat(OVFS_TEST_UPPER_DIR "/OnlyInLowerFile", &StatBuffer), ENOENT);
    LxtCheckErrnoFailure(stat(OVFS_TEST_UPPER_DIR "/OnlyInLowerSym", &StatBuffer), ENOENT);

    //
    // Unmount and check it was unmounted.
    //

    if (Fd != -1)
    {
        LxtClose(Fd);
        Fd = -1;
    }

    LxtCheckErrnoZeroSuccess(umount(OVFS_TEST_MOUNT_PATH));
    LxtCheckResult(MountCheckIsNotMount(OVFS_TEST_MOUNT_PATH));
    Result = 0;

ErrorExit:
    if (Fd != -1)
    {
        LxtClose(Fd);
    }

    umount(OVFS_TEST_MOUNT_PATH);
    return Result;
}

int OvFsTestInodeRename(PLXT_ARGS Args)

/*++

Description:

    This routine tests inode operations that may modify state.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    ssize_t BufferSize;
    char Buffer;
    int Result;
    struct stat StatBuffer;
    struct stat StatMergedBuffer;

    //
    // Set up the directories and populate some state.
    //

    LxtCheckResult(OvFsTestDirsSetup());
    LxtCheckResult(OvFsTestDirsPopulate());

    //
    // Mount an overlayfs instance.
    //

    LxtCheckResult(MountCheckIsNotMount(OVFS_TEST_MOUNT_PATH));
    LxtCheckErrnoZeroSuccess(mount("myovfsnew", OVFS_TEST_MOUNT_PATH, OVFS_TEST_MOUNT_NAME, 0, OVFS_TEST_MOUNT_DEFAULT));

    //
    // Rename each file and check for the expected state.
    //

    //
    // When renaming a file from the lower, a whiteout and the renamed file are
    // set in the upper.
    //

    LxtCheckErrnoFailure(stat(OVFS_TEST_UPPER_DIR "/OnlyInLowerFile", &StatBuffer), ENOENT);
    LxtCheckErrno(rename(OVFS_TEST_MERGED_DIR "/OnlyInLowerFile", OVFS_TEST_MERGED_DIR "/OnlyInLowerFileRenamed"));

    LxtCheckErrno(stat(OVFS_TEST_UPPER_DIR "/OnlyInLowerFile", &StatBuffer));
    LxtCheckTrue(S_ISCHR(StatBuffer.st_mode));
    LxtCheckErrno(stat(OVFS_TEST_UPPER_DIR "/OnlyInLowerFileRenamed", &StatBuffer));
    LxtCheckTrue(S_ISREG(StatBuffer.st_mode));
    LxtCheckErrno(stat(OVFS_TEST_MERGED_DIR "/OnlyInLowerFileRenamed", &StatMergedBuffer));
    LxtCheckTrue(S_ISREG(StatMergedBuffer.st_mode));
    if (g_LxtUnstableInodes != 0)
    {
        LxtCheckEqual(StatBuffer.st_ino, StatMergedBuffer.st_ino, "%d");
    }

    //
    // When renaming a file from the upper, the file is simply renamed.
    //

    LxtCheckErrno(stat(OVFS_TEST_UPPER_DIR "/OnlyInUpperFile", &StatBuffer));
    LxtCheckErrno(rename(OVFS_TEST_MERGED_DIR "/OnlyInUpperFile", OVFS_TEST_MERGED_DIR "/OnlyInUpperFileRenamed"));

    LxtCheckErrnoFailure(stat(OVFS_TEST_UPPER_DIR "/OnlyInUpperFile", &StatBuffer), ENOENT);
    LxtCheckErrno(stat(OVFS_TEST_UPPER_DIR "/OnlyInUpperFileRenamed", &StatBuffer));
    LxtCheckTrue(S_ISREG(StatBuffer.st_mode));
    LxtCheckErrno(stat(OVFS_TEST_MERGED_DIR "/OnlyInUpperFileRenamed", &StatMergedBuffer));
    LxtCheckTrue(S_ISREG(StatMergedBuffer.st_mode));
    if (g_LxtUnstableInodes != 0)
    {
        LxtCheckEqual(StatBuffer.st_ino, StatMergedBuffer.st_ino, "%d");
    }

    //
    // When renaming a file from both, a whiteout and the renamed file are
    // set in the upper.
    //

    LxtCheckErrno(stat(OVFS_TEST_UPPER_DIR "/InBothFile", &StatBuffer));
    LxtCheckTrue(S_ISREG(StatBuffer.st_mode));
    LxtCheckErrno(rename(OVFS_TEST_MERGED_DIR "/InBothFile", OVFS_TEST_MERGED_DIR "/InBothFileRenamed"));

    LxtCheckErrno(stat(OVFS_TEST_UPPER_DIR "/InBothFile", &StatBuffer));
    LxtCheckTrue(S_ISCHR(StatBuffer.st_mode));
    LxtCheckErrno(stat(OVFS_TEST_UPPER_DIR "/InBothFileRenamed", &StatBuffer));
    LxtCheckTrue(S_ISREG(StatBuffer.st_mode));
    LxtCheckErrno(stat(OVFS_TEST_MERGED_DIR "/InBothFileRenamed", &StatMergedBuffer));
    LxtCheckTrue(S_ISREG(StatMergedBuffer.st_mode));
    if (g_LxtUnstableInodes != 0)
    {
        LxtCheckEqual(StatBuffer.st_ino, StatMergedBuffer.st_ino, "%d");
    }

    //
    // Check the behavior for renaming a directory.
    //

    //
    // When renaming a directory from the lower, the rename call should fail.
    //

    LxtCheckErrnoFailure(stat(OVFS_TEST_UPPER_DIR "/OnlyInLowerDir", &StatBuffer), ENOENT);
    LxtCheckErrnoFailure(rename(OVFS_TEST_MERGED_DIR "/OnlyInLowerDir", OVFS_TEST_MERGED_DIR "/OnlyInLowerDirRenamed"), EXDEV);

    LxtCheckErrnoFailure(stat(OVFS_TEST_UPPER_DIR "/OnlyInLowerDir", &StatBuffer), ENOENT);

    //
    // When renaming a directory from the upper, directory is simply renamed.
    //

    LxtCheckErrno(stat(OVFS_TEST_UPPER_DIR "/OnlyInUpperDir", &StatBuffer));
    LxtCheckErrno(rename(OVFS_TEST_MERGED_DIR "/OnlyInUpperDir", OVFS_TEST_MERGED_DIR "/OnlyInUpperDirRenamed"));

    LxtCheckErrnoFailure(stat(OVFS_TEST_UPPER_DIR "/OnlyInUpperDir", &StatBuffer), ENOENT);
    LxtCheckErrno(stat(OVFS_TEST_UPPER_DIR "/OnlyInUpperDirRenamed", &StatBuffer));
    LxtCheckTrue(S_ISDIR(StatBuffer.st_mode));
    LxtCheckErrno(stat(OVFS_TEST_MERGED_DIR "/OnlyInUpperDirRenamed", &StatMergedBuffer));
    LxtCheckTrue(S_ISDIR(StatMergedBuffer.st_mode));
    if (g_LxtUnstableInodes != 0)
    {
        LxtCheckNotEqual(StatBuffer.st_ino, StatMergedBuffer.st_ino, "%d");
    }

    //
    // When renaming a directory from both, the rename call should fail.
    //

    LxtCheckErrno(stat(OVFS_TEST_UPPER_DIR "/InBothDir", &StatBuffer));
    LxtCheckErrnoFailure(rename(OVFS_TEST_MERGED_DIR "/InBothDir", OVFS_TEST_MERGED_DIR "/InBothDirRenamed"), EXDEV);

    LxtCheckErrno(stat(OVFS_TEST_UPPER_DIR "/InBothDir", &StatBuffer));
    LxtCheckErrnoFailure(stat(OVFS_TEST_UPPER_DIR "/InBothDirRenamed", &StatBuffer), ENOENT);

    //
    // When renaming an opaque directory, the rename call should succeed.
    //

    LxtCheckErrno(rmdir(OVFS_TEST_MERGED_DIR "/InBothDir"));
    LxtCheckErrno(mkdir(OVFS_TEST_MERGED_DIR "/InBothDir", 0777));
    LxtCheckErrno(stat(OVFS_TEST_UPPER_DIR "/InBothDir", &StatBuffer));
    LxtCheckTrue(S_ISDIR(StatBuffer.st_mode));
    LxtCheckErrno(BufferSize = getxattr(OVFS_TEST_UPPER_DIR "/InBothDir", "trusted.overlay.opaque", &Buffer, sizeof(Buffer)));

    LxtCheckEqual(Buffer, 'y', "%c");
    LxtCheckEqual(BufferSize, 1, "%d");
    LxtCheckErrno(rename(OVFS_TEST_MERGED_DIR "/InBothDir", OVFS_TEST_MERGED_DIR "/OnlyInLowerDir"));

    LxtCheckErrno(stat(OVFS_TEST_UPPER_DIR "/InBothDir", &StatBuffer));
    LxtCheckTrue(S_ISCHR(StatBuffer.st_mode));
    LxtCheckErrno(stat(OVFS_TEST_UPPER_DIR "/OnlyInLowerDir", &StatBuffer));
    LxtCheckTrue(S_ISDIR(StatBuffer.st_mode));
    LxtCheckErrno(BufferSize = getxattr(OVFS_TEST_UPPER_DIR "/OnlyInLowerDir", "trusted.overlay.opaque", &Buffer, sizeof(Buffer)));

    LxtCheckEqual(Buffer, 'y', "%c");
    LxtCheckEqual(BufferSize, 1, "%d");

    //
    // Unmount.
    //

    LxtCheckErrnoZeroSuccess(umount(OVFS_TEST_MOUNT_PATH));
    LxtCheckResult(MountCheckIsNotMount(OVFS_TEST_MOUNT_PATH));

    Result = 0;

ErrorExit:
    umount(OVFS_TEST_MOUNT_PATH);
    return Result;
}

int OvFsTestInodeWhiteout(PLXT_ARGS Args)

/*++

Description:

    This routine tests inode whiteout operations.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int Fd;
    int Result;
    struct stat StatBuffer;

    Fd = -1;

    //
    // Set up the directories and populate some state.
    //

    LxtCheckResult(OvFsTestDirsSetup());
    LxtCheckResult(OvFsTestDirsPopulate());

    //
    // Mount an overlayfs instance and check for the expected file state.
    //

    LxtCheckResult(MountCheckIsNotMount(OVFS_TEST_MOUNT_PATH));
    LxtCheckErrnoZeroSuccess(mount("myovfsnew", OVFS_TEST_MOUNT_PATH, OVFS_TEST_MOUNT_NAME, 0, OVFS_TEST_MOUNT_DEFAULT));

    LxtCheckErrnoFailure(stat(OVFS_TEST_UPPER_DIR "/OnlyInLowerDir", &StatBuffer), ENOENT);
    LxtCheckErrnoFailure(stat(OVFS_TEST_UPPER_DIR "/OnlyInLowerFile", &StatBuffer), ENOENT);
    LxtCheckErrnoFailure(stat(OVFS_TEST_UPPER_DIR "/OnlyInLowerSym", &StatBuffer), ENOENT);
    LxtCheckErrno(stat(OVFS_TEST_UPPER_DIR "/OnlyInUpperDir", &StatBuffer));
    LxtCheckTrue(S_ISDIR(StatBuffer.st_mode));
    LxtCheckErrno(stat(OVFS_TEST_UPPER_DIR "/OnlyInUpperFile", &StatBuffer));
    LxtCheckTrue(S_ISREG(StatBuffer.st_mode));
    LxtCheckErrno(lstat(OVFS_TEST_UPPER_DIR "/OnlyInUpperSym", &StatBuffer));
    LxtCheckTrue(S_ISLNK(StatBuffer.st_mode));
    LxtCheckErrno(stat(OVFS_TEST_UPPER_DIR "/InBothDir", &StatBuffer));
    LxtCheckTrue(S_ISDIR(StatBuffer.st_mode));
    LxtCheckErrno(stat(OVFS_TEST_UPPER_DIR "/InBothFile", &StatBuffer));
    LxtCheckTrue(S_ISREG(StatBuffer.st_mode));
    LxtCheckErrno(lstat(OVFS_TEST_UPPER_DIR "/InBothSym", &StatBuffer));
    LxtCheckTrue(S_ISLNK(StatBuffer.st_mode));

    //
    // Unlink each entry and check for the expected behavior.
    //

    LxtCheckErrno(rmdir(OVFS_TEST_MERGED_DIR "/OnlyInLowerDir"));
    LxtCheckErrno(stat(OVFS_TEST_UPPER_DIR "/OnlyInLowerDir", &StatBuffer));
    LxtCheckTrue(S_ISCHR(StatBuffer.st_mode));

    LxtCheckErrno(unlink(OVFS_TEST_MERGED_DIR "/OnlyInLowerFile"));
    LxtCheckErrno(stat(OVFS_TEST_UPPER_DIR "/OnlyInLowerFile", &StatBuffer));
    LxtCheckTrue(S_ISCHR(StatBuffer.st_mode));

    LxtCheckErrno(unlink(OVFS_TEST_MERGED_DIR "/OnlyInLowerSym"));
    LxtCheckErrno(stat(OVFS_TEST_UPPER_DIR "/OnlyInLowerSym", &StatBuffer));
    LxtCheckTrue(S_ISCHR(StatBuffer.st_mode));

    LxtCheckErrno(rmdir(OVFS_TEST_MERGED_DIR "/OnlyInUpperDir"));
    LxtCheckErrnoFailure(stat(OVFS_TEST_UPPER_DIR "/OnlyInUpperDir", &StatBuffer), ENOENT);

    LxtCheckErrno(unlink(OVFS_TEST_MERGED_DIR "/OnlyInUpperFile"));
    LxtCheckErrnoFailure(stat(OVFS_TEST_UPPER_DIR "/OnlyInUpperFile", &StatBuffer), ENOENT);

    LxtCheckErrno(unlink(OVFS_TEST_MERGED_DIR "/OnlyInUpperSym"));
    LxtCheckErrnoFailure(stat(OVFS_TEST_UPPER_DIR "/OnlyInUpperSym", &StatBuffer), ENOENT);

    LxtCheckErrno(rmdir(OVFS_TEST_MERGED_DIR "/InBothDir"));
    LxtCheckErrno(stat(OVFS_TEST_UPPER_DIR "/InBothDir", &StatBuffer));
    LxtCheckTrue(S_ISCHR(StatBuffer.st_mode));

    LxtCheckErrno(unlink(OVFS_TEST_MERGED_DIR "/InBothFile"));
    LxtCheckErrno(stat(OVFS_TEST_UPPER_DIR "/InBothFile", &StatBuffer));
    LxtCheckTrue(S_ISCHR(StatBuffer.st_mode));

    LxtCheckErrno(unlink(OVFS_TEST_MERGED_DIR "/InBothSym"));
    LxtCheckErrno(stat(OVFS_TEST_UPPER_DIR "/InBothSym", &StatBuffer));
    LxtCheckTrue(S_ISCHR(StatBuffer.st_mode));

    //
    // Create entries over the whiteouts and check for the expected state.
    //

    LxtCheckErrno(mkdir(OVFS_TEST_MERGED_DIR "/OnlyInLowerDir", 0777));
    LxtCheckErrno(stat(OVFS_TEST_UPPER_DIR "/OnlyInLowerDir", &StatBuffer));
    LxtCheckTrue(S_ISDIR(StatBuffer.st_mode));

    LxtCheckErrno(Fd = creat(OVFS_TEST_MERGED_DIR "/OnlyInLowerFile", 0777));
    LxtCheckErrno(stat(OVFS_TEST_UPPER_DIR "/OnlyInLowerFile", &StatBuffer));
    LxtCheckTrue(S_ISREG(StatBuffer.st_mode));
    LxtClose(Fd);
    Fd = -1;

    LxtCheckErrno(Fd = creat(OVFS_TEST_MERGED_DIR "/OnlyInLowerSym", 0777));
    LxtCheckErrno(stat(OVFS_TEST_UPPER_DIR "/OnlyInLowerSym", &StatBuffer));
    LxtCheckTrue(S_ISREG(StatBuffer.st_mode));
    LxtClose(Fd);
    Fd = -1;

    LxtCheckErrno(mkdir(OVFS_TEST_MERGED_DIR "/OnlyInUpperDir", 0777));
    LxtCheckErrno(stat(OVFS_TEST_UPPER_DIR "/OnlyInLowerDir", &StatBuffer));
    LxtCheckTrue(S_ISDIR(StatBuffer.st_mode));

    LxtCheckErrno(Fd = creat(OVFS_TEST_MERGED_DIR "/OnlyInUpperFile", 0777));
    LxtCheckErrno(stat(OVFS_TEST_UPPER_DIR "/OnlyInUpperFile", &StatBuffer));
    LxtClose(Fd);
    Fd = -1;

    LxtCheckErrno(Fd = creat(OVFS_TEST_MERGED_DIR "/OnlyInUpperSym", 0777));
    LxtCheckErrno(stat(OVFS_TEST_UPPER_DIR "/OnlyInUpperSym", &StatBuffer));
    LxtCheckTrue(S_ISREG(StatBuffer.st_mode));
    LxtClose(Fd);
    Fd = -1;

    LxtCheckErrno(mkdir(OVFS_TEST_MERGED_DIR "/InBothDir", 0777));
    LxtCheckErrno(stat(OVFS_TEST_UPPER_DIR "/InBothDir", &StatBuffer));
    LxtCheckTrue(S_ISDIR(StatBuffer.st_mode));

    LxtCheckErrno(mkdir(OVFS_TEST_MERGED_DIR "/InBothFile", 0777));
    LxtCheckErrno(stat(OVFS_TEST_UPPER_DIR "/InBothFile", &StatBuffer));
    LxtCheckTrue(S_ISDIR(StatBuffer.st_mode));

    LxtCheckErrno(Fd = creat(OVFS_TEST_MERGED_DIR "/InBothSym", 0777));
    LxtCheckErrno(stat(OVFS_TEST_UPPER_DIR "/InBothSym", &StatBuffer));
    LxtCheckTrue(S_ISREG(StatBuffer.st_mode));
    LxtClose(Fd);
    Fd = -1;

    LxtCheckErrnoZeroSuccess(umount(OVFS_TEST_MOUNT_PATH));
    LxtCheckResult(MountCheckIsNotMount(OVFS_TEST_MOUNT_PATH));

    Result = 0;

ErrorExit:
    if (Fd != -1)
    {
        LxtClose(Fd);
    }

    umount(OVFS_TEST_MOUNT_PATH);
    return Result;
}

int OvFsTestInodeWriteOpsUpper(PLXT_ARGS Args)

/*++

Description:

    This routine tests inode operations that do not modify state.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    char Buffer[100];
    int BytesRead;
    int Count;
    struct dirent* Entry;
    int EntryPosition;
    int Fd;
    char* writeUpperCreate[] = {"writeUpperDir", "writeUpperFile", "writeUpperSymlink", "writeUpperLink"};
    int Found[LXT_COUNT_OF(g_OvFsMergedContents) + 2 + LXT_COUNT_OF(writeUpperCreate)];
    int FoundIndex;
    int Index;
    char Path[128];
    ssize_t PathSize;
    int Result;
    struct stat StatBuffer;
    struct stat StatMergedBuffer;
    struct utimbuf Times;

    Fd = -1;

    //
    // Set up the directories and populate some state.
    //

    LxtCheckResult(OvFsTestDirsSetup());
    LxtCheckResult(OvFsTestDirsPopulate());

    //
    // Mount an overlayfs instance and check inode operations that do not
    // hydrate files.
    //
    // N.B. The overlay fs mount does not need to be recreated after each
    //      variation since only the upper is modified.
    //

    LxtCheckResult(MountCheckIsNotMount(OVFS_TEST_MOUNT_PATH));
    LxtCheckErrnoZeroSuccess(mount("myovfsnew", OVFS_TEST_MOUNT_PATH, OVFS_TEST_MOUNT_NAME, 0, OVFS_TEST_MOUNT_DEFAULT));

    //
    // Check the behavior for open, chown, chmod, create cases, set times. The
    // inode numbers in the merged folder should be unique for directories but
    // match the files.
    //

    if (g_LxtUnstableInodes != 0)
    {
        LxtCheckResult(Fd = open(OVFS_TEST_MOUNT_PATH "/InBothFile", O_RDWR, 0));
        LxtCheckResult(fstat(Fd, &StatMergedBuffer));
        LxtClose(Fd);
        Fd = -1;
        LxtCheckResult(stat(OVFS_TEST_LOWER_DIR "/InBothFile", &StatBuffer));
        LxtCheckNotEqual(StatBuffer.st_ino, StatMergedBuffer.st_ino, "%d");
        LxtCheckResult(stat(OVFS_TEST_UPPER_DIR "/InBothFile", &StatBuffer));
        LxtCheckEqual(StatBuffer.st_ino, StatMergedBuffer.st_ino, "%d");

        LxtCheckResult(Fd = open(OVFS_TEST_MOUNT_PATH "/OnlyInUpperFile", O_RDWR, 0));
        LxtCheckResult(fstat(Fd, &StatMergedBuffer));
        LxtClose(Fd);
        Fd = -1;
        LxtCheckResult(stat(OVFS_TEST_UPPER_DIR "/OnlyInUpperFile", &StatBuffer));
        LxtCheckEqual(StatBuffer.st_ino, StatMergedBuffer.st_ino, "%d");

        LxtCheckResult(Fd = open(OVFS_TEST_MOUNT_PATH "/InBothSym", O_RDWR | O_PATH | O_NOFOLLOW, 0));
        LxtCheckResult(fstat(Fd, &StatMergedBuffer));
        LxtClose(Fd);
        Fd = -1;
        LxtCheckResult(lstat(OVFS_TEST_LOWER_DIR "/InBothSym", &StatBuffer));
        LxtCheckNotEqual(StatBuffer.st_ino, StatMergedBuffer.st_ino, "%d");
        LxtCheckResult(lstat(OVFS_TEST_UPPER_DIR "/InBothSym", &StatBuffer));
        LxtCheckEqual(StatBuffer.st_ino, StatMergedBuffer.st_ino, "%d");

        LxtCheckResult(Fd = open(OVFS_TEST_MOUNT_PATH "/OnlyInUpperSym", O_RDWR | O_PATH | O_NOFOLLOW, 0));
        LxtCheckResult(fstat(Fd, &StatMergedBuffer));
        LxtClose(Fd);
        Fd = -1;
        LxtCheckResult(lstat(OVFS_TEST_UPPER_DIR "/OnlyInUpperSym", &StatBuffer));
        LxtCheckEqual(StatBuffer.st_ino, StatMergedBuffer.st_ino, "%d");
    }

    //
    // Check the behavior for chown.
    //

    for (Index = 0; Index < LXT_COUNT_OF(g_OvFsMergedContents); ++Index)
    {
        if (((S_ISREG(g_OvFsMergedContents[Index].Mode) == FALSE) && (S_ISDIR(g_OvFsMergedContents[Index].Mode) == FALSE)) ||
            (g_OvFsMergedContents[Index].Hydrates != 0))
        {

            continue;
        }

        LxtCheckErrno(chown(g_OvFsMergedContents[Index].Path, 111, 111));
    }

    //
    // Check the behavior for chmod.
    //

    for (Index = 0; Index < LXT_COUNT_OF(g_OvFsMergedContents); ++Index)
    {
        if (((S_ISREG(g_OvFsMergedContents[Index].Mode) == FALSE) && (S_ISDIR(g_OvFsMergedContents[Index].Mode) == FALSE)) ||
            (g_OvFsMergedContents[Index].Hydrates != 0))
        {

            continue;
        }

        LxtCheckErrno(chmod(g_OvFsMergedContents[Index].Path, 0777));
    }

    //
    // Check the behavior for the 4 create cases.
    //

    LxtCheckErrno(mkdir(OVFS_TEST_MOUNT_PATH "/writeUpperDir", 0777));
    LxtCheckResult(Fd = open(OVFS_TEST_MOUNT_PATH "/writeUpperDir", O_RDONLY, 0));
    LxtCheckResult(fstat(Fd, &StatMergedBuffer));
    LxtClose(Fd);
    Fd = -1;
    LxtCheckResult(stat(OVFS_TEST_UPPER_DIR "/writeUpperDir", &StatBuffer));
    if (g_LxtUnstableInodes != 0)
    {
        LxtCheckNotEqual(StatBuffer.st_ino, StatMergedBuffer.st_ino, "%d");
    }

    LxtCheckErrnoFailure(stat(OVFS_TEST_LOWER_DIR "/writeUpperDir", &StatBuffer), ENOENT);

    LxtCheckErrno(Fd = creat(OVFS_TEST_MOUNT_PATH "/writeUpperFile", 0777));
    LxtCheckResult(fstat(Fd, &StatMergedBuffer));
    LxtClose(Fd);
    Fd = -1;
    LxtCheckResult(stat(OVFS_TEST_UPPER_DIR "/writeUpperFile", &StatBuffer));
    if (g_LxtUnstableInodes != 0)
    {
        LxtCheckEqual(StatBuffer.st_ino, StatMergedBuffer.st_ino, "%d");
    }

    LxtCheckErrnoFailure(stat(OVFS_TEST_LOWER_DIR "/writeUpperFile", &StatBuffer), ENOENT);

    LxtCheckErrno(symlink("writeUpperSymlink", OVFS_TEST_MOUNT_PATH "/writeUpperSymlink"));
    LxtCheckResult(Fd = open(OVFS_TEST_MOUNT_PATH "/writeUpperSymlink", O_RDONLY | O_PATH | O_NOFOLLOW, 0));
    LxtCheckResult(fstat(Fd, &StatMergedBuffer));
    LxtClose(Fd);
    Fd = -1;
    LxtCheckResult(lstat(OVFS_TEST_UPPER_DIR "/writeUpperSymlink", &StatBuffer));
    if (g_LxtUnstableInodes != 0)
    {
        LxtCheckEqual(StatBuffer.st_ino, StatMergedBuffer.st_ino, "%d");
    }

    LxtCheckErrnoFailure(lstat(OVFS_TEST_LOWER_DIR "/writeUpperSymlink", &StatBuffer), ENOENT);

    LxtCheckErrno(link(OVFS_TEST_MOUNT_PATH "/writeUpperFile", OVFS_TEST_MOUNT_PATH "/writeUpperLink"));
    LxtCheckResult(Fd = open(OVFS_TEST_MOUNT_PATH "/writeUpperLink", O_RDONLY, 0));
    LxtCheckResult(fstat(Fd, &StatMergedBuffer));
    LxtClose(Fd);
    Fd = -1;
    LxtCheckResult(stat(OVFS_TEST_UPPER_DIR "/writeUpperLink", &StatBuffer));
    if (g_LxtUnstableInodes != 0)
    {
        LxtCheckEqual(StatBuffer.st_ino, StatMergedBuffer.st_ino, "%d");
    }

    LxtCheckErrnoFailure(stat(OVFS_TEST_LOWER_DIR "/writeUpperLink", &StatBuffer), ENOENT);

    //
    // Check the behavior for set times.
    //

    Times.actime = time(NULL);
    Times.modtime = time(NULL);
    for (Index = 0; Index < LXT_COUNT_OF(g_OvFsMergedContents); ++Index)
    {
        if (((S_ISREG(g_OvFsMergedContents[Index].Mode) == FALSE) && (S_ISDIR(g_OvFsMergedContents[Index].Mode) == FALSE)) ||
            (g_OvFsMergedContents[Index].Hydrates != 0))
        {

            continue;
        }

        LxtCheckErrno(utime(g_OvFsMergedContents[Index].Path, &Times));
    }

    //
    // Check the behavior for read directory on the root after new files were
    // added.
    //

    memset(Found, 0, sizeof(Found));
    LxtCheckErrno(Fd = open(OVFS_TEST_MOUNT_PATH, O_RDONLY));
    LxtCheckErrno(BytesRead = LxtGetdents64(Fd, (struct dirent*)Buffer, sizeof(Buffer)));

    while (BytesRead > 0)
    {
        for (EntryPosition = 0; EntryPosition < BytesRead; EntryPosition += Entry->d_reclen)
        {

            Entry = (struct dirent*)&Buffer[EntryPosition];
            if (strcmp(Entry->d_name, ".") == 0)
            {
                FoundIndex = 0;
            }
            else if (strcmp(Entry->d_name, "..") == 0)
            {
                FoundIndex = 1;
            }
            else
            {
                for (Index = 0; Index < LXT_COUNT_OF(g_OvFsMergedContents); ++Index)
                {
                    if (strcmp(g_OvFsMergedContents[Index].Name, Entry->d_name) == 0)
                    {
                        FoundIndex = Index + 2;
                        break;
                    }
                }

                if (Index == LXT_COUNT_OF(g_OvFsMergedContents))
                {
                    for (Index = 0; Index < LXT_COUNT_OF(writeUpperCreate); ++Index)
                    {
                        if (strcmp(writeUpperCreate[Index], Entry->d_name) == 0)
                        {
                            FoundIndex = Index + 2 + LXT_COUNT_OF(g_OvFsMergedContents);
                            break;
                        }
                    }

                    if (Index == LXT_COUNT_OF(writeUpperCreate))
                    {
                        LxtLogError("Unexpected entry %s", Entry->d_name);
                        LxtCheckNotEqual(Index, LXT_COUNT_OF(writeUpperCreate), "%d");
                    }
                }
            }

            LxtCheckEqual(Found[FoundIndex], 0, "%d");
            Found[FoundIndex] = 1;
        }

        Entry = (struct dirent*)Buffer;
        LxtCheckErrno(BytesRead = LxtGetdents64(Fd, (struct dirent*)Buffer, sizeof(Buffer)));
    }

    for (Index = 0; Index < LXT_COUNT_OF(Found); ++Index)
    {
        LxtCheckEqual(Found[FoundIndex], 1, "%d");
    }

    LxtClose(Fd);
    Fd = -1;

    //
    // Check the behavior for read directory on sub directories.
    //

    for (Index = 0; Index < LXT_COUNT_OF(g_OvFsMergedContents); ++Index)
    {
        if (S_ISDIR(g_OvFsMergedContents[Index].Mode) == FALSE)
        {
            continue;
        }

        LxtCheckErrno(Fd = open(g_OvFsMergedContents[Index].Path, O_RDONLY));
        LxtCheckErrno(BytesRead = LxtGetdents64(Fd, (struct dirent*)Buffer, sizeof(Buffer)))

            Count = 0;
        for (EntryPosition = 0; EntryPosition < BytesRead; EntryPosition += Entry->d_reclen)
        {

            Entry = (struct dirent*)&Buffer[EntryPosition];
            Count += 1;
        }

        LxtCheckEqual(Count, 2, "%d");
        LxtClose(Fd);
        Fd = -1;
    }

    //
    // Check that none of the operations hydrated files from the lower
    // directory.
    //

    LxtCheckErrnoFailure(stat(OVFS_TEST_UPPER_DIR "/OnlyInLowerDir", &StatBuffer), ENOENT);
    LxtCheckErrnoFailure(stat(OVFS_TEST_UPPER_DIR "/OnlyInLowerFile", &StatBuffer), ENOENT);
    LxtCheckErrnoFailure(stat(OVFS_TEST_UPPER_DIR "/OnlyInLowerSym", &StatBuffer), ENOENT);

    //
    // Unmount and check it was unmounted.
    //

    if (Fd != -1)
    {
        LxtClose(Fd);
        Fd = -1;
    }

    LxtCheckErrnoZeroSuccess(umount(OVFS_TEST_MOUNT_PATH));
    LxtCheckResult(MountCheckIsNotMount(OVFS_TEST_MOUNT_PATH));
    Result = 0;

ErrorExit:
    if (Fd != -1)
    {
        LxtClose(Fd);
    }

    umount(OVFS_TEST_MOUNT_PATH);
    return Result;
}

int OvFsTestInodeWriteOps(PLXT_ARGS Args)

/*++

Description:

    This routine tests inode operations that may modify state.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    char BytesRead;
    char Buffer[100];
    struct __user_cap_data_struct CapData[2];
    struct __user_cap_header_struct CapHeader;
    pid_t ChildPid;
    int Fd;
    int FdWrite;
    int Result;
    struct stat StatBuffer;
    struct stat StatBufferWrite;

    ChildPid = -1;
    Fd = -1;
    FdWrite = -1;

    //
    // Set up the directories and populate some state.
    //

    LxtCheckResult(OvFsTestDirsSetup());
    LxtCheckResult(OvFsTestDirsPopulate());

    //
    // Mount an overlayfs instance.
    //

    LxtCheckResult(MountCheckIsNotMount(OVFS_TEST_MOUNT_PATH));
    LxtCheckErrnoZeroSuccess(mount("myovfsnew", OVFS_TEST_MOUNT_PATH, OVFS_TEST_MOUNT_NAME, 0, OVFS_TEST_MOUNT_DEFAULT));

    //
    // Open the same file for read and write. The read file will be from the
    // lower layer, but opening the file for write will cause the file to be
    // hydrated in the upper layer and the inode updated.
    //

    LxtCheckErrno(Fd = open(OVFS_TEST_MOUNT_PATH "/OnlyInLowerFile", O_RDONLY, 0));
    LxtCheckErrno(fstat(Fd, &StatBuffer));
    LxtCheckErrnoFailure(stat(OVFS_TEST_UPPER_DIR "/OnlyInLowerFile", &StatBufferWrite), ENOENT);
    LxtCheckErrno(FdWrite = open(OVFS_TEST_MOUNT_PATH "/OnlyInLowerFile", O_RDWR, 0));
    LxtCheckErrno(stat(OVFS_TEST_UPPER_DIR "/OnlyInLowerFile", &StatBufferWrite));
    LxtCheckErrno(fstat(FdWrite, &StatBufferWrite));
    if (g_LxtUnstableInodes != 0)
    {
        LxtCheckNotEqual(StatBuffer.st_ino, StatBufferWrite.st_ino, "%d");
    }

    LxtCheckEqual(StatBuffer.st_mode, StatBufferWrite.st_mode, "%d");

    //
    // Check that the inode numbers are the same now that both files are open.
    // Any open files referring to this inode will access the new inode
    // metadata.
    //

    LxtCheckErrno(fstat(Fd, &StatBuffer));
    if (g_LxtUnstableInodes != 0)
    {
        LxtCheckEqual(StatBuffer.st_ino, StatBufferWrite.st_ino, "%d");
    }

    LxtCheckErrno(fstat(FdWrite, &StatBufferWrite));
    if (g_LxtUnstableInodes != 0)
    {
        LxtCheckEqual(StatBuffer.st_ino, StatBufferWrite.st_ino, "%d");
    }

    LxtCheckEqual(StatBuffer.st_mode, StatBufferWrite.st_mode, "%d");

    //
    // Check that chmod on one of the file descriptors impacts both. Any open
    // files referring to this inode will access the new inode metadata.
    //

    LxtCheckNotEqual(StatBuffer.st_mode, S_IFREG | 0111, "%d");
    LxtCheckErrno(fchmod(FdWrite, 0111));
    LxtCheckErrno(fstat(Fd, &StatBuffer));
    LxtCheckErrno(fstat(FdWrite, &StatBufferWrite));
    LxtLogInfo("%d, %d", StatBuffer.st_mode, StatBufferWrite.st_mode);
    LxtCheckEqual(StatBuffer.st_mode, S_IFREG | 0111, "%d");
    LxtCheckEqual(StatBufferWrite.st_mode, S_IFREG | 0111, "%d");

    //
    // Check that writing only impacts the file object opened for write and not
    // the one opened for read. Any open files objects will access the old file
    // data.
    //

    LxtCheckErrno(write(FdWrite, "OnlyInLowerFileModified", sizeof("OnlyInLowerFileModified")));
    LxtCheckErrno(BytesRead = read(Fd, Buffer, sizeof(Buffer) - 1));
    Buffer[BytesRead] = 0;
    LxtCheckStringEqual(Buffer, "OnlyInLowerFile") LxtCheckErrno(lseek(FdWrite, SEEK_SET, 0));
    LxtCheckErrno(BytesRead = read(FdWrite, Buffer, sizeof(Buffer) - 1));
    Buffer[BytesRead] = 0;
    LxtCheckStringEqual(Buffer, "OnlyInLowerFileModified")

        //
        // Unmount and check it was unmounted.
        //

        if (Fd != -1)
    {
        LxtClose(Fd);
        Fd = -1;
    }

    if (FdWrite != -1)
    {
        LxtClose(FdWrite);
        FdWrite = -1;
    }

    LxtCheckErrnoZeroSuccess(umount(OVFS_TEST_MOUNT_PATH));
    LxtCheckResult(MountCheckIsNotMount(OVFS_TEST_MOUNT_PATH));

    //
    // Check that chmod, chown, and utime hydrate files.
    //

    LxtCheckResult(OvFsTestDirsSetup());
    LxtCheckResult(OvFsTestDirsPopulate());
    LxtCheckResult(MountCheckIsNotMount(OVFS_TEST_MOUNT_PATH));
    LxtCheckErrnoZeroSuccess(mount("myovfsnew", OVFS_TEST_MOUNT_PATH, OVFS_TEST_MOUNT_NAME, 0, OVFS_TEST_MOUNT_DEFAULT));

    LxtCheckErrnoFailure(stat(OVFS_TEST_UPPER_DIR "/OnlyInLowerDir", &StatBuffer), ENOENT);
    LxtCheckErrnoFailure(stat(OVFS_TEST_UPPER_DIR "/OnlyInLowerFile", &StatBuffer), ENOENT);
    LxtCheckErrnoFailure(stat(OVFS_TEST_UPPER_DIR "/OnlyInLowerSym", &StatBuffer), ENOENT);
    LxtCheckErrno(chmod(OVFS_TEST_MOUNT_PATH "/OnlyInLowerDir", 0777));
    LxtCheckErrno(stat(OVFS_TEST_UPPER_DIR "/OnlyInLowerDir", &StatBuffer));
    LxtCheckErrnoFailure(stat(OVFS_TEST_UPPER_DIR "/OnlyInLowerFile", &StatBuffer), ENOENT);
    LxtCheckErrnoFailure(lstat(OVFS_TEST_UPPER_DIR "/OnlyInLowerSym", &StatBuffer), ENOENT);
    LxtCheckErrnoZeroSuccess(umount(OVFS_TEST_MOUNT_PATH));
    LxtCheckResult(MountCheckIsNotMount(OVFS_TEST_MOUNT_PATH));

    LxtCheckResult(OvFsTestDirsSetup());
    LxtCheckResult(OvFsTestDirsPopulate());
    LxtCheckResult(MountCheckIsNotMount(OVFS_TEST_MOUNT_PATH));
    LxtCheckErrnoZeroSuccess(mount("myovfsnew", OVFS_TEST_MOUNT_PATH, OVFS_TEST_MOUNT_NAME, 0, OVFS_TEST_MOUNT_DEFAULT));

    LxtCheckErrnoFailure(stat(OVFS_TEST_UPPER_DIR "/OnlyInLowerDir", &StatBuffer), ENOENT);
    LxtCheckErrnoFailure(stat(OVFS_TEST_UPPER_DIR "/OnlyInLowerFile", &StatBuffer), ENOENT);
    LxtCheckErrnoFailure(stat(OVFS_TEST_UPPER_DIR "/OnlyInLowerSym", &StatBuffer), ENOENT);
    LxtCheckErrno(chown(OVFS_TEST_MOUNT_PATH "/OnlyInLowerFile", 1, 1));
    LxtCheckErrnoFailure(stat(OVFS_TEST_UPPER_DIR "/OnlyInLowerDir", &StatBuffer), ENOENT);
    LxtCheckErrno(stat(OVFS_TEST_UPPER_DIR "/OnlyInLowerFile", &StatBuffer));
    LxtCheckErrnoFailure(lstat(OVFS_TEST_UPPER_DIR "/OnlyInLowerSym", &StatBuffer), ENOENT);
    LxtCheckErrnoZeroSuccess(umount(OVFS_TEST_MOUNT_PATH));
    LxtCheckResult(MountCheckIsNotMount(OVFS_TEST_MOUNT_PATH));

    LxtCheckResult(OvFsTestDirsSetup());
    LxtCheckResult(OvFsTestDirsPopulate());
    LxtCheckResult(MountCheckIsNotMount(OVFS_TEST_MOUNT_PATH));
    LxtCheckErrnoZeroSuccess(mount("myovfsnew", OVFS_TEST_MOUNT_PATH, OVFS_TEST_MOUNT_NAME, 0, OVFS_TEST_MOUNT_DEFAULT));

    LxtCheckErrnoFailure(stat(OVFS_TEST_UPPER_DIR "/OnlyInLowerDir", &StatBuffer), ENOENT);
    LxtCheckErrnoFailure(stat(OVFS_TEST_UPPER_DIR "/OnlyInLowerFile", &StatBuffer), ENOENT);
    LxtCheckErrnoFailure(stat(OVFS_TEST_UPPER_DIR "/OnlyInLowerSym", &StatBuffer), ENOENT);
    LxtCheckErrno(utimensat(-1, OVFS_TEST_MOUNT_PATH "/OnlyInLowerSym", NULL, AT_SYMLINK_NOFOLLOW));
    LxtCheckErrnoFailure(stat(OVFS_TEST_UPPER_DIR "/OnlyInLowerDir", &StatBuffer), ENOENT);
    LxtCheckErrnoFailure(stat(OVFS_TEST_UPPER_DIR "/OnlyInLowerFile", &StatBuffer), ENOENT);
    LxtCheckErrno(lstat(OVFS_TEST_UPPER_DIR "/OnlyInLowerSym", &StatBuffer));
    LxtCheckErrnoZeroSuccess(umount(OVFS_TEST_MOUNT_PATH));
    LxtCheckResult(MountCheckIsNotMount(OVFS_TEST_MOUNT_PATH));

    //
    // Check that the 4 types of creation hydrate paths.
    //

    LxtCheckResult(OvFsTestDirsSetup());
    LxtCheckResult(OvFsTestDirsPopulate());
    LxtCheckResult(MountCheckIsNotMount(OVFS_TEST_MOUNT_PATH));
    LxtCheckErrnoZeroSuccess(mount("myovfsnew", OVFS_TEST_MOUNT_PATH, OVFS_TEST_MOUNT_NAME, 0, OVFS_TEST_MOUNT_DEFAULT));

    LxtCheckErrnoFailure(stat(OVFS_TEST_UPPER_DIR "/OnlyInLowerDir", &StatBuffer), ENOENT);
    LxtCheckErrnoFailure(stat(OVFS_TEST_UPPER_DIR "/OnlyInLowerFile", &StatBuffer), ENOENT);
    LxtCheckErrnoFailure(stat(OVFS_TEST_UPPER_DIR "/OnlyInLowerSym", &StatBuffer), ENOENT);
    LxtCheckErrno(symlink("CreatedSymlink", OVFS_TEST_MOUNT_PATH "/OnlyInLowerDir/CreatedSymlink"));
    LxtCheckErrno(stat(OVFS_TEST_UPPER_DIR "/OnlyInLowerDir", &StatBuffer));
    LxtCheckErrno(lstat(OVFS_TEST_UPPER_DIR "/OnlyInLowerDir/CreatedSymlink", &StatBuffer));
    LxtCheckErrnoFailure(stat(OVFS_TEST_UPPER_DIR "/OnlyInLowerFile", &StatBuffer), ENOENT);
    LxtCheckErrnoFailure(lstat(OVFS_TEST_UPPER_DIR "/OnlyInLowerSym", &StatBuffer), ENOENT);
    LxtCheckErrnoZeroSuccess(umount(OVFS_TEST_MOUNT_PATH));
    LxtCheckResult(MountCheckIsNotMount(OVFS_TEST_MOUNT_PATH));

    LxtCheckResult(OvFsTestDirsSetup());
    LxtCheckResult(OvFsTestDirsPopulate());
    LxtCheckResult(MountCheckIsNotMount(OVFS_TEST_MOUNT_PATH));
    LxtCheckErrnoZeroSuccess(mount("myovfsnew", OVFS_TEST_MOUNT_PATH, OVFS_TEST_MOUNT_NAME, 0, OVFS_TEST_MOUNT_DEFAULT));

    LxtCheckErrnoFailure(stat(OVFS_TEST_UPPER_DIR "/OnlyInLowerDir", &StatBuffer), ENOENT);
    LxtCheckErrnoFailure(stat(OVFS_TEST_UPPER_DIR "/OnlyInLowerFile", &StatBuffer), ENOENT);
    LxtCheckErrnoFailure(stat(OVFS_TEST_UPPER_DIR "/OnlyInLowerSym", &StatBuffer), ENOENT);
    LxtCheckErrno(Fd = creat(OVFS_TEST_MOUNT_PATH "/OnlyInLowerDir/CreatedFile", 0777));
    LxtClose(Fd);
    Fd = -1;
    LxtCheckErrno(stat(OVFS_TEST_UPPER_DIR "/OnlyInLowerDir", &StatBuffer));
    LxtCheckErrno(stat(OVFS_TEST_UPPER_DIR "/OnlyInLowerDir/CreatedFile", &StatBuffer));
    LxtCheckErrnoFailure(stat(OVFS_TEST_UPPER_DIR "/OnlyInLowerFile", &StatBuffer), ENOENT);
    LxtCheckErrnoFailure(lstat(OVFS_TEST_UPPER_DIR "/OnlyInLowerSym", &StatBuffer), ENOENT);
    LxtCheckErrnoZeroSuccess(umount(OVFS_TEST_MOUNT_PATH));
    LxtCheckResult(MountCheckIsNotMount(OVFS_TEST_MOUNT_PATH));

    LxtCheckResult(OvFsTestDirsSetup());
    LxtCheckResult(OvFsTestDirsPopulate());
    LxtCheckResult(MountCheckIsNotMount(OVFS_TEST_MOUNT_PATH));
    LxtCheckErrnoZeroSuccess(mount("myovfsnew", OVFS_TEST_MOUNT_PATH, OVFS_TEST_MOUNT_NAME, 0, OVFS_TEST_MOUNT_DEFAULT));

    LxtCheckErrnoFailure(stat(OVFS_TEST_UPPER_DIR "/OnlyInLowerDir", &StatBuffer), ENOENT);
    LxtCheckErrnoFailure(stat(OVFS_TEST_UPPER_DIR "/OnlyInLowerFile", &StatBuffer), ENOENT);
    LxtCheckErrnoFailure(stat(OVFS_TEST_UPPER_DIR "/OnlyInLowerSym", &StatBuffer), ENOENT);
    LxtCheckErrno(mkdir(OVFS_TEST_MOUNT_PATH "/OnlyInLowerDir/CreatedDir", 0777));
    LxtCheckErrno(stat(OVFS_TEST_UPPER_DIR "/OnlyInLowerDir", &StatBuffer));
    LxtCheckErrno(stat(OVFS_TEST_UPPER_DIR "/OnlyInLowerDir/CreatedDir", &StatBuffer));
    LxtCheckErrnoFailure(stat(OVFS_TEST_UPPER_DIR "/OnlyInLowerFile", &StatBuffer), ENOENT);
    LxtCheckErrnoFailure(lstat(OVFS_TEST_UPPER_DIR "/OnlyInLowerSym", &StatBuffer), ENOENT);
    LxtCheckErrnoZeroSuccess(umount(OVFS_TEST_MOUNT_PATH));
    LxtCheckResult(MountCheckIsNotMount(OVFS_TEST_MOUNT_PATH));

    LxtCheckResult(OvFsTestDirsSetup());
    LxtCheckResult(OvFsTestDirsPopulate());
    LxtCheckResult(MountCheckIsNotMount(OVFS_TEST_MOUNT_PATH));
    LxtCheckErrnoZeroSuccess(mount("myovfsnew", OVFS_TEST_MOUNT_PATH, OVFS_TEST_MOUNT_NAME, 0, OVFS_TEST_MOUNT_DEFAULT));

    LxtCheckErrnoFailure(stat(OVFS_TEST_UPPER_DIR "/OnlyInLowerDir", &StatBuffer), ENOENT);
    LxtCheckErrnoFailure(stat(OVFS_TEST_UPPER_DIR "/OnlyInLowerFile", &StatBuffer), ENOENT);
    LxtCheckErrnoFailure(stat(OVFS_TEST_UPPER_DIR "/OnlyInLowerSym", &StatBuffer), ENOENT);
    LxtCheckErrno(link(OVFS_TEST_MOUNT_PATH "/OnlyInLowerFile", OVFS_TEST_MOUNT_PATH "/OnlyInLowerDir/CreatedLink"));
    LxtCheckErrno(stat(OVFS_TEST_UPPER_DIR "/OnlyInLowerDir", &StatBuffer));
    LxtCheckErrno(stat(OVFS_TEST_UPPER_DIR "/OnlyInLowerDir/CreatedLink", &StatBuffer));
    LxtCheckErrno(stat(OVFS_TEST_UPPER_DIR "/OnlyInLowerFile", &StatBuffer));
    LxtCheckErrnoFailure(lstat(OVFS_TEST_UPPER_DIR "/OnlyInLowerSym", &StatBuffer), ENOENT);
    LxtCheckErrnoZeroSuccess(umount(OVFS_TEST_MOUNT_PATH));
    LxtCheckResult(MountCheckIsNotMount(OVFS_TEST_MOUNT_PATH));

    //
    // Check the undefined behavior for O_RDONLY with O_TRUNC
    //

    LxtCheckResult(OvFsTestDirsSetup());
    LxtCheckResult(OvFsTestDirsPopulate());
    LxtCheckResult(MountCheckIsNotMount(OVFS_TEST_MOUNT_PATH));
    LxtCheckErrnoZeroSuccess(mount("myovfsnew", OVFS_TEST_MOUNT_PATH, OVFS_TEST_MOUNT_NAME, 0, OVFS_TEST_MOUNT_DEFAULT));

    LxtCheckErrno(Fd = open(OVFS_TEST_MOUNT_PATH "/OnlyInLowerFile", O_RDONLY | O_TRUNC, 0));
    LxtCheckErrnoFailure(ftruncate(Fd, 0), EINVAL);
    LxtClose(Fd);
    Fd = -1;
    LxtCheckErrno(stat(OVFS_TEST_LOWER_DIR "/OnlyInLowerFile", &StatBuffer));
    LxtCheckNotEqual(StatBuffer.st_size, 0, "%d");
    LxtCheckErrno(stat(OVFS_TEST_UPPER_DIR "/OnlyInLowerFile", &StatBuffer));
    LxtCheckEqual(StatBuffer.st_size, 0, "%d");

    LxtCheckErrno(Fd = open(OVFS_TEST_MOUNT_PATH "/OnlyInUpperFile", O_RDONLY | O_TRUNC, 0));
    LxtCheckErrnoFailure(ftruncate(Fd, 0), EINVAL);
    LxtClose(Fd);
    Fd = -1;
    LxtCheckErrnoFailure(stat(OVFS_TEST_LOWER_DIR "/OnlyInUpperFile", &StatBuffer), ENOENT);
    LxtCheckErrno(stat(OVFS_TEST_UPPER_DIR "/OnlyInUpperFile", &StatBuffer));
    LxtCheckEqual(StatBuffer.st_size, 0, "%d");

    LxtCheckErrno(Fd = open(OVFS_TEST_MOUNT_PATH "/InBothFile", O_RDONLY | O_TRUNC, 0));
    LxtCheckErrnoFailure(ftruncate(Fd, 0), EINVAL);
    LxtClose(Fd);
    Fd = -1;
    LxtCheckErrno(stat(OVFS_TEST_LOWER_DIR "/OnlyInLowerFile", &StatBuffer));
    LxtCheckNotEqual(StatBuffer.st_size, 0, "%d");
    LxtCheckErrno(stat(OVFS_TEST_UPPER_DIR "/InBothFile", &StatBuffer));
    LxtCheckEqual(StatBuffer.st_size, 0, "%d");

    LxtCheckErrnoZeroSuccess(umount(OVFS_TEST_MOUNT_PATH));
    LxtCheckResult(MountCheckIsNotMount(OVFS_TEST_MOUNT_PATH));

    //
    // Repeat the above for a file outside of overlay.
    //

    LxtCheckResult(OvFsTestDirsSetup());
    LxtCheckResult(OvFsTestDirsPopulate());
    LxtCheckResult(MountCheckIsNotMount(OVFS_TEST_MOUNT_PATH));
    LxtCheckErrno(stat(OVFS_TEST_LOWER_DIR "/OnlyInLowerFile", &StatBuffer));
    LxtCheckNotEqual(StatBuffer.st_size, 0, "%d");
    LxtCheckErrno(Fd = open(OVFS_TEST_LOWER_DIR "/OnlyInLowerFile", O_RDONLY | O_TRUNC, 0));
    LxtCheckErrnoFailure(ftruncate(Fd, 0), EINVAL);
    LxtClose(Fd);
    Fd = -1;
    LxtCheckErrno(stat(OVFS_TEST_LOWER_DIR "/OnlyInLowerFile", &StatBuffer));
    LxtCheckEqual(StatBuffer.st_size, 0, "%d");

    //
    // Repeat the above for a file where write access is not granted.
    //

    LxtCheckResult(OvFsTestDirsSetup());
    LxtCheckResult(OvFsTestDirsPopulate());
    LxtCheckResult(MountCheckIsNotMount(OVFS_TEST_MOUNT_PATH));
    LxtCheckErrno(stat(OVFS_TEST_LOWER_DIR "/OnlyInLowerFile", &StatBuffer));
    LxtCheckNotEqual(StatBuffer.st_size, 0, "%d");
    LxtCheckErrno(chmod(OVFS_TEST_LOWER_DIR "/OnlyInLowerFile", 0444));
    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {

        //
        // Drop privileges so the current process does not have VFS capabilities
        // and is in another user\group.
        //

        memset(&CapData, 0, sizeof(CapData));
        memset(&CapHeader, 0, sizeof(CapHeader));
        CapHeader.version = _LINUX_CAPABILITY_VERSION_3;
        LxtCheckErrno(prctl(PR_SET_KEEPCAPS, 1));
        LxtCheckErrno(setgid(2002));
        LxtCheckErrno(setuid(2002));
        LxtCheckErrno(LxtCapSet(&CapHeader, CapData));

        //
        // Open the file with different truncate variations.
        //

        LxtCheckErrno(Fd = open(OVFS_TEST_LOWER_DIR "/OnlyInLowerFile", O_RDONLY, 0));
        LxtClose(Fd);
        Fd = -1;
        LxtCheckErrnoFailure(Fd = open(OVFS_TEST_LOWER_DIR "/OnlyInLowerFile", O_RDWR, 0), EACCES);
        LxtCheckErrnoFailure(Fd = open(OVFS_TEST_LOWER_DIR "/OnlyInLowerFile", O_RDONLY | O_TRUNC, 0), EACCES);
        _exit(0);
    }

    LxtCheckResult(LxtWaitPidPoll(ChildPid, 0));

    Result = 0;

ErrorExit:
    if (ChildPid == 0)
    {
        _exit(Result);
    }

    if (Fd != -1)
    {
        LxtClose(Fd);
    }

    if (FdWrite != -1)
    {
        LxtClose(FdWrite);
    }

    umount(OVFS_TEST_MOUNT_PATH);
    return Result;
}

int OvFsTestInodeUnlink(PLXT_ARGS Args)

/*++

Description:

    This routine tests inode operations unlink.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int Fd;
    int FdLower;
    int Result;
    struct stat StatBuffer;

    Fd = -1;
    FdLower = -1;

    //
    // Set up the directories and populate some state.
    //

    LxtCheckResult(OvFsTestDirsSetup());
    LxtCheckResult(OvFsTestDirsPopulate());

    //
    // Mount an overlayfs instance and check for the expected file state.
    //

    LxtCheckResult(MountCheckIsNotMount(OVFS_TEST_MOUNT_PATH));
    LxtCheckErrnoZeroSuccess(mount("myovfsnew", OVFS_TEST_MOUNT_PATH, OVFS_TEST_MOUNT_NAME, 0, OVFS_TEST_MOUNT_DEFAULT));

    LxtCheckErrnoFailure(stat(OVFS_TEST_UPPER_DIR "/OnlyInLowerDir", &StatBuffer), ENOENT);
    LxtCheckErrnoFailure(stat(OVFS_TEST_UPPER_DIR "/OnlyInLowerFile", &StatBuffer), ENOENT);
    LxtCheckErrnoFailure(stat(OVFS_TEST_UPPER_DIR "/OnlyInLowerSym", &StatBuffer), ENOENT);
    LxtCheckErrno(stat(OVFS_TEST_UPPER_DIR "/OnlyInUpperDir", &StatBuffer));
    LxtCheckTrue(S_ISDIR(StatBuffer.st_mode));
    LxtCheckErrno(stat(OVFS_TEST_UPPER_DIR "/OnlyInUpperFile", &StatBuffer));
    LxtCheckTrue(S_ISREG(StatBuffer.st_mode));
    LxtCheckErrno(lstat(OVFS_TEST_UPPER_DIR "/OnlyInUpperSym", &StatBuffer));
    LxtCheckTrue(S_ISLNK(StatBuffer.st_mode));
    LxtCheckErrno(stat(OVFS_TEST_UPPER_DIR "/InBothDir", &StatBuffer));
    LxtCheckTrue(S_ISDIR(StatBuffer.st_mode));
    LxtCheckErrno(stat(OVFS_TEST_UPPER_DIR "/InBothFile", &StatBuffer));
    LxtCheckTrue(S_ISREG(StatBuffer.st_mode));
    LxtCheckErrno(lstat(OVFS_TEST_UPPER_DIR "/InBothSym", &StatBuffer));
    LxtCheckTrue(S_ISLNK(StatBuffer.st_mode));

    //
    // Unlink each entry and check for the expected behavior.
    //

    LxtCheckErrno(rmdir(OVFS_TEST_MERGED_DIR "/OnlyInLowerDir"));
    LxtCheckErrno(stat(OVFS_TEST_UPPER_DIR "/OnlyInLowerDir", &StatBuffer));
    LxtCheckTrue(S_ISCHR(StatBuffer.st_mode));

    LxtCheckErrno(unlink(OVFS_TEST_MERGED_DIR "/OnlyInLowerFile"));
    LxtCheckErrno(stat(OVFS_TEST_UPPER_DIR "/OnlyInLowerFile", &StatBuffer));
    LxtCheckTrue(S_ISCHR(StatBuffer.st_mode));

    LxtCheckErrno(unlink(OVFS_TEST_MERGED_DIR "/OnlyInLowerSym"));
    LxtCheckErrno(stat(OVFS_TEST_UPPER_DIR "/OnlyInLowerSym", &StatBuffer));
    LxtCheckTrue(S_ISCHR(StatBuffer.st_mode));

    LxtCheckErrno(rmdir(OVFS_TEST_MERGED_DIR "/OnlyInUpperDir"));
    LxtCheckErrnoFailure(stat(OVFS_TEST_UPPER_DIR "/OnlyInUpperDir", &StatBuffer), ENOENT);

    LxtCheckErrno(unlink(OVFS_TEST_MERGED_DIR "/OnlyInUpperFile"));
    LxtCheckErrnoFailure(stat(OVFS_TEST_UPPER_DIR "/OnlyInUpperFile", &StatBuffer), ENOENT);

    LxtCheckErrno(unlink(OVFS_TEST_MERGED_DIR "/OnlyInUpperSym"));
    LxtCheckErrnoFailure(stat(OVFS_TEST_UPPER_DIR "/OnlyInUpperSym", &StatBuffer), ENOENT);

    LxtCheckErrno(rmdir(OVFS_TEST_MERGED_DIR "/InBothDir"));
    LxtCheckErrno(stat(OVFS_TEST_UPPER_DIR "/InBothDir", &StatBuffer));
    LxtCheckTrue(S_ISCHR(StatBuffer.st_mode));

    LxtCheckErrno(unlink(OVFS_TEST_MERGED_DIR "/InBothFile"));
    LxtCheckErrno(stat(OVFS_TEST_UPPER_DIR "/InBothFile", &StatBuffer));
    LxtCheckTrue(S_ISCHR(StatBuffer.st_mode));

    LxtCheckErrno(unlink(OVFS_TEST_MERGED_DIR "/InBothSym"));
    LxtCheckErrno(stat(OVFS_TEST_UPPER_DIR "/InBothSym", &StatBuffer));
    LxtCheckTrue(S_ISCHR(StatBuffer.st_mode));

    //
    // Check that the lower file is detected during open and not on unlink.
    //

    LxtCheckErrnoZeroSuccess(umount(OVFS_TEST_MOUNT_PATH));
    LxtCheckResult(MountCheckIsNotMount(OVFS_TEST_MOUNT_PATH));
    LxtCheckResult(OvFsTestDirsSetup());
    LxtCheckResult(OvFsTestDirsPopulate());
    LxtCheckResult(MountCheckIsNotMount(OVFS_TEST_MOUNT_PATH));
    LxtCheckErrnoZeroSuccess(mount("myovfsnew", OVFS_TEST_MOUNT_PATH, OVFS_TEST_MOUNT_NAME, 0, OVFS_TEST_MOUNT_DEFAULT));

    LxtCheckErrno(Fd = open(OVFS_TEST_MERGED_DIR "/OnlyInUpperFile", O_RDWR));
    LxtCheckErrno(FdLower = creat(OVFS_TEST_LOWER_DIR "/OnlyInUpperFile", 0777));
    LxtCheckErrno(unlink(OVFS_TEST_MERGED_DIR "/OnlyInUpperFile"));
    LxtCheckErrnoFailure(stat(OVFS_TEST_UPPER_DIR "/OnlyInUpperFile", &StatBuffer), ENOENT);

    //
    // Repeat the above, but create the file in the lower first.
    //

    LxtClose(Fd);
    Fd = -1;
    LxtClose(FdLower);
    FdLower = -1;
    LxtCheckErrnoZeroSuccess(umount(OVFS_TEST_MOUNT_PATH));
    LxtCheckResult(MountCheckIsNotMount(OVFS_TEST_MOUNT_PATH));
    LxtCheckResult(OvFsTestDirsSetup());
    LxtCheckResult(OvFsTestDirsPopulate());
    LxtCheckResult(MountCheckIsNotMount(OVFS_TEST_MOUNT_PATH));
    LxtCheckErrnoZeroSuccess(mount("myovfsnew", OVFS_TEST_MOUNT_PATH, OVFS_TEST_MOUNT_NAME, 0, OVFS_TEST_MOUNT_DEFAULT));

    LxtCheckErrno(FdLower = creat(OVFS_TEST_LOWER_DIR "/OnlyInUpperFile", 0777));
    LxtCheckErrno(Fd = open(OVFS_TEST_MERGED_DIR "/OnlyInUpperFile", O_RDWR));
    LxtCheckErrno(unlink(OVFS_TEST_MERGED_DIR "/OnlyInUpperFile"));
    LxtCheckErrno(stat(OVFS_TEST_UPPER_DIR "/OnlyInUpperFile", &StatBuffer));
    LxtCheckTrue(S_ISCHR(StatBuffer.st_mode));
    LxtClose(Fd);
    Fd = -1;
    LxtClose(FdLower);
    FdLower = -1;

    //
    // Check the behavior for unlink while a file is open.
    //

    LxtCheckErrnoZeroSuccess(umount(OVFS_TEST_MOUNT_PATH));
    LxtCheckResult(MountCheckIsNotMount(OVFS_TEST_MOUNT_PATH));
    LxtCheckResult(OvFsTestDirsSetup());
    LxtCheckResult(OvFsTestDirsPopulate());
    LxtCheckResult(MountCheckIsNotMount(OVFS_TEST_MOUNT_PATH));
    LxtCheckErrnoZeroSuccess(mount("myovfsnew", OVFS_TEST_MOUNT_PATH, OVFS_TEST_MOUNT_NAME, 0, OVFS_TEST_MOUNT_DEFAULT));

    LxtCheckErrno(FdLower = open(OVFS_TEST_MERGED_DIR "/OnlyInLowerFile", O_RDONLY));
    LxtCheckErrno(fstat(FdLower, &StatBuffer));
    LxtCheckTrue(S_ISREG(StatBuffer.st_mode));
    LxtCheckErrno(Fd = open(OVFS_TEST_MERGED_DIR "/OnlyInLowerFile", O_RDWR));
    LxtCheckErrno(fstat(Fd, &StatBuffer));
    LxtCheckTrue(S_ISREG(StatBuffer.st_mode));
    LxtCheckErrno(unlink(OVFS_TEST_MERGED_DIR "/OnlyInLowerFile"));

    LxtCheckErrno(fstat(Fd, &StatBuffer));
    LxtCheckTrue(S_ISREG(StatBuffer.st_mode));
    LxtCheckErrno(fstat(FdLower, &StatBuffer));
    LxtCheckTrue(S_ISREG(StatBuffer.st_mode));
    LxtCheckErrno(fchmod(FdLower, 0777));
    LxtCheckErrno(fchmod(Fd, 0777));
    LxtClose(Fd);
    Fd = -1;
    LxtClose(FdLower);
    FdLower = -1;

    LxtCheckErrno(Fd = open(OVFS_TEST_MERGED_DIR "/OnlyInUpperFile", O_RDONLY));
    LxtCheckErrno(fstat(Fd, &StatBuffer));
    LxtCheckTrue(S_ISREG(StatBuffer.st_mode));
    LxtCheckErrno(unlink(OVFS_TEST_MERGED_DIR "/OnlyInUpperFile"));
    LxtCheckErrno(fstat(Fd, &StatBuffer));
    LxtCheckTrue(S_ISREG(StatBuffer.st_mode));
    LxtCheckErrno(fchmod(Fd, 0777));
    LxtClose(Fd);
    Fd = -1;

    //
    // Check the behavior for rmdir while a directory is open.
    //

    LxtCheckErrnoZeroSuccess(umount(OVFS_TEST_MOUNT_PATH));
    LxtCheckResult(MountCheckIsNotMount(OVFS_TEST_MOUNT_PATH));
    LxtCheckResult(OvFsTestDirsSetup());
    LxtCheckResult(OvFsTestDirsPopulate());
    LxtCheckResult(MountCheckIsNotMount(OVFS_TEST_MOUNT_PATH));
    LxtCheckErrnoZeroSuccess(mount("myovfsnew", OVFS_TEST_MOUNT_PATH, OVFS_TEST_MOUNT_NAME, 0, OVFS_TEST_MOUNT_DEFAULT));

    LxtCheckErrno(FdLower = open(OVFS_TEST_MERGED_DIR "/OnlyInLowerDir", O_RDONLY));
    LxtCheckErrno(fstat(FdLower, &StatBuffer));
    LxtCheckTrue(S_ISDIR(StatBuffer.st_mode));
    LxtCheckErrno(rmdir(OVFS_TEST_MERGED_DIR "/OnlyInLowerDir"));
    LxtCheckErrno(fstat(FdLower, &StatBuffer));
    LxtCheckTrue(S_ISDIR(StatBuffer.st_mode));
    LxtCheckErrnoFailure(fchmod(FdLower, 0777), ENOTDIR);

    LxtCheckErrno(Fd = open(OVFS_TEST_MERGED_DIR "/InBothDir", O_RDONLY));
    LxtCheckErrno(fstat(Fd, &StatBuffer));
    LxtCheckTrue(S_ISDIR(StatBuffer.st_mode));
    LxtCheckErrno(rmdir(OVFS_TEST_MERGED_DIR "/InBothDir"));
    LxtCheckErrno(fstat(Fd, &StatBuffer));
    LxtCheckTrue(S_ISDIR(StatBuffer.st_mode));
    LxtCheckErrno(fchmod(Fd, 0777));

    //
    // Unmount and check it was unmounted.
    //

    if (Fd != -1)
    {
        LxtClose(Fd);
        Fd = -1;
    }

    if (FdLower != -1)
    {
        LxtClose(FdLower);
        FdLower = -1;
    }

    LxtCheckErrnoZeroSuccess(umount(OVFS_TEST_MOUNT_PATH));
    LxtCheckResult(MountCheckIsNotMount(OVFS_TEST_MOUNT_PATH));

    Result = 0;

ErrorExit:
    if (Fd != -1)
    {
        LxtClose(Fd);
    }

    if (FdLower != -1)
    {
        LxtClose(FdLower);
    }

    umount(OVFS_TEST_MOUNT_PATH);
    return Result;
}

int OvFsTestInodeXattr(PLXT_ARGS Args)

/*++

Description:

    This routine tests inode operations that hydrate xattrs.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int Count;
    struct
    {
        char* Path;
        char* Name;
    } HydratedData[] = {
        {OVFS_TEST_UPPER_DIR "/OnlyInLowerDir", "OnlyInLowerDir"}, {OVFS_TEST_UPPER_DIR "/OnlyInLowerFile", "OnlyInLowerFile"}};

    int Index;
    char* ListCurrent;
    char List[256];
    ssize_t ListSize;
    int Result;
    struct stat StatBuffer;
    char Value[64];
    ssize_t ValueSize;

    //
    // Set up the directories and populate some state.
    //

    LxtCheckResult(OvFsTestDirsSetup());
    LxtCheckResult(OvFsTestDirsPopulate());

    //
    // Mount an overlayfs instance.
    //

    LxtCheckResult(MountCheckIsNotMount(OVFS_TEST_MOUNT_PATH));
    LxtCheckErrnoZeroSuccess(mount("myovfsnew", OVFS_TEST_MOUNT_PATH, OVFS_TEST_MOUNT_NAME, 0, OVFS_TEST_MOUNT_DEFAULT));

    //
    // Check the xattr state in the merged directory.
    //

    for (Index = 0; Index < LXT_COUNT_OF(g_OvFsMergedContents); ++Index)
    {
        if ((S_ISDIR(g_OvFsMergedContents[Index].Mode) == FALSE) && (S_ISREG(g_OvFsMergedContents[Index].Mode) == FALSE))
        {

            continue;
        }

        LxtCheckErrno(ListSize = listxattr(g_OvFsMergedContents[Index].Path, List, sizeof(List)));

        ListCurrent = List;
        Count = 0;
        for (;;)
        {
            LxtCheckErrno(ValueSize = getxattr(g_OvFsMergedContents[Index].Path, ListCurrent, Value, sizeof(Value)));

            LxtCheckStringEqual(Value, g_OvFsMergedContents[Index].Name);
            ListCurrent += strlen(ListCurrent) + 1;
            Count += 1;
            if (ListCurrent >= List + ListSize)
            {
                break;
            }
        }

        LxtCheckEqual(Count, 2, "%d");
    }

    //
    // Hydrate files and check that the xattr state was copied.
    //

    LxtCheckErrnoFailure(stat(OVFS_TEST_UPPER_DIR "/OnlyInLowerDir", &StatBuffer), ENOENT);
    LxtCheckErrnoFailure(stat(OVFS_TEST_UPPER_DIR "/OnlyInLowerFile", &StatBuffer), ENOENT);
    LxtCheckResult(chmod(OVFS_TEST_MERGED_DIR "/OnlyInLowerDir", 0111));
    LxtCheckResult(chmod(OVFS_TEST_MERGED_DIR "/OnlyInLowerFile", 0111));

    for (Index = 0; Index < LXT_COUNT_OF(HydratedData); ++Index)
    {
        LxtCheckErrno(stat(HydratedData[Index].Path, &StatBuffer));
        LxtCheckErrno(ListSize = listxattr(HydratedData[Index].Path, List, sizeof(List)));

        ListCurrent = List;
        Count = 0;
        for (;;)
        {
            LxtCheckErrno(ValueSize = getxattr(HydratedData[Index].Path, ListCurrent, Value, sizeof(Value)));

            LxtCheckStringEqual(Value, HydratedData[Index].Name);
            ListCurrent += strlen(ListCurrent) + 1;
            Count += 1;
            if (ListCurrent >= List + ListSize)
            {
                break;
            }
        }

        LxtCheckEqual(Count, 2, "%d");
    }

    LxtCheckErrnoZeroSuccess(umount(OVFS_TEST_MOUNT_PATH));
    LxtCheckResult(MountCheckIsNotMount(OVFS_TEST_MOUNT_PATH));

    //
    // Check the behavior for hydration of "trusted.overlay.opaque" where the
    // value is dropped.
    //

    LxtCheckResult(OvFsTestDirsSetup());
    LxtCheckResult(OvFsTestDirsPopulate());
    Value[0] = 'y';
    LxtCheckErrno(setxattr(OVFS_TEST_LOWER_DIR "/OnlyInLowerDir", "trusted.overlay.opaque", Value, 1, XATTR_CREATE));

    LxtCheckErrno(setxattr(OVFS_TEST_LOWER_DIR "/OnlyInLowerFile", "trusted.overlay.opaque", Value, 1, XATTR_CREATE));

    LxtCheckResult(MountCheckIsNotMount(OVFS_TEST_MOUNT_PATH));
    LxtCheckErrnoZeroSuccess(mount("myovfsnew", OVFS_TEST_MOUNT_PATH, OVFS_TEST_MOUNT_NAME, 0, OVFS_TEST_MOUNT_DEFAULT));

    LxtCheckErrnoFailure(stat(OVFS_TEST_UPPER_DIR "/OnlyInLowerDir", &StatBuffer), ENOENT);
    LxtCheckErrnoFailure(stat(OVFS_TEST_UPPER_DIR "/OnlyInLowerFile", &StatBuffer), ENOENT);
    LxtCheckResult(chmod(OVFS_TEST_MERGED_DIR "/OnlyInLowerDir", 0111));
    LxtCheckResult(chmod(OVFS_TEST_MERGED_DIR "/OnlyInLowerFile", 0111));

    for (Index = 0; Index < LXT_COUNT_OF(HydratedData); ++Index)
    {
        LxtCheckErrno(stat(HydratedData[Index].Path, &StatBuffer));
        LxtCheckErrno(ListSize = listxattr(HydratedData[Index].Path, List, sizeof(List)));

        ListCurrent = List;
        Count = 0;
        for (;;)
        {
            LxtCheckErrno(ValueSize = getxattr(HydratedData[Index].Path, ListCurrent, Value, sizeof(Value)));

            LxtCheckStringEqual(Value, HydratedData[Index].Name);
            ListCurrent += strlen(ListCurrent) + 1;
            Count += 1;
            if (ListCurrent >= List + ListSize)
            {
                break;
            }
        }

        LxtCheckEqual(Count, 2, "%d");
    }

    //
    // Unmount and check that it was unmounted.
    //

    LxtCheckErrnoZeroSuccess(umount(OVFS_TEST_MOUNT_PATH));
    LxtCheckResult(MountCheckIsNotMount(OVFS_TEST_MOUNT_PATH));

    Result = 0;

ErrorExit:
    umount(OVFS_TEST_MOUNT_PATH);
    return Result;
}

int OvFsTestLowerWhiteout(PLXT_ARGS Args)

/*++

Description:

    This routine tests how whiteouts behavior when they are in the lower layer.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    char Buffer[256];
    int BytesRead;
    int Count;
    struct dirent* Entry;
    int EntryPosition;
    int Fd;
    int Found[LXT_COUNT_OF(g_OvFsMergedMultiContents) + 2];
    int FoundIndex;
    int Index;
    void* Mapping;
    void* MapResult;
    int Result;
    struct stat StatBuffer;
    struct stat StatMergedBuffer;

    Fd = -1;
    Mapping = NULL;

    //
    // Set up the directories and populate some state.
    //

    LxtCheckResult(OvFsTestDirsSetup());
    LxtCheckResult(OvFsTestDirsPopulate());

    //
    // Create some whiteout files in the lower directory.
    //

    LxtCheckResult(mknod(OVFS_TEST_LOWER_DIR "/OnlyInLowerDir/whiteoutFile", S_IFCHR | 0777, 0));
    LxtCheckResult(mknod(OVFS_TEST_LOWER_DIR "/InBothDir/whiteoutFile", S_IFCHR | 0777, 0));

    //
    // Mount an overlayfs instance and check the behavior for the whiteouts
    // created above.
    //

    LxtCheckResult(MountCheckIsNotMount(OVFS_TEST_MOUNT_PATH));
    LxtCheckErrnoZeroSuccess(mount("myovfsnew", OVFS_TEST_MOUNT_PATH, OVFS_TEST_MOUNT_NAME, 0, OVFS_TEST_MOUNT_DEFAULT));

    //
    // Check that the whiteout cannot be opened but are reported through readdir
    // when the folder is not merged with the upper.
    //

    LxtCheckErrno(stat(OVFS_TEST_LOWER_DIR "/OnlyInLowerDir/whiteoutFile", &StatBuffer));
    LxtCheckTrue(S_ISCHR(StatBuffer.st_mode));
    LxtCheckErrnoFailure(stat(OVFS_TEST_MOUNT_PATH "/OnlyInLowerDir/whiteoutFile", &StatMergedBuffer), ENOENT);

    LxtCheckErrno(Fd = open(OVFS_TEST_MOUNT_PATH "/OnlyInLowerDir", O_RDONLY));
    LxtCheckErrno(BytesRead = LxtGetdents64(Fd, (struct dirent*)Buffer, sizeof(Buffer)))

        Count = 0;
    for (EntryPosition = 0; EntryPosition < BytesRead; EntryPosition += Entry->d_reclen)
    {

        Entry = (struct dirent*)&Buffer[EntryPosition];
        LxtLogInfo("%s", Entry->d_name);
        Count += 1;
    }
    LxtCheckEqual(Count, 3, "%d");
    LxtClose(Fd);
    Fd = -1;

    //
    // Check that the whiteout cannot be opened and is not reported through
    // readdir when the folder is merged with the upper.
    //

    LxtCheckErrno(stat(OVFS_TEST_LOWER_DIR "/InBothDir/whiteoutFile", &StatBuffer));
    LxtCheckTrue(S_ISCHR(StatBuffer.st_mode));
    LxtCheckErrnoFailure(stat(OVFS_TEST_MOUNT_PATH "/InBothDir/whiteoutFile", &StatMergedBuffer), ENOENT);

    LxtCheckErrno(Fd = open(OVFS_TEST_MOUNT_PATH "/InBothDir", O_RDONLY));
    LxtCheckErrno(BytesRead = LxtGetdents64(Fd, (struct dirent*)Buffer, sizeof(Buffer)))

        Count = 0;
    for (EntryPosition = 0; EntryPosition < BytesRead; EntryPosition += Entry->d_reclen)
    {

        Entry = (struct dirent*)&Buffer[EntryPosition];
        LxtLogInfo("%s", Entry->d_name);
        Count += 1;
    }
    LxtCheckEqual(Count, 2, "%d");
    LxtClose(Fd);
    Fd = -1;

    //
    // Check that the whiteout can be overwritten in the upper and that it
    // is not replaced by a whiteout when removed.
    //

    LxtCheckErrno(mkdir(OVFS_TEST_MOUNT_PATH "/InBothDir/whiteoutFile", 0777));
    LxtCheckErrno(stat(OVFS_TEST_MOUNT_PATH "/InBothDir/whiteoutFile", &StatMergedBuffer));
    LxtCheckTrue(S_ISDIR(StatMergedBuffer.st_mode));
    LxtCheckErrno(stat(OVFS_TEST_UPPER_DIR "/InBothDir/whiteoutFile", &StatBuffer));
    LxtCheckTrue(S_ISDIR(StatBuffer.st_mode));
    LxtCheckErrno(rmdir(OVFS_TEST_MOUNT_PATH "/InBothDir/whiteoutFile"));
    LxtCheckErrnoFailure(stat(OVFS_TEST_MOUNT_PATH "/InBothDir/whiteoutFile", &StatMergedBuffer), ENOENT);
    LxtCheckErrnoFailure(stat(OVFS_TEST_UPPER_DIR "/InBothDir/whiteoutFile", &StatBuffer), ENOENT);

    //
    // Repeat the above with multiple lowers.
    //

    LxtCheckErrnoZeroSuccess(umount(OVFS_TEST_MOUNT_PATH));
    LxtCheckResult(MountCheckIsNotMount(OVFS_TEST_MOUNT_PATH));

    LxtCheckResult(MountCheckIsNotMount(OVFS_TEST_MOUNT_PATH));
    LxtCheckErrnoZeroSuccess(mount("myovfsnew", OVFS_TEST_MOUNT_PATH, OVFS_TEST_MOUNT_NAME, 0, OVFS_TEST_MOUNT_MULTI_LOWER));

    //
    // Check that the whiteout cannot be opened but are reported through readdir
    // when the folder is not merged with the upper.
    //

    LxtCheckErrno(stat(OVFS_TEST_LOWER_DIR "/OnlyInLowerDir/whiteoutFile", &StatBuffer));
    LxtCheckTrue(S_ISCHR(StatBuffer.st_mode));
    LxtCheckErrnoFailure(stat(OVFS_TEST_MOUNT_PATH "/OnlyInLowerDir/whiteoutFile", &StatMergedBuffer), ENOENT);

    LxtCheckErrno(Fd = open(OVFS_TEST_MOUNT_PATH "/OnlyInLowerDir", O_RDONLY));
    LxtCheckErrno(BytesRead = LxtGetdents64(Fd, (struct dirent*)Buffer, sizeof(Buffer)))

        Count = 0;
    for (EntryPosition = 0; EntryPosition < BytesRead; EntryPosition += Entry->d_reclen)
    {

        Entry = (struct dirent*)&Buffer[EntryPosition];
        LxtLogInfo("%s", Entry->d_name);
        Count += 1;
    }

    LxtCheckEqual(Count, 3, "%d");
    LxtClose(Fd);
    Fd = -1;

    //
    // Check that the whiteout cannot be opened and is not reported through
    // readdir when the folder is merged with the upper.
    //

    LxtCheckErrno(stat(OVFS_TEST_LOWER_DIR "/InBothDir/whiteoutFile", &StatBuffer));
    LxtCheckTrue(S_ISCHR(StatBuffer.st_mode));
    LxtCheckErrnoFailure(stat(OVFS_TEST_MOUNT_PATH "/InBothDir/whiteoutFile", &StatMergedBuffer), ENOENT);

    LxtCheckErrno(Fd = open(OVFS_TEST_MOUNT_PATH "/InBothDir", O_RDONLY));
    LxtCheckErrno(BytesRead = LxtGetdents64(Fd, (struct dirent*)Buffer, sizeof(Buffer)))

        Count = 0;
    for (EntryPosition = 0; EntryPosition < BytesRead; EntryPosition += Entry->d_reclen)
    {

        Entry = (struct dirent*)&Buffer[EntryPosition];
        LxtLogInfo("%s", Entry->d_name);
        Count += 1;
    }

    LxtCheckEqual(Count, 2, "%d");
    LxtClose(Fd);
    Fd = -1;

    //
    // Check that the whiteout can be overwritten in the upper and that it
    // is not replaced by a whiteout when removed.
    //

    LxtCheckErrno(mkdir(OVFS_TEST_MOUNT_PATH "/InBothDir/whiteoutFile", 0777));
    LxtCheckErrno(stat(OVFS_TEST_MOUNT_PATH "/InBothDir/whiteoutFile", &StatMergedBuffer));
    LxtCheckTrue(S_ISDIR(StatMergedBuffer.st_mode));
    LxtCheckErrno(stat(OVFS_TEST_UPPER_DIR "/InBothDir/whiteoutFile", &StatBuffer));
    LxtCheckTrue(S_ISDIR(StatBuffer.st_mode));
    LxtCheckErrno(rmdir(OVFS_TEST_MOUNT_PATH "/InBothDir/whiteoutFile"));
    LxtCheckErrnoFailure(stat(OVFS_TEST_MOUNT_PATH "/InBothDir/whiteoutFile", &StatMergedBuffer), ENOENT);
    LxtCheckErrnoFailure(stat(OVFS_TEST_UPPER_DIR "/InBothDir/whiteoutFile", &StatBuffer), ENOENT);

    //
    // Unmount and check it was unmounted.
    //

    if (Fd != -1)
    {
        LxtClose(Fd);
        Fd = -1;
    }

    LxtCheckErrnoZeroSuccess(umount(OVFS_TEST_MOUNT_PATH));
    LxtCheckResult(MountCheckIsNotMount(OVFS_TEST_MOUNT_PATH));

    Result = 0;

ErrorExit:
    if (Mapping != MAP_FAILED)
    {
        munmap(Mapping, PAGE_SIZE);
    }

    if (Fd != -1)
    {
        LxtClose(Fd);
    }

    umount(OVFS_TEST_MOUNT_PATH);
    return Result;
}

int OvFsTestMultipleLower(PLXT_ARGS Args)

/*++

Description:

    This routine tests various operations with multiple lower layers.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    char Buffer[100];
    int BytesRead;
    int Count;
    struct dirent* Entry;
    int EntryPosition;
    int Fd;
    int Found[LXT_COUNT_OF(g_OvFsMergedMultiContents) + 2];
    int FoundIndex;
    int Index;
    void* Mapping;
    void* MapResult;
    int Result;
    struct stat StatBuffer;
    struct stat StatMergedBuffer;

    Fd = -1;
    Mapping = NULL;

    //
    // Set up the directories and populate some state.
    //

    LxtCheckResult(OvFsTestDirsSetup());
    LxtCheckResult(OvFsTestDirsPopulate());

    //
    // Mount an overlayfs instance and check inode operations that do not
    // hydrate files.
    //

    LxtCheckResult(MountCheckIsNotMount(OVFS_TEST_MOUNT_PATH));
    LxtCheckErrnoZeroSuccess(mount("myovfsnew", OVFS_TEST_MOUNT_PATH, OVFS_TEST_MOUNT_NAME, 0, OVFS_TEST_MOUNT_MULTI_LOWER));

    //
    // Check the behavior for VFS file object operations that support file
    // descriptors opened for read only.
    //
    // N.B. All other file operations will fail the request and are verified
    //      in the VFS access test.
    //

    //
    // Check the behavior for read directory on the root.
    //

    memset(Found, 0, sizeof(Found));
    LxtCheckErrno(Fd = open(OVFS_TEST_MOUNT_PATH, O_RDONLY));
    LxtCheckErrno(BytesRead = LxtGetdents64(Fd, (struct dirent*)Buffer, sizeof(Buffer)));

    while (BytesRead > 0)
    {
        for (EntryPosition = 0; EntryPosition < BytesRead; EntryPosition += Entry->d_reclen)
        {

            Entry = (struct dirent*)&Buffer[EntryPosition];
            if (strcmp(Entry->d_name, ".") == 0)
            {
                FoundIndex = 0;
            }
            else if (strcmp(Entry->d_name, "..") == 0)
            {
                FoundIndex = 1;
            }
            else
            {
                for (Index = 0; Index < LXT_COUNT_OF(g_OvFsMergedMultiContents); ++Index)
                {
                    if (strcmp(g_OvFsMergedMultiContents[Index].Name, Entry->d_name) == 0)
                    {
                        FoundIndex = Index + 2;
                        break;
                    }
                }

                if (Index == LXT_COUNT_OF(g_OvFsMergedMultiContents))
                {
                    LxtLogError("Unexpected entry %s", Entry->d_name);
                    LxtCheckNotEqual(Index, LXT_COUNT_OF(g_OvFsMergedMultiContents), "%d");
                }
            }

            LxtCheckEqual(Found[FoundIndex], 0, "%d");
            Found[FoundIndex] = 1;
        }

        Entry = (struct dirent*)Buffer;
        LxtCheckErrno(BytesRead = LxtGetdents64(Fd, (struct dirent*)Buffer, sizeof(Buffer)));
    }

    for (Index = 0; Index < LXT_COUNT_OF(Found); ++Index)
    {
        LxtCheckEqual(Found[FoundIndex], 1, "%d");
    }

    LxtClose(Fd);
    Fd = -1;

    //
    // Check the behavior for read directory on sub directories.
    //

    for (Index = 0; Index < LXT_COUNT_OF(g_OvFsMergedMultiContents); ++Index)
    {
        if (S_ISDIR(g_OvFsMergedMultiContents[Index].Mode) == FALSE)
        {
            continue;
        }

        LxtCheckErrno(Fd = open(g_OvFsMergedMultiContents[Index].Path, O_RDONLY));
        LxtCheckErrno(BytesRead = LxtGetdents64(Fd, (struct dirent*)Buffer, sizeof(Buffer)))

            Count = 0;
        for (EntryPosition = 0; EntryPosition < BytesRead; EntryPosition += Entry->d_reclen)
        {

            Entry = (struct dirent*)&Buffer[EntryPosition];
            Count += 1;
        }

        LxtCheckEqual(Count, 2, "%d");
        LxtClose(Fd);
        Fd = -1;
    }

    //
    // Check the behavior for read file.
    //

    for (Index = 0; Index < LXT_COUNT_OF(g_OvFsMergedMultiContents); ++Index)
    {
        if (S_ISREG(g_OvFsMergedMultiContents[Index].Mode) == FALSE)
        {
            continue;
        }

        LxtCheckErrno(Fd = open(g_OvFsMergedMultiContents[Index].Path, O_RDONLY));
        LxtCheckErrno(BytesRead = read(Fd, Buffer, sizeof(Buffer) - 1));
        Buffer[BytesRead] = 0;
        LxtCheckStringEqual(Buffer, g_OvFsMergedMultiContents[Index].Name);
        LxtClose(Fd);
        Fd = -1;
    }

    //
    // Check the behavior for seek.
    //

    for (Index = 0; Index < LXT_COUNT_OF(g_OvFsMergedMultiContents); ++Index)
    {
        if ((S_ISDIR(g_OvFsMergedMultiContents[Index].Mode) == FALSE) && (S_ISREG(g_OvFsMergedMultiContents[Index].Mode) == FALSE))
        {

            continue;
        }

        LxtLogInfo("%s", g_OvFsMergedMultiContents[Index].Path);
        LxtCheckErrno(Fd = open(g_OvFsMergedMultiContents[Index].Path, O_RDONLY));
        LxtCheckErrno(lseek(Fd, SEEK_SET, 1));
        LxtClose(Fd);
        Fd = -1;
    }

    //
    // Check that none of the operations hydrated files from the lower
    // directory.
    //

    LxtCheckErrnoFailure(stat(OVFS_TEST_UPPER_DIR "/OnlyInLowerDir", &StatBuffer), ENOENT);
    LxtCheckErrnoFailure(stat(OVFS_TEST_UPPER_DIR "/OnlyInLowerFile", &StatBuffer), ENOENT);
    LxtCheckErrnoFailure(stat(OVFS_TEST_UPPER_DIR "/OnlyInLowerSym", &StatBuffer), ENOENT);

    //
    // Check the search order for the multiple lower layers.
    //

    LxtCheckErrno(stat(OVFS_TEST_MOUNT_PATH "/OnlyInLower2File", &StatMergedBuffer));
    LxtCheckEqual(S_IFREG | 0444, StatMergedBuffer.st_mode, "%d");
    LxtCheckErrno(stat(OVFS_TEST_MOUNT_PATH "/OnlyInLower23File", &StatMergedBuffer));
    LxtCheckEqual(S_IFREG | 0444, StatMergedBuffer.st_mode, "%d");
    LxtCheckErrno(stat(OVFS_TEST_MOUNT_PATH "/OnlyInLower3File", &StatMergedBuffer));
    LxtCheckEqual(S_IFREG | 0111, StatMergedBuffer.st_mode, "%d");

    //
    // Hydrate some files from the lower layers.
    //

    LxtCheckErrno(stat(OVFS_TEST_MOUNT_PATH "/OnlyInLower2File", &StatMergedBuffer));
    LxtCheckErrno(stat(OVFS_TEST_LOWER2_DIR "/OnlyInLower2File", &StatBuffer));
    if (g_LxtUnstableInodes != 0)
    {
        LxtCheckEqual(StatMergedBuffer.st_ino, StatBuffer.st_ino, "%d");
    }

    LxtCheckErrnoFailure(stat(OVFS_TEST_UPPER_DIR "/OnlyInLower2File", &StatBuffer), ENOENT);
    LxtCheckErrno(chmod(OVFS_TEST_MOUNT_PATH "/OnlyInLower2File", 0777));
    LxtCheckErrno(stat(OVFS_TEST_MOUNT_PATH "/OnlyInLower2File", &StatMergedBuffer));
    LxtCheckErrno(stat(OVFS_TEST_UPPER_DIR "/OnlyInLower2File", &StatBuffer));
    if (g_LxtUnstableInodes != 0)
    {
        LxtCheckEqual(StatMergedBuffer.st_ino, StatBuffer.st_ino, "%d");
    }

    LxtCheckErrno(stat(OVFS_TEST_MOUNT_PATH "/OnlyInLower3Dir", &StatMergedBuffer));
    LxtCheckErrno(stat(OVFS_TEST_LOWER3_DIR "/OnlyInLower3Dir", &StatBuffer));
    if (g_LxtUnstableInodes != 0)
    {
        LxtCheckNotEqual(StatMergedBuffer.st_ino, StatBuffer.st_ino, "%d");
    }

    LxtCheckErrnoFailure(stat(OVFS_TEST_UPPER_DIR "/OnlyInLower3Dir", &StatBuffer), ENOENT);
    LxtCheckErrno(chown(OVFS_TEST_MOUNT_PATH "/OnlyInLower3Dir", 2001, 2001));
    LxtCheckErrno(stat(OVFS_TEST_MOUNT_PATH "/OnlyInLower3Dir", &StatMergedBuffer));
    LxtCheckErrno(stat(OVFS_TEST_UPPER_DIR "/OnlyInLower3Dir", &StatBuffer));
    if (g_LxtUnstableInodes != 0)
    {
        LxtCheckNotEqual(StatMergedBuffer.st_ino, StatBuffer.st_ino, "%d");
    }

    LxtCheckErrno(stat(OVFS_TEST_MOUNT_PATH "/OnlyInLower23File", &StatMergedBuffer));
    LxtCheckErrno(stat(OVFS_TEST_LOWER2_DIR "/OnlyInLower23File", &StatBuffer));
    if (g_LxtUnstableInodes != 0)
    {
        LxtCheckEqual(StatMergedBuffer.st_ino, StatBuffer.st_ino, "%d");
    }

    LxtCheckErrnoFailure(stat(OVFS_TEST_UPPER_DIR "/OnlyInLower23File", &StatBuffer), ENOENT);
    LxtCheckErrno(truncate(OVFS_TEST_MOUNT_PATH "/OnlyInLower23File", 0));
    LxtCheckErrno(stat(OVFS_TEST_MOUNT_PATH "/OnlyInLower23File", &StatMergedBuffer));
    LxtCheckErrno(stat(OVFS_TEST_UPPER_DIR "/OnlyInLower23File", &StatBuffer));
    if (g_LxtUnstableInodes != 0)
    {
        LxtCheckEqual(StatMergedBuffer.st_ino, StatBuffer.st_ino, "%d");
    }

    //
    // Unmount and check it was unmounted.
    //

    if (Fd != -1)
    {
        LxtClose(Fd);
        Fd = -1;
    }

    LxtCheckErrnoZeroSuccess(umount(OVFS_TEST_MOUNT_PATH));
    LxtCheckResult(MountCheckIsNotMount(OVFS_TEST_MOUNT_PATH));

    //
    // Check the behavior for entries with the same name but differing types in
    // the lower.
    //

    LxtCheckResult(OvFsTestDirsSetup());
    LxtCheckResult(OvFsTestDirsPopulate());
    LxtCheckErrno(mkdir(OVFS_TEST_LOWER_DIR "/mixedType", 0777));
    LxtCheckErrno(Fd = creat(OVFS_TEST_LOWER_DIR "/mixedType/onlyInLower1", 0777));
    LxtClose(Fd);
    Fd = -1;
    LxtCheckErrno(Fd = creat(OVFS_TEST_LOWER2_DIR "/mixedType", 0777));
    LxtClose(Fd);
    Fd = -1;
    LxtCheckErrno(mkdir(OVFS_TEST_LOWER3_DIR "/mixedType", 0777));
    LxtCheckErrno(Fd = creat(OVFS_TEST_LOWER3_DIR "/mixedType/onlyInLower3", 0777));
    LxtClose(Fd);
    Fd = -1;

    //
    // Mount an overlayfs instance and check for the expected lookup and readdir
    // behavior.
    //

    LxtCheckResult(MountCheckIsNotMount(OVFS_TEST_MOUNT_PATH));
    LxtCheckErrnoZeroSuccess(mount("myovfsnew", OVFS_TEST_MOUNT_PATH, OVFS_TEST_MOUNT_NAME, 0, OVFS_TEST_MOUNT_MULTI_LOWER));

    LxtCheckErrno(stat(OVFS_TEST_MOUNT_PATH "/mixedType", &StatMergedBuffer));
    LxtCheckTrue(S_ISDIR(StatMergedBuffer.st_mode));
    LxtCheckErrno(stat(OVFS_TEST_MOUNT_PATH "/mixedType/onlyInLower1", &StatMergedBuffer));
    LxtCheckTrue(S_ISREG(StatMergedBuffer.st_mode));
    LxtCheckErrnoFailure(stat(OVFS_TEST_MOUNT_PATH "/mixedType/onlyInLower3", &StatMergedBuffer), ENOENT);

    Count = 0;
    LxtCheckErrno(Fd = open(OVFS_TEST_MOUNT_PATH "/mixedType", O_RDONLY));
    LxtCheckErrno(BytesRead = LxtGetdents64(Fd, (struct dirent*)Buffer, sizeof(Buffer)));

    while (BytesRead > 0)
    {
        for (EntryPosition = 0; EntryPosition < BytesRead; EntryPosition += Entry->d_reclen)
        {

            Entry = (struct dirent*)&Buffer[EntryPosition];
            LxtLogInfo("%s", Entry->d_name);
            Count += 1;
        }

        LxtCheckErrno(BytesRead = LxtGetdents64(Fd, (struct dirent*)Buffer, sizeof(Buffer)));
    }

    LxtCheckEqual(Count, 3, "%d");
    LxtClose(Fd);
    Fd = -1;

    //
    // Repeat the above with the mismatch in the lowest layer.
    //

    if (Fd != -1)
    {
        LxtClose(Fd);
        Fd = -1;
    }

    LxtCheckErrnoZeroSuccess(umount(OVFS_TEST_MOUNT_PATH));
    LxtCheckResult(MountCheckIsNotMount(OVFS_TEST_MOUNT_PATH));

    //
    // Check the behavior for entries with the same name but differing types in
    // the lower.
    //

    LxtCheckResult(OvFsTestDirsSetup());
    LxtCheckResult(OvFsTestDirsPopulate());
    LxtCheckErrno(mkdir(OVFS_TEST_LOWER_DIR "/mixedType", 0777));
    LxtCheckErrno(Fd = creat(OVFS_TEST_LOWER_DIR "/mixedType/onlyInLower1", 0777));
    LxtClose(Fd);
    Fd = -1;
    LxtCheckErrno(mkdir(OVFS_TEST_LOWER2_DIR "/mixedType", 0777));
    LxtCheckErrno(Fd = creat(OVFS_TEST_LOWER2_DIR "/mixedType/onlyInLower2", 0777));
    LxtClose(Fd);
    Fd = -1;
    LxtCheckErrno(Fd = creat(OVFS_TEST_LOWER3_DIR "/mixedType", 0777));
    LxtClose(Fd);
    Fd = -1;

    //
    // Mount an overlayfs instance and check for the expected lookup and readdir
    // behavior.
    //

    LxtCheckResult(MountCheckIsNotMount(OVFS_TEST_MOUNT_PATH));
    LxtCheckErrnoZeroSuccess(mount("myovfsnew", OVFS_TEST_MOUNT_PATH, OVFS_TEST_MOUNT_NAME, 0, OVFS_TEST_MOUNT_MULTI_LOWER));

    LxtCheckErrno(stat(OVFS_TEST_MOUNT_PATH "/mixedType", &StatMergedBuffer));
    LxtCheckTrue(S_ISDIR(StatMergedBuffer.st_mode));
    LxtCheckErrno(stat(OVFS_TEST_MOUNT_PATH "/mixedType/onlyInLower1", &StatMergedBuffer));
    LxtCheckTrue(S_ISREG(StatMergedBuffer.st_mode));
    LxtCheckErrno(stat(OVFS_TEST_MOUNT_PATH "/mixedType/onlyInLower2", &StatMergedBuffer));
    LxtCheckTrue(S_ISREG(StatMergedBuffer.st_mode));

    Count = 0;
    LxtCheckErrno(Fd = open(OVFS_TEST_MOUNT_PATH "/mixedType", O_RDONLY));
    LxtCheckErrno(BytesRead = LxtGetdents64(Fd, (struct dirent*)Buffer, sizeof(Buffer)));

    while (BytesRead > 0)
    {
        for (EntryPosition = 0; EntryPosition < BytesRead; EntryPosition += Entry->d_reclen)
        {

            Entry = (struct dirent*)&Buffer[EntryPosition];
            LxtLogInfo("%s", Entry->d_name);
            Count += 1;
        }

        LxtCheckErrno(BytesRead = LxtGetdents64(Fd, (struct dirent*)Buffer, sizeof(Buffer)));
    }

    LxtCheckEqual(Count, 4, "%d");
    LxtClose(Fd);
    Fd = -1;

    //
    // Unmount and check it was unmounted.
    //

    if (Fd != -1)
    {
        LxtClose(Fd);
        Fd = -1;
    }

    LxtCheckErrnoZeroSuccess(umount(OVFS_TEST_MOUNT_PATH));
    LxtCheckResult(MountCheckIsNotMount(OVFS_TEST_MOUNT_PATH));

    Result = 0;

ErrorExit:
    if (Mapping != MAP_FAILED)
    {
        munmap(Mapping, PAGE_SIZE);
    }

    if (Fd != -1)
    {
        LxtClose(Fd);
    }

    umount(OVFS_TEST_MOUNT_PATH);
    return Result;
}
