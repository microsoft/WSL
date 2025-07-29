/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    lxtfs.c

Abstract:

    This file contains common test functions for file system tests.

--*/

#include "lxtcommon.h"
#include "lxtfs.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/epoll.h>
#include <sys/inotify.h>
#include <sys/mount.h>
#include <sys/xattr.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <stdio.h>
#include <limits.h>
#include <dirent.h>
#include <stdlib.h>
#include <libmount/libmount.h>
#include "lxtmount.h"

#define FS_RENAMEAT_TEST_FILE "file"
#define FS_RENAMEAT_TEST_FILE2 "file2"
#define FS_UTIME_TESTFILE "testfile"
#define FS_UTIME_TESTLINK "testlink"
#define FS_NS_PER_SEC (1000000000ull)
#define FS_NS_PER_NT_UNIT (100ull)
#define FS_UNIX_TIME_2000 (946684800)
#define FS_SECONDS_PER_DAY (86400)
#define FS_FAT_MODIFIED_TIME_PRECISION (2)
#define FS_CURRENT_TIME_ALLOWED_VARIANCE (2)

//
// The following macros are used by LxtFsDeleteCurrentWorkingDirectoryCommon
// and LxtFsDeleteOpenFileCommon.
//

#define FS_DELETE_TEST_DIR_NAME "delete_test"
#define FS_DELETE_TEST_DIR FS_TEST_DIR_PARENT "/" FS_DELETE_TEST_DIR_NAME
#define FS_DELETE_TEST_RENAME_FILE_NAME "/delete_test_file"
#define FS_DELETE_TEST_RENAME_FILE FS_TEST_DIR_PARENT FS_DELETE_TEST_RENAME_FILE_NAME
#define FS_DELETE_TEST_DIR_AT "../" FS_DELETE_TEST_DIR_NAME
#define FS_DELETE_TEST_CHILD "child"
#define FS_DELETE_LINK_SUFFIX " (deleted)"
#define FS_CHILD_PATH_FORMAT "%s/%s"
#define FS_FD_PATH_FORMAT "/proc/self/fd/%d"
#define FS_PROC_SELF_CWD "/proc/self/cwd"

//
// The following macros are used by LxtFsRenameDirCommon.
//

#define FS_RENAME_TEST_DIR "/rename_test"
#define FS_RENAME_TEST_DIR2 "/rename_test2"
#define FS_RENAME_TEST_DIR3 "/rename_test3"
#define FS_RENAME_TEST_FILE "/rename_test_file"
#define FS_RENAME_TEST_DIR_CHILD FS_RENAME_TEST_DIR "/child"
#define FS_RENAME_TEST_DIR_CHILD2 FS_RENAME_TEST_DIR "/child2"
#define FS_RENAME_TEST_DIR2_CHILD FS_RENAME_TEST_DIR2 "/child"
#define FS_RENAME_TEST_DIR2_CHILD2 FS_RENAME_TEST_DIR2 "/child2"
#define FS_RENAME_TEST_DIR_GRANDCHILD FS_RENAME_TEST_DIR_CHILD "/child2"
#define FS_RENAME_TEST_DIR_SLASH "/rename_test_slash/"
#define FS_RENAME_TEST_DIR_SLASH2 "/rename_test_slash2"
#define FS_RENAME_TEST_DIR_SLASH_LINK "/rename_test_slash_link"
#define FS_RENAME_TEST_DIR_SLASH_LINK2 "/rename_test_slash_link2"

//
// Flags for LxtFsTimestampCheckUpdate
//

#define FS_TIMESTAMP_ACCESS (0x1)
#define FS_TIMESTAMP_MODIFY (0x2)
#define FS_TIMESTAMP_CHANGE (0x4)

#define FS_TIMESTAMP_SLEEP_TIME (100000)

#define FS_MOUNT_DRVFS_COMMAND_FORMAT "mount -t drvfs %s %s -o rw,noatime"
#define FS_MOUNT_DRVFS_OPTIONS_COMMAND_FORMAT FS_MOUNT_DRVFS_COMMAND_FORMAT ",%s"
#define FS_PLAN9_UNC_PREFIX "UNC\\"
#define FS_PLAN9_UNC_PREFIX_LENGTH (sizeof(FS_PLAN9_UNC_PREFIX) - 1)
#define FS_UNC_PATH_PREFIX_LENGTH (2)

typedef struct _BASIC_TEST_CASE
{
    struct timespec SetTime[2];
    struct timespec ExpectTime[2];
} BASIC_TEST_CASE, *PBASIC_TEST_CASE;

enum
{
    NameVariationFullName,
    NameVariationCwdRelative,
    NameVariationRelative,
    NameVariationDescriptor,
    NameVariationFullFileViaLink,
    NameVariationCwdRelativeViaLink,
    NameVariationRelativeViaLink,
    NameVariationDescriptorViaLink,
    NameVariationFullFileOnLink,
    NameVariationCwdRelativeOnLink,
    NameVariationRelativeOnLink,
    NameVariationMax = NameVariationRelativeOnLink,
    NameVariationFatMax = NameVariationDescriptor
};

struct linux_dirent
{
    unsigned long d_ino;
    unsigned long d_off;
    unsigned short d_reclen;
    char d_name[];
    // char           pad;
    // char           d_type;
};

int LxtFsDeleteCurrentWorkingDirectoryHelper(char* BaseDir, char* DeleteTestDir, int Flags);

int LxtFsDeleteOpenFileHelper(int Fd, char* BaseDir, char* DeleteTestDir, int Flags);

int LxtFsTimestampCheckCurrent(const struct timespec* Timestamp);

int LxtFsTimestampCheckEqual(const struct timespec* Timestamp1, const struct timespec* Timestamp2);

int LxtFsTimestampCheckGreater(const struct timespec* Timestamp1, const struct timespec* Timestamp2);

int LxtFsTimestampCheckUpdate(const char* Path, struct stat* PreviousStat, int Flags);

long long LxtFsTimestampDiff(const struct timespec* Timestamp1, const struct timespec* Timestamp2);

bool LxtFsUtimeDoTimesMatch(const struct timespec* Actual, const struct timespec* Expected, int AllowedVarianceSeconds);

void LxtFsUtimeRoundToFatAccessTime(struct timespec* Timespec);

void LxtFsUtimeRoundToFatModifiedTime(struct timespec* Timespec);

void LxtFsUtimeRoundToNt(struct timespec* Timespec);

//
// All real timestamps are offset from the year 2000 because FAT can only
// accept timestamps afer 1980.

BASIC_TEST_CASE BasicTestCases[] = {
    {{{FS_UNIX_TIME_2000 + 1111111, 2222222}, {FS_UNIX_TIME_2000 + 3333333, 4444444}},
     {{FS_UNIX_TIME_2000 + 1111111, 2222222}, {FS_UNIX_TIME_2000 + 3333333, 4444444}}},

    {{{5555555, UTIME_OMIT}, {FS_UNIX_TIME_2000 + 6666666, 7777777}},
     {{FS_UNIX_TIME_2000 + 1111111, 2222222}, {FS_UNIX_TIME_2000 + 6666666, 7777777}}},

    {{{FS_UNIX_TIME_2000 + 5555555, 8888888}, {9999999, UTIME_OMIT}},
     {{FS_UNIX_TIME_2000 + 5555555, 8888888}, {FS_UNIX_TIME_2000 + 6666666, 7777777}}},

    {{{1111111, UTIME_NOW}, {FS_UNIX_TIME_2000 + 2222222, 3333333}}, {{5555555, UTIME_NOW}, {FS_UNIX_TIME_2000 + 2222222, 3333333}}},

    {{{FS_UNIX_TIME_2000 + 1111111, 22222222}, {3333333, UTIME_NOW}}, {{FS_UNIX_TIME_2000 + 1111111, 22222222}, {4444444, UTIME_NOW}}},

    {{{1111111, UTIME_NOW}, {3333333, UTIME_NOW}}, {{2222222, UTIME_NOW}, {4444444, UTIME_NOW}}},

    {{{0, UTIME_NOW}, {3333333, UTIME_NOW}}, {{0, UTIME_NOW}, {4444444, UTIME_NOW}}},

    //
    // This time is at 1am UTC, which is likely to be in the previous day local
    // time (if the test is run on a system in the US). Having this value here
    // ensures the test handles that correctly for FAT timestamp rounding in
    // case it occurs for the current time.
    //

    {{{1498440508, 22222222}, {3333333, UTIME_NOW}}, {{1498440508, 22222222}, {4444444, UTIME_NOW}}},
};

LXT_FS_INFO g_LxtFsInfo;

int LxtFsCheckDrvFsMount(const char* Source, const char* Target, const char* Options, int ParentId, const char* MountRoot)

/*++

Description:

    This routine verifies the mount options for a drvfs mount.

    N.B. On WSL 2 this uses 9p mount options.

Arguments:

    Source - Supplies the mount source.

    Target - Supplies the mount target.

    Options - Supplies optional mount options.

    ParentId - Supplies the expected parent ID of the mount.

    MountRoot - Supplies the expected mount root.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    char ExpectedOptions[1024];
    char ExpectedCombinedOptions[1024];
    LXT_FS_INFO FsInfo;
    size_t Index;
    size_t Length;
    char Plan9Options[1024];
    const char* Plan9Source;
    int Result;
    char Temp[1024];
    char* UncSource = NULL;

    LxtCheckResult(LxtFsGetFsInfo(Target, &FsInfo));
    if (FsInfo.FsType == LxtFsTypeDrvFs)
    {
        if (Options == NULL)
        {
            Options = "case=off";
        }

        snprintf(ExpectedOptions, sizeof(ExpectedOptions), "rw,%s", Options);
        snprintf(ExpectedCombinedOptions, sizeof(ExpectedOptions), "rw,noatime,%s", Options);

        LxtCheckResult(MountCheckIsMount(Target, ParentId, Source, "drvfs", MountRoot, "rw,noatime", ExpectedOptions, ExpectedCombinedOptions, 0));
    }
    else if (FsInfo.FsType == LxtFsTypePlan9)
    {
        if (Options == NULL)
        {
            Temp[0] = '\0';
        }
        else
        {
            Temp[0] = ';';
            strncpy(Temp + 1, Options, sizeof(Temp) - 1);
            for (Index = 0; Index < strlen(Temp); Index += 1)
            {
                if (Temp[Index] == ',')
                {
                    Temp[Index] = ';';
                }
            }
        }

        Length = strlen(Source);
        if ((Length >= FS_UNC_PATH_PREFIX_LENGTH) && ((Source[0] == '/') || (Source[0] == '\\')) &&
            ((Source[1] == '/') || (Source[1] == '\\')))
        {

            Length -= FS_UNC_PATH_PREFIX_LENGTH;
            UncSource = malloc(Length + FS_PLAN9_UNC_PREFIX_LENGTH + 1);
            if (UncSource == NULL)
            {
                LxtLogError("malloc");
                Result = -1;
                goto ErrorExit;
            }

            memcpy(UncSource, FS_PLAN9_UNC_PREFIX, FS_PLAN9_UNC_PREFIX_LENGTH);
            memcpy(&UncSource[FS_PLAN9_UNC_PREFIX_LENGTH], &Source[FS_UNC_PATH_PREFIX_LENGTH], Length);

            UncSource[Length + FS_PLAN9_UNC_PREFIX_LENGTH] = '\0';
            Plan9Source = UncSource;
        }
        else
        {
            Plan9Source = Source;
        }

        if (FsInfo.Flags.VirtIo != 0)
        {
            Source = "drvfsa";
            snprintf(
                Plan9Options,
                sizeof(Plan9Options),
                "aname=drvfs;path=%s%s;symlinkroot=/mnt/,cache=5,access=client,msize=262144,trans=virtio",
                Plan9Source,
                Temp);
        }
        else
        {
            snprintf(
                Plan9Options,
                sizeof(Plan9Options),
                "aname=drvfs;path=%s%s;symlinkroot=/mnt/,cache=5,access=client,msize=65536,trans=fd,rfd=4,wfd=4",
                Plan9Source,
                Temp);
        }

        //
        // The combined options aren't checked for 9p because the placement of
        // noatime by libmount is inconsistent.
        //

        snprintf(ExpectedOptions, sizeof(ExpectedOptions), "rw,%s", Plan9Options);
        LxtCheckResult(MountCheckIsMount(Target, ParentId, Source, "9p", MountRoot, "rw,noatime", ExpectedOptions, NULL, 0));
    }
    else if (FsInfo.FsType == LxtFsTypeVirtioFs)
    {
        snprintf(ExpectedOptions, sizeof(ExpectedOptions), "rw");
        LxtCheckResult(MountCheckIsMount(Target, ParentId, NULL, "virtiofs", MountRoot, "rw,noatime", ExpectedOptions, NULL, 0));
    }

ErrorExit:
    free(UncSource);
    return Result;
}

int LxtFsCreateTestDir(char* Directory)

/*++

Description:

    This routine creates a test directory, succeeding if it already exists.

Arguments:

    Directory - Supplies the directory name.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int Result;

    Result = mkdir(Directory, 0777);
    if ((Result < 0) && (errno != EEXIST))
    {
        LxtLogError("Failed to create directory %s", Directory);
        goto ErrorExit;
    }

    Result = 0;

ErrorExit:
    return Result;
}

int LxtFsDeleteCurrentWorkingDirectoryCommon(char* BaseDir, int Flags)

/*++

Description:

    This routine tests the behavior if the current working directory is
    unlinked.

Arguments:

    BaseDir - Supplies the top directory to use for the test.

    Flags - Supplies various flags.

Return Value:

    Returns 0 on success, -1 on failure.

    --*/

{

    char DeleteTestDir[PATH_MAX];
    char DeleteTestRenameFile[PATH_MAX];
    int Fd;
    char Path[PATH_MAX];
    char* PointerResult;
    int Result;

    sprintf(DeleteTestDir, "%s/%s", BaseDir, FS_DELETE_TEST_DIR_NAME);
    sprintf(DeleteTestRenameFile, "%s/%s", BaseDir, FS_DELETE_TEST_RENAME_FILE_NAME);

    //
    // Create the directory, change to it, and do a sanity check on the normal
    // return values of these functions.
    //

    Fd = -1;
    memset(Path, 0, sizeof(Path));
    LxtCheckErrno(Fd = creat(DeleteTestRenameFile, 0666));
    LxtCheckErrno(close(Fd));
    LxtCheckErrnoZeroSuccess(mkdir(DeleteTestDir, 0777));
    LxtCheckErrnoZeroSuccess(chdir(DeleteTestDir));
    LxtCheckErrno(LxtGetcwd(Path, sizeof(Path)));
    LxtCheckStringEqual(Path, DeleteTestDir);
    memset(Path, 0, sizeof(Path));
    LxtCheckErrno(readlink(FS_PROC_SELF_CWD, Path, sizeof(Path)));
    LxtCheckStringEqual(Path, DeleteTestDir);

    //
    // Unlinking the directory with "." should fail.
    //

    LxtCheckErrnoFailure(rmdir("."), EINVAL);

    //
    // Unlink the directory.
    //

    LxtCheckErrnoZeroSuccess(rmdir(DeleteTestDir));

    //
    // Check the behavior.
    //

    LxtCheckResult(LxtFsDeleteCurrentWorkingDirectoryHelper(BaseDir, DeleteTestDir, Flags));

    //
    // Nothing should change if a new directory is created with the same name.
    //

    LxtCheckErrnoZeroSuccess(mkdir(DeleteTestDir, 0777));
    LxtCheckResult(LxtFsDeleteCurrentWorkingDirectoryHelper(BaseDir, DeleteTestDir, Flags));
    rmdir(DeleteTestDir);

    //
    // Opening the deleted directory should succeed.
    //
    // N.B. This currently doesn't work on Plan 9 or virtiofs.
    //

    if (g_LxtFsInfo.FsType != LxtFsTypePlan9 && g_LxtFsInfo.FsType != LxtFsTypeVirtioFs)
    {
        LxtCheckErrno(Fd = open(".", O_DIRECTORY | O_RDONLY));
        LxtCheckResult(LxtFsDeleteOpenFileHelper(Fd, BaseDir, DeleteTestDir, Flags));
    }

    //
    // Try to chdir to the parent.
    //

    LxtCheckErrnoZeroSuccess(chdir(".."));
    LxtCheckErrno(LxtGetcwd(Path, sizeof(Path)));
    LxtCheckStringEqual(Path, BaseDir);
    memset(Path, 0, sizeof(Path));
    LxtCheckErrno(readlink(FS_PROC_SELF_CWD, Path, sizeof(Path)));
    LxtCheckStringEqual(Path, BaseDir);

    //
    // Try to chdir back to the deleted directory.
    //
    // N.B. This currently does not work on Plan 9 or virtiofs.
    //

    if (g_LxtFsInfo.FsType != LxtFsTypePlan9 && g_LxtFsInfo.FsType != LxtFsTypeVirtioFs)
    {
        LxtCheckErrnoZeroSuccess(fchdir(Fd));
        LxtCheckResult(LxtFsDeleteCurrentWorkingDirectoryHelper(BaseDir, DeleteTestDir, Flags));
    }

ErrorExit:
    if (Fd >= 0)
    {
        close(Fd);
    }

    rmdir(DeleteTestDir);
    unlink(DeleteTestRenameFile);

    return Result;
}

int LxtFsDeleteCurrentWorkingDirectoryHelper(char* BaseDir, char* DeleteTestDir, int Flags)

/*++

Description:

    This routine checks if a deleted working directory behaves as expected.

Arguments:

    BaseDir - Supplies the top directory to use for the test.

    DeleteTestDir - Supplies the path to the delete test root directory.

    Flags - Supplies various flags.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    char DeleteTestDirDeleteSuffix[PATH_MAX];
    char DeleteTestRenameFile[PATH_MAX];
    char Path[PATH_MAX];
    int ParentFd;
    char* PointerResult;
    int Result;

    sprintf(DeleteTestDirDeleteSuffix, "%s%s", DeleteTestDir, FS_DELETE_LINK_SUFFIX);
    sprintf(DeleteTestRenameFile, "%s/%s", BaseDir, FS_DELETE_TEST_RENAME_FILE_NAME);
    ParentFd = -1;

    //
    // Check the result of getcwd and /proc/self/cwd
    //

    memset(Path, 0, sizeof(Path));
    LxtCheckErrnoFailure(LxtGetcwd(Path, sizeof(Path)), ENOENT);
    memset(Path, 0, sizeof(Path));
    LxtCheckErrno(readlink(FS_PROC_SELF_CWD, Path, sizeof(Path)));
    LxtCheckStringEqual(Path, DeleteTestDirDeleteSuffix);

    //
    // Creating a new item in the current working directory should fail.
    //

    LxtCheckErrnoFailure(open(FS_DELETE_TEST_CHILD, O_CREAT | O_WRONLY, 0777), ENOENT);

    LxtCheckErrnoFailure(mkdir(FS_DELETE_TEST_CHILD, 0777), ENOENT);
    LxtCheckErrnoFailure(link(BaseDir, FS_DELETE_TEST_CHILD), ENOENT);
    LxtCheckErrnoFailure(symlink("/proc", FS_DELETE_TEST_CHILD), ENOENT);
    LxtCheckErrnoFailure(rename(DeleteTestRenameFile, "./" FS_DELETE_TEST_CHILD), ENOENT);

    if ((Flags & FS_DELETE_DRVFS) == 0)
    {
        LxtCheckErrnoFailure(mknod(FS_DELETE_TEST_CHILD, S_IFIFO | 0777, 0), ENOENT);
    }

    //
    // Opening the parent should succeed.
    //

    LxtCheckErrno(ParentFd = open("..", O_DIRECTORY | O_RDONLY));
    LxtCheckResult(LxtCheckFdPath(ParentFd, BaseDir));

ErrorExit:
    if (ParentFd >= 0)
    {
        close(ParentFd);
    }

    return Result;
}

int LxtFsDeleteOpenFileCommon(char* BaseDir, int Flags)

/*++

Description:

    This routine tests using unlink and rmdir on a file/directory that's open.

Arguments:

    BaseDir - Supplies the top directory to use for the test.

    Flags - Supplies various flags.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int ChildFd;
    char ChildPath[PATH_MAX];
    char ChildPathDeleteSuffix[PATH_MAX];
    char ChildPathSubpath[PATH_MAX];
    char DeleteTestDir[PATH_MAX];
    char DeleteTestDirAt[PATH_MAX];
    char DeleteTestRenameFile[PATH_MAX];
    int Fd;
    char Path[PATH_MAX];
    int ReopenFd;
    int Result;
    struct stat Stat;

    sprintf(DeleteTestDir, "%s/%s", BaseDir, FS_DELETE_TEST_DIR_NAME);
    sprintf(DeleteTestDirAt, "../%s", FS_DELETE_TEST_DIR_NAME);
    sprintf(DeleteTestRenameFile, "%s/%s", BaseDir, FS_DELETE_TEST_RENAME_FILE_NAME);
    sprintf(ChildPath, "%s/%s", DeleteTestDir, FS_DELETE_TEST_CHILD);
    sprintf(ChildPathDeleteSuffix, "%s%s", ChildPath, FS_DELETE_LINK_SUFFIX);
    Fd = -1;
    ChildFd = -1;
    ReopenFd = -1;
    LxtCheckErrno(Fd = creat(DeleteTestRenameFile, 0666));
    LxtCheckErrno(close(Fd));
    LxtCheckErrnoZeroSuccess(mkdir(DeleteTestDir, 0777));
    LxtCheckErrno(Fd = open(DeleteTestDir, O_DIRECTORY | O_RDONLY));

    //
    // It should be possible to create a child in the directory.
    //

    LxtCheckErrno(ChildFd = openat(Fd, FS_DELETE_TEST_CHILD, O_CREAT | O_WRONLY, 0777));

    LxtCheckResult(LxtCheckFdPath(ChildFd, ChildPath));

    //
    // Unlink the file and check the path indicates it's deleted.
    //

    LxtCheckErrnoZeroSuccess(unlinkat(Fd, FS_DELETE_TEST_CHILD, 0));
    LxtCheckResult(LxtCheckFdPath(ChildFd, ChildPathDeleteSuffix));

    //
    // Reopening through the file descriptor should work.
    //
    // N.B Reopening currently doesn't work on Plan 9 or virtiofs.
    //

    if (g_LxtFsInfo.FsType != LxtFsTypePlan9 && g_LxtFsInfo.FsType != LxtFsTypeVirtioFs)
    {
        sprintf(Path, FS_FD_PATH_FORMAT, ChildFd);

        LxtCheckErrno(ReopenFd = open(Path, O_RDONLY));
        LxtCheckResult(LxtCheckFdPath(ReopenFd, ChildPathDeleteSuffix));

        //
        // Check that fstat on the deleted file returns 0 link count.
        //
        // N.B. Plan 9 will return ENOENT instead for all the below.
        //

        LxtCheckErrnoZeroSuccess(fstat(ChildFd, &Stat));
        LxtCheckEqual(Stat.st_nlink, 0, "%d");

        //
        // The target is a file so subpaths don't work.
        //

        sprintf(ChildPathSubpath, FS_CHILD_PATH_FORMAT, Path, ".");
        LxtCheckErrnoFailure(open(ChildPathSubpath, O_RDONLY), ENOTDIR);
        sprintf(ChildPathSubpath, FS_CHILD_PATH_FORMAT, Path, "..");
        LxtCheckErrnoFailure(open(ChildPathSubpath, O_RDONLY), ENOTDIR);
        sprintf(ChildPathSubpath, FS_CHILD_PATH_FORMAT, Path, "foo");
        LxtCheckErrnoFailure(open(ChildPathSubpath, O_RDONLY), ENOTDIR);
        LxtCheckErrnoZeroSuccess(close(ReopenFd));
        ReopenFd = -1;
    }

    LxtCheckErrnoZeroSuccess(close(ChildFd));
    ChildFd = -1;

    //
    // Unlinking the directory through "." should fail.
    //

    LxtCheckErrnoFailure(unlinkat(Fd, ".", AT_REMOVEDIR), EINVAL);

    //
    // Unlink the directory.
    //

    LxtCheckErrnoZeroSuccess(rmdir(DeleteTestDir));

    //
    // Trying to re-open the deleted directory should fail.
    //

    LxtCheckErrnoFailure(ChildFd = openat(Fd, DeleteTestDirAt, O_DIRECTORY | O_RDONLY), ENOENT);

    //
    // Check behavior is correct after deleting.
    //

    LxtCheckResult(LxtFsDeleteOpenFileHelper(Fd, BaseDir, DeleteTestDir, Flags));

    //
    // Even if a directory with the same name is created.
    //

    LxtCheckErrnoZeroSuccess(mkdir(DeleteTestDir, 0777));
    LxtCheckResult(LxtFsDeleteOpenFileHelper(Fd, BaseDir, DeleteTestDir, Flags));

ErrorExit:
    if (ReopenFd >= 0)
    {
        close(ReopenFd);
    }

    if (ChildFd >= 0)
    {
        close(ChildFd);
    }

    if (Fd >= 0)
    {
        close(Fd);
    }

    unlink(DeleteTestRenameFile);
    unlink(ChildPath);
    rmdir(DeleteTestDir);

    return Result;
}

int LxtFsDeleteOpenFileHelper(int Fd, char* BaseDir, char* DeleteTestDir, int Flags)

/*++

Description:

    This routine checks if a file descriptor pointing to a deleted directory
    behaves as expected.

Arguments:

    Fd - Supplies the file descriptor.

    BaseDir - Supplies the top directory to use for the test.

    DeleteTestDir - Supplies the path to the delete test root directory.

    Flags - Supplies various flags.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    char ChildPathSubpath[PATH_MAX];
    char DeleteTestDirDeleteSuffix[PATH_MAX];
    char DeleteTestRenameFile[PATH_MAX];
    int ParentFd;
    char Path[PATH_MAX];
    int ReopenFd;
    int Result;

    sprintf(DeleteTestDirDeleteSuffix, "%s%s", DeleteTestDir, FS_DELETE_LINK_SUFFIX);
    sprintf(DeleteTestRenameFile, "%s/%s", BaseDir, FS_DELETE_TEST_RENAME_FILE_NAME);
    ParentFd = -1;
    ReopenFd = -1;

    //
    // Check the path indicates it's deleted.
    //

    LxtCheckResult(LxtCheckFdPath(Fd, DeleteTestDirDeleteSuffix));

    //
    // Check that creating new items fails as expected.
    //

    LxtCheckErrnoFailure(openat(Fd, FS_DELETE_TEST_CHILD, O_CREAT | O_WRONLY, 0666), ENOENT);

    LxtCheckErrnoFailure(mkdirat(Fd, FS_DELETE_TEST_CHILD, 0777), ENOENT);
    LxtCheckErrnoFailure(linkat(AT_FDCWD, BaseDir, Fd, FS_DELETE_TEST_CHILD, 0), ENOENT);

    LxtCheckErrnoFailure(symlinkat("/proc", Fd, FS_DELETE_TEST_CHILD), ENOENT);
    LxtCheckErrnoFailure(renameat(AT_FDCWD, DeleteTestRenameFile, Fd, FS_DELETE_TEST_CHILD), ENOENT);

    //
    // Drvfs doesn't support mknod.
    //

    if ((Flags & FS_DELETE_DRVFS) == 0)
    {
        LxtCheckErrnoFailure(mknodat(Fd, FS_DELETE_TEST_CHILD, S_IFIFO | 0777, 0), ENOENT);
    }

    //
    // Navigating to the parent from the deleted directory should succeed.
    //

    LxtCheckErrno(ParentFd = openat(Fd, "..", O_DIRECTORY | O_RDONLY));
    LxtCheckResult(LxtCheckFdPath(ParentFd, BaseDir));

    //
    // Reopening through the file descriptor should work.
    //

    sprintf(ChildPathSubpath, FS_CHILD_PATH_FORMAT, Path, "foo");
    LxtCheckErrnoFailure(open(ChildPathSubpath, O_RDONLY), ENOENT);

ErrorExit:
    if (ParentFd >= 0)
    {
        close(ParentFd);
    }

    if (ReopenFd >= 0)
    {
        close(ReopenFd);
    }

    return Result;
}

int LxtFsDeleteLoopCommon(const char* BaseDir)

/*++

Description:

    This routine tests deleting files in a loop with multiple getdents calls.

Arguments:

    BaseDir - Supplies the base directory.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    char Buffer[512];
    int BytesRead;
    int Calls;
    int Count;
    struct dirent64* Entry;
    int Fd;
    const int FileCount = 500;
    int Index;
    char Path[PATH_MAX];
    int Result;

    Fd = -1;

    //
    // Create the directory and the test files.
    //

    LxtCheckErrnoZeroSuccess(mkdir(BaseDir, 0777));
    for (Index = 0; Index < FileCount; Index += 1)
    {
        snprintf(Path, sizeof(Path), "%s/file%d", BaseDir, Index);
        LxtCheckErrno(Fd = creat(Path, 0666));
        LxtCheckClose(Fd);
    }

    //
    // List the directory, and delete the files in between calls.
    //

    Calls = 0;
    Count = 0;
    LxtCheckErrno(Fd = open(BaseDir, O_RDONLY | O_DIRECTORY));
    LxtCheckErrno(BytesRead = LxtGetdents64(Fd, Buffer, sizeof(Buffer)));

    while (BytesRead != 0)
    {
        Calls += 1;
        Index = 0;
        while (Index < BytesRead)
        {
            Entry = (struct dirent64*)&Buffer[Index];
            if ((strcmp(Entry->d_name, ".") != 0) && (strcmp(Entry->d_name, "..") != 0))
            {

                snprintf(Path, sizeof(Path), "%s/%s", BaseDir, Entry->d_name);
                LxtCheckErrnoZeroSuccess(unlink(Path));
                Count += 1;
            }

            Index += Entry->d_reclen;
        }

        LxtCheckErrno(BytesRead = LxtGetdents64(Fd, Buffer, sizeof(Buffer)));
    }

    //
    // Make sure all files were deleted, and that more than one getdents call
    // was used (otherwise the test is meaningless).
    //

    LxtCheckEqual(Count, FileCount, "%d");
    LxtCheckGreater(Calls, 1, "%d");
    LxtLogInfo("Calls: %d", Calls);
    LxtCheckClose(Fd);

    //
    // The directory is now empty so it can be removed.
    //

    LxtCheckErrnoZeroSuccess(rmdir(BaseDir));

ErrorExit:
    if (Fd >= 0)
    {
        close(Fd);
    }

    rmdir(BaseDir);
    return Result;
}

int LxtFsGetDentsAlignmentCommon(const char* BaseDir, int Flags)

/*++

Description:

    This routine tests the alignment and padding of the entries returned by
    getdents.

Arguments:

    BaseDir - Supplies the directory to use.

    Flags - Supplies the flags.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    char Buffer[4096];
    int Count;
    struct linux_dirent* Entry;
    struct dirent64* Entry64;
    int Fd;
    int Fd2;
    int Index;
    int Length;
    const int MaxChildLength = 16;
    char Name[MaxChildLength + 1];
    int Offset;
    int Result;
    int Size;

    Fd = -1;
    Fd2 = -1;

    LxtCheckErrnoZeroSuccess(mkdir(BaseDir, 0777));
    LxtCheckErrno(Fd = open(BaseDir, O_RDONLY | O_DIRECTORY));

    //
    // No need to create entries with length 1 and 2 since the . and .. entries
    // already take care of that.
    //

    for (Length = 3; Length <= MaxChildLength; Length += 1)
    {
        for (Index = 0; Index < Length; Index += 1)
        {
            Name[Index] = 'a' + Index;
        }

        Name[Length] = '\0';
        LxtCheckErrnoZeroSuccess(mkdirat(Fd, Name, 0777));
    }

    if ((Flags & FS_TEST_GETDENTS64) == 0)
    {
#ifdef __NR_getdents
        LxtCheckErrno(Size = LxtGetdents(Fd, Buffer, sizeof(Buffer)));
#else
        LxtLogError("Test not supported on this architecture.");
        Result = LXT_RESULT_FAILURE;
        goto ErrorExit;
#endif
    }
    else
    {
        LxtCheckErrno(Size = LxtGetdents64(Fd, Buffer, sizeof(Buffer)));
    }

    LxtCheckGreater(Size, 0, "%d");
    LxtCheckEqual(Size % sizeof(long), 0, "%d");
    Offset = 0;
    Count = 0;
    while (Offset < Size)
    {

        //
        // Verify the record length of each entry.
        //
        // N.B. These sizes are precalculated for amd64; they may need to be
        //      adjusted on different architectures.
        //

        if ((Flags & FS_TEST_GETDENTS64) == 0)
        {
            Entry = (struct linux_dirent*)&Buffer[Offset];
            Length = strlen(Entry->d_name);
            LxtLogInfo("getdents %s: %d", Entry->d_name, Entry->d_reclen);
            if (Length <= 4)
            {
                LxtCheckEqual(Entry->d_reclen, 24, "%d");
            }
            else if (Length <= 12)
            {
                LxtCheckEqual(Entry->d_reclen, 32, "%d");
            }
            else
            {
                LxtCheckEqual(Entry->d_reclen, 40, "%d");
            }

            //
            // Make sure the file type is in the last entry.
            //

            LxtCheckEqual(Buffer[Offset + Entry->d_reclen - 1], DT_DIR, "%d");
            Offset += Entry->d_reclen;
        }
        else
        {
            Entry64 = (struct dirent64*)&Buffer[Offset];
            Length = strlen(Entry64->d_name);
            LxtLogInfo("getdents64 %s: %d", Entry64->d_name, Entry64->d_reclen);
            if (Length <= 4)
            {
                LxtCheckEqual(Entry64->d_reclen, 24, "%d");
            }
            else if (Length <= 12)
            {
                LxtCheckEqual(Entry64->d_reclen, 32, "%d");
            }
            else
            {
                LxtCheckEqual(Entry64->d_reclen, 40, "%d");
            }

            LxtCheckEqual(Entry64->d_type, DT_DIR, "%d");
            Offset += Entry64->d_reclen;
        }

        Count += 1;
    }

    LxtCheckEqual(Count, MaxChildLength, "%d");

    //
    // Open a child for the buffer size tests; on real Linux, the entries are
    // not in any specific order so the minimum entry size may not fit the
    // actual first item returned. The child directory is empty so the entries
    // will fit in the minimum size.
    //

    LxtCheckErrno(Fd2 = openat(Fd, "abc", O_RDONLY | O_DIRECTORY));
    if ((Flags & FS_TEST_GETDENTS64) == 0)
    {

        //
        // getdents variation (and not getdents64) won't run when
        // __NR_getdents is not defined. So, commenting out the
        // below is just for compilation safety.
        //

#ifdef __NR_getdents
        LxtCheckErrnoFailure(LxtGetdents(Fd2, Buffer, 23), EINVAL);
        LxtCheckErrno(Size = LxtGetdents(Fd2, Buffer, 24));
        LxtCheckEqual(Size, 24, "%d");
#endif
    }
    else
    {
        LxtCheckErrnoFailure(LxtGetdents64(Fd2, Buffer, 23), EINVAL);
        LxtCheckErrno(Size = LxtGetdents64(Fd2, Buffer, 24));
        LxtCheckEqual(Size, 24, "%d");
    }

ErrorExit:
    if (Fd2 >= 0)
    {
        close(Fd2);
    }

    if (Fd >= 0)
    {
        for (Length = 3; Length <= MaxChildLength; Length += 1)
        {
            for (Index = 0; Index < Length; Index += 1)
            {
                Name[Index] = 'a' + Index;
            }

            Name[Length] = '\0';
            unlinkat(Fd, Name, AT_REMOVEDIR);
        }

        close(Fd);
    }

    rmdir(BaseDir);
    return Result;
}

int LxtFsGetFsInfo(const char* Path, PLXT_FS_INFO Info)

/*++

Description:

    This routine gets file system information.

Arguments:

    Path - Supplies the test root path.

    Info - Supplies a buffer which receives file system information.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    char FsType[10];
    LXT_FS_INFO LocalInfo;
    char Options[1024];
    int Result;

    memset(&LocalInfo, 0, sizeof(LocalInfo));

    LxtCheckResult(MountGetFileSystem(Path, FsType, sizeof(FsType), Options, sizeof(Options)));

    if (strcmp(FsType, FS_DRVFS_NAME) == 0)
    {
        LocalInfo.FsType = LxtFsTypeDrvFs;
        LocalInfo.Flags.DrvFsBehavior = 1;
    }
    else if (strcmp(FsType, FS_WSLFS_NAME) == 0)
    {
        LocalInfo.FsType = LxtFsTypeWslFs;
        LocalInfo.Flags.DrvFsBehavior = 1;
        LocalInfo.Flags.Cached = 1;
    }
    else if (strcmp(FsType, FS_9P_NAME) == 0)
    {
        LocalInfo.FsType = LxtFsTypePlan9;
        LocalInfo.Flags.DrvFsBehavior = 1;
        if (strstr(Options, "loose") != NULL)
        {
            LocalInfo.Flags.Cached = 1;
        }

        if (strstr(Options, "trans=virtio") != NULL)
        {
            LocalInfo.Flags.VirtIo = 1;
        }
    }
    else if (strcmp(FsType, FS_VIRTIOFS_NAME) == 0)
    {
        LocalInfo.FsType = LxtFsTypeVirtioFs;
        LocalInfo.Flags.DrvFsBehavior = 1;
        if (strstr(Options, "dax") != NULL)
        {
            LocalInfo.Flags.Dax = 1;
        }
    }
    else
    {
        LocalInfo.FsType = LxtFsTypeLxFs;
    }

    *Info = LocalInfo;
    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    return Result;
}

int LxtFsInotifyEpollCommon(char* BaseDir)

/*++

Description:

    This routine contains common inotify epoll tests that are shared between
    lxfs and drvfs tests.

Arguments:

    BaseDir - Supplies the base directory for the test.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int Ed;
    int Fd;
    int Id;
    int Wd;
    int Result;
    char Buf[10];
    char InotifyBuf[500];
    struct inotify_event* Events[INOTIFY_TEST_EVENTS_BUF_SIZE];
    int NumEvents;
    struct epoll_event EpollControlEvent;
    struct epoll_event EpollWaitEvent[2];
    char TestFile1[PATH_MAX];
    char TestFile2[PATH_MAX];
    char TestFile1Hlink[PATH_MAX];
    char TestFile1Slink[PATH_MAX];

    //
    // Initialize and also do cleanup if the files have not been removed.
    //

    sprintf(TestFile1, "%s%s", BaseDir, INOTIFY_TEST_FILE1_NAME_ONLY);
    sprintf(TestFile2, "%s%s", BaseDir, INOTIFY_TEST_FILE2_NAME_ONLY);
    sprintf(TestFile1Hlink, "%s%s", BaseDir, INOTIFY_TEST_FILE1_HLINK_NAME_ONLY);
    sprintf(TestFile1Slink, "%s%s", BaseDir, INOTIFY_TEST_FILE1_SLINK_NAME_ONLY);
    unlink(TestFile1);
    unlink(TestFile2);
    unlink(TestFile1Hlink);
    unlink(TestFile1Slink);
    rmdir(BaseDir);
    LxtCheckErrnoZeroSuccess(mkdir(BaseDir, 0777));
    LxtCheckErrno(Fd = creat(TestFile1, 0777));
    LxtCheckErrnoZeroSuccess(close(Fd));

    LxtCheckErrno(Id = inotify_init());
    LxtCheckErrno(Result = inotify_add_watch(Id, TestFile1, IN_ALL_EVENTS));

    //
    // Create an epoll container and add the inotify file descriptor to it.
    //

    LxtCheckErrno(Ed = epoll_create(1));
    EpollControlEvent.events = EPOLLIN;
    EpollControlEvent.data.fd = Id;
    Result = epoll_ctl(Ed, EPOLL_CTL_ADD, Id, &EpollControlEvent);
    LxtCheckErrno(Result);

    //
    // Wait for data to be available with a timeout. This should timeout since
    // there is no data.
    //

    Result = epoll_wait(Ed, EpollWaitEvent, 2, 80);
    LxtCheckEqual(Result, 0, "%d");

    //
    // Generate some inotify events.
    //

    LxtCheckErrno(Fd = open(TestFile1, O_WRONLY));
    LxtCheckErrno(Result = write(Fd, Buf, 10));
    LxtCheckEqual(Result, 10, "%d");
    LxtCheckErrnoZeroSuccess(close(Fd));

    //
    // Now epoll should return 1 ready file descriptor.
    //

    Result = epoll_wait(Ed, EpollWaitEvent, 2, 1000);
    LxtCheckEqual(Result, 1, "%d");

    //
    // Drain the inotify events.
    //

    usleep(1000 * 80);
    LxtCheckErrno(LxtFsInotifyReadAndProcess(Id, InotifyBuf, sizeof(InotifyBuf), Events, INOTIFY_TEST_EVENTS_BUF_SIZE, &NumEvents, FALSE));

    LxtCheckTrue(NumEvents > 0);

    //
    // Epoll should timeout again, since there are no events to be read.
    //

    Result = epoll_wait(Ed, EpollWaitEvent, 2, 80);
    LxtCheckEqual(Result, 0, "%d");

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    close(Id);
    close(Ed);
    unlink(TestFile1);
    rmdir(BaseDir);
    return Result;
}

int LxtFsInotifyPosixUnlinkRenameCommon(char* BaseDir)

/*++

Description:

    This routine contains common inotify POSIX unlink/rename tests that are
    shared between lxfs and drvfs tests.

Arguments:

    BaseDir - Supplies the base directory for the test.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int Fd;
    int Id;
    int Result;
    char TestFile1[PATH_MAX];
    char TestFile2[PATH_MAX];
    char TestFile3[PATH_MAX];

    //
    // Initialize and also do cleanup if the files have not been removed.
    //

    sprintf(TestFile1, "%s%s", BaseDir, INOTIFY_TEST_FILE1_NAME_ONLY);
    sprintf(TestFile2, "%s%s", BaseDir, INOTIFY_TEST_FILE2_NAME_ONLY);
    sprintf(TestFile3, "%s%s", BaseDir, INOTIFY_TEST_FILE3_NAME_ONLY);
    unlink(TestFile1);
    unlink(TestFile2);
    unlink(TestFile3);
    rmdir(BaseDir);
    LxtCheckErrnoZeroSuccess(mkdir(BaseDir, 0777));
    LxtCheckErrno(Fd = creat(TestFile1, 0777));
    LxtCheckErrnoZeroSuccess(close(Fd));
    LxtCheckErrno(Fd = creat(TestFile2, 0777));
    LxtCheckErrnoZeroSuccess(close(Fd));
    LxtCheckErrno(Fd = creat(TestFile3, 0777));
    LxtCheckErrnoZeroSuccess(close(Fd));

    //
    // Setup inotify.
    //

    LxtCheckErrno(Id = inotify_init());
    LxtCheckErrno(Result = inotify_add_watch(Id, TestFile1, IN_ALL_EVENTS));
    LxtCheckErrno(Result = inotify_add_watch(Id, TestFile2, IN_ALL_EVENTS));
    LxtCheckErrno(Result = inotify_add_watch(Id, TestFile3, IN_ALL_EVENTS));

    //
    // Test that POSIX unlinking a file watched by inotify succeeds. Verify that
    // a new file with the same name can be created.
    //

    LxtCheckErrnoZeroSuccess(unlink(TestFile1));
    LxtCheckErrnoFailure(Fd = open(TestFile1, O_RDONLY), ENOENT);
    LxtCheckErrno(Fd = creat(TestFile1, 0777));
    LxtCheckErrnoZeroSuccess(close(Fd));

    //
    // Test that renaming a file with overwrite succeeds when both files are
    // being watched by inotify.
    //

    LxtCheckErrnoZeroSuccess(rename(TestFile2, TestFile3));
    LxtCheckErrnoFailure(Fd = open(TestFile2, O_RDONLY), ENOENT);
    LxtCheckErrno(Fd = creat(TestFile2, 0777));
    LxtCheckErrnoZeroSuccess(close(Fd));
    LxtCheckErrnoFailure(Fd = open(TestFile3, O_CREAT | O_EXCL), EEXIST);
    LxtCheckErrno(Fd = open(TestFile3, O_RDONLY));
    LxtCheckErrnoZeroSuccess(close(Fd));

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    close(Id);
    close(Fd);
    unlink(TestFile1);
    unlink(TestFile2);
    unlink(TestFile3);
    rmdir(BaseDir);
    return Result;
}

int LxtFsInotifyReadAndProcess(int Id, char* ReadBuf, int ReadBufSize, struct inotify_event** Events, int NumEventsIn, int* NumEventsOut, int IgnoreAttribModify)

/*++

Description:

    This routine reads from the supplied inotify fd and returns a list
    of pointers to all the inotify events read.

Arguments:

    Id - Inotify file descriptor.

    ReadBuf - Buffer to read the inotify events into.

    ReadBufSize - Size of ReadBuf in bytes.

    Events - An array of pointers to events to fill in.

    NumEventsIn - Max number of event pointers that Events can hold.

    NumEventsOut - Number of event pointers filled into Events.

    IgnoreAttribModify - Whether to ignore IN_ATTRIB and IN_MODIFY for
        directories (used for DrvFs only).

Return Value:

    Returns the number of bytes read, -1 on failure.

--*/

{

    char* Ptr;
    struct inotify_event* Event;
    int Result;
    int NumEvents;

    NumEvents = 0;
    Result = read(Id, ReadBuf, ReadBufSize);
    if (Result < 0)
    {
        goto ErrorExit;
    }

    for (Ptr = ReadBuf; Ptr < ReadBuf + Result; Ptr += sizeof(struct inotify_event) + Event->len)
    {

        if (NumEvents >= NumEventsIn)
        {
            Result = -1;
            goto ErrorExit;
        }

        Event = (struct inotify_event*)Ptr;

        //
        // Ignore IN_ATTRIB and IN_MODIFY for DrvFs directories, since Windows
        // generates lots of irrelevant such notifications for DrvFs directories.
        //

        if ((IgnoreAttribModify != FALSE) && ((Event->mask & IN_ISDIR) != 0) && ((Event->mask & (IN_ATTRIB | IN_MODIFY)) != 0))
        {

            continue;
        }

        Events[NumEvents] = Event;
        NumEvents += 1;
    }

ErrorExit:
    *NumEventsOut = NumEvents;
    return Result;
}

int LxtFsInotifyUnmountBindCommon(char* BaseDir)

/*++

Description:

    This routine contains common inotify unmount tests that are shared between
    lxfs and drvfs.

Arguments:

    BaseDir - Supplies the base directory for the test.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int Fd;
    int Id1;
    int Wd[10];
    char Buf[10];
    char InotifyBuf[500];
    struct inotify_event* Events[INOTIFY_TEST_EVENTS_BUF_SIZE];
    int NumEvents;
    int Bytes;
    int Result;
    char TestFile1[PATH_MAX];
    char TestFile2[PATH_MAX];
    char TestFile1Hlink[PATH_MAX];
    char TestFile1Slink[PATH_MAX];
    char TestDir1[PATH_MAX];
    char TestDir2[PATH_MAX];
    char TestDir11[PATH_MAX];
    char TestFile111[PATH_MAX];

    //
    // Initialize and also do cleanup if the files have not been removed.
    //

    sprintf(TestFile1, "%s%s", BaseDir, INOTIFY_TEST_FILE1_NAME_ONLY);
    sprintf(TestFile2, "%s%s", BaseDir, INOTIFY_TEST_FILE2_NAME_ONLY);
    sprintf(TestFile1Hlink, "%s%s", BaseDir, INOTIFY_TEST_FILE1_HLINK_NAME_ONLY);
    sprintf(TestFile1Slink, "%s%s", BaseDir, INOTIFY_TEST_FILE1_SLINK_NAME_ONLY);
    sprintf(TestDir1, "%s%s", BaseDir, "bind_mount_tmp/");
    sprintf(TestDir2, "%s%s", BaseDir, "bind_mount_tmp_2/");
    sprintf(TestDir11, "%s%s", TestDir1, "subdir11/");
    sprintf(TestFile111, "%s%s", TestDir11, "file111");
    unlink(TestFile1);
    unlink(TestFile2);
    unlink(TestFile1Hlink);
    unlink(TestFile1Slink);
    rmdir(TestDir11);
    umount(TestDir2);
    rmdir(TestDir2);
    umount(TestDir1);
    rmdir(TestDir1);
    rmdir(BaseDir);

    LxtCheckErrnoZeroSuccess(mkdir(BaseDir, 0777));
    LxtCheckErrno(Fd = creat(TestFile1, 0777));
    LxtCheckErrnoZeroSuccess(close(Fd));

    //
    // Create a tmpfs (TestDir1) and create some files inside it.
    //

    LxtCheckErrnoZeroSuccess(mkdir(TestDir1, 0777));
    LxtCheckErrnoZeroSuccess(mount("tmpfs", TestDir1, "tmpfs", 0, NULL));
    LxtCheckErrnoZeroSuccess(mkdir(TestDir11, 0777));
    LxtCheckErrno(Fd = creat(TestFile111, 0777));
    LxtCheckErrnoZeroSuccess(close(Fd));

    //
    // Create a bind mount (TestDir2) to the tmpfs (TestDir1).
    //

    LxtCheckErrnoZeroSuccess(mkdir(TestDir2, 0777));
    LxtCheckErrnoZeroSuccess(mount(TestDir1, TestDir2, NULL, MS_BIND, NULL));

    //
    // Setup inotify and verify. Note that TestDir1 and TestDir2 point to the
    // same inode (directory).
    //

    LxtCheckErrno(Id1 = inotify_init1(IN_NONBLOCK));

    LxtCheckErrno(
        Wd[0] = // wd: 1
        inotify_add_watch(Id1, TestDir1, IN_ALL_EVENTS));

    LxtCheckErrno(
        Wd[1] = // wd: 2
        inotify_add_watch(Id1, TestDir11, IN_ALL_EVENTS));

    LxtCheckErrno(
        Wd[2] = // wd: 3
        inotify_add_watch(Id1, TestFile111, IN_ALL_EVENTS));

    LxtCheckErrno(
        Wd[3] = // wd: 1
        inotify_add_watch(Id1, TestDir2, IN_ALL_EVENTS));

    LxtCheckEqual(Wd[0], 1, "%d");
    LxtCheckEqual(Wd[1], 2, "%d");
    LxtCheckEqual(Wd[2], 3, "%d");
    LxtCheckEqual(Wd[0], Wd[3], "%d");

    //
    // Umount TestDir1. Verify that nothing happens in inotify since the inodes
    // are not really unmounted (TestDir2 is still mounted).
    //

    LxtCheckErrnoZeroSuccess(umount(TestDir1));

    LxtCheckErrnoFailure(
        LxtFsInotifyReadAndProcess(Id1, InotifyBuf, sizeof(InotifyBuf), Events, INOTIFY_TEST_EVENTS_BUF_SIZE, &NumEvents, FALSE), EAGAIN);

    LxtCheckEqual(NumEvents, 0, "%d");

    //
    // Unmount TestDir2. Now, each file and directory inside TestDir2 should
    // generate 2 inotify events (IN_UNMOUNT, then IN_IGNORED).
    //

    LxtCheckErrnoZeroSuccess(umount(TestDir2));

    LxtCheckErrno(LxtFsInotifyReadAndProcess(Id1, InotifyBuf, sizeof(InotifyBuf), Events, INOTIFY_TEST_EVENTS_BUF_SIZE, &NumEvents, FALSE));

    LxtCheckEqual(NumEvents, 6, "%d");
    LxtCheckEqual(Events[0]->mask, IN_UNMOUNT, "%d");
    LxtCheckEqual(Events[1]->mask, IN_IGNORED, "%d");

    //
    // In Linux kernel 5.10 the IN_ISDIR flag is also returned.
    // Ignore the presence of this flag so this test can be run on multiple
    // versions of the kernel.
    //

    Events[2]->mask &= ~IN_ISDIR;
    Events[4]->mask &= ~IN_ISDIR;
    LxtCheckEqual(Events[2]->mask, IN_UNMOUNT, "%d");
    LxtCheckEqual(Events[3]->mask, IN_IGNORED, "%d");
    LxtCheckEqual(Events[4]->mask, IN_UNMOUNT, "%d");
    LxtCheckEqual(Events[5]->mask, IN_IGNORED, "%d");
    LxtCheckEqual(Events[0]->wd, Events[1]->wd, "%d");
    LxtCheckEqual(Events[2]->wd, Events[3]->wd, "%d");
    LxtCheckEqual(Events[4]->wd, Events[5]->wd, "%d");
    LxtCheckNotEqual(Events[0]->wd, Events[2]->wd, "%d");
    LxtCheckNotEqual(Events[2]->wd, Events[4]->wd, "%d");

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    close(Id1);
    close(Fd);
    unlink(TestFile1);
    rmdir(TestDir11);
    umount(TestDir2);
    rmdir(TestDir2);
    umount(TestDir1);
    rmdir(TestDir1);
    rmdir(BaseDir);
    return Result;
}

int LxtFsMountDrvFs(const char* Source, const char* Target, const char* Options)

/*++

Description:

    This routine mounts drvfs through the mount binary.

    N.B. This allows drvfs to be mounted using 9p for WSL 2.

Arguments:

    Source - Supplies the mount source.

    Target - Supplies the mount target.

    Options - Supplies optional mount options.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    char Command[4096];
    size_t Index;
    int Result;

    if (Options == NULL)
    {
        snprintf(Command, sizeof(Command), FS_MOUNT_DRVFS_COMMAND_FORMAT, Source, Target);
    }
    else
    {
        snprintf(Command, sizeof(Command), FS_MOUNT_DRVFS_OPTIONS_COMMAND_FORMAT, Source, Target, Options);
    }

    LxtCheckErrnoZeroSuccess(system(Command));

ErrorExit:
    return Result;
}

int LxtFsRenameAtCommon(int DirFd1, int DirFd2)

/*++

Description:

    This routine tests the renameat system call on volfs.

Arguments:

    DirFd1 - Supplies a file descriptor for the first directory to use for the test.

    DirFd2 - Supplies a file descriptor for the second directory to use for the test.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int Fd1;
    int Result;

    Fd1 = -1;

    //
    // Create a file and rename it using AT_FDCWD.
    //

    LxtCheckErrno(Fd1 = open(FS_RENAMEAT_TEST_FILE, O_RDWR | O_CREAT, S_IRWXU));
    LxtClose(Fd1);
    Fd1 = -1;

    LxtCheckErrno(renameat(AT_FDCWD, FS_RENAMEAT_TEST_FILE, AT_FDCWD, FS_RENAMEAT_TEST_FILE2));

    LxtCheckErrno(Fd1 = open(FS_RENAMEAT_TEST_FILE, O_RDWR | O_CREAT, S_IRWXU));
    LxtClose(Fd1);
    Fd1 = -1;

    LxtCheckErrno(renameat(AT_FDCWD, FS_RENAMEAT_TEST_FILE, DirFd2, FS_RENAMEAT_TEST_FILE));

    LxtCheckErrno(Fd1 = openat(DirFd1, FS_RENAMEAT_TEST_FILE, O_RDWR | O_CREAT, S_IRWXU));
    LxtClose(Fd1);
    Fd1 = -1;

    //
    // Rename a file over an existing file, but add a trailing slash to the target filename, error.
    //

    LxtCheckErrnoFailure(renameat(DirFd1, FS_RENAMEAT_TEST_FILE, DirFd2, FS_RENAMEAT_TEST_FILE "/"), ENOTDIR);

    LxtCheckErrno(renameat(DirFd1, FS_RENAMEAT_TEST_FILE, AT_FDCWD, FS_RENAMEAT_TEST_FILE2));

    //
    // Create a file and rename it into a subdirectory.
    //

    LxtCheckErrno(Fd1 = openat(DirFd1, FS_RENAMEAT_TEST_FILE, O_RDWR | O_CREAT, S_IRWXU));
    LxtClose(Fd1);
    Fd1 = -1;

    LxtCheckErrno(renameat(DirFd1, FS_RENAMEAT_TEST_FILE, DirFd2, FS_RENAMEAT_TEST_FILE));

    //
    // Create a new file and rename it into a deeper subdirectory.
    // The rename originally renamed the file into a/b/c/d/e/f/, but due to Bug 8405643, the file
    // is now renamed into a/b/c, so that the directory rename further below will not fail (It
    // sometimes fails if the source directory contains a file).
    //

    LxtCheckErrno(Fd1 = openat(DirFd1, FS_RENAMEAT_TEST_FILE, O_RDWR | O_CREAT, S_IRWXU));
    LxtClose(Fd1);
    Fd1 = -1;

    LxtCheckErrno(renameat(DirFd1, FS_RENAMEAT_TEST_FILE, DirFd2, FS_RENAMEAT_TEST_FILE));

    //
    // Attempt to rename a folder into a descendent of itself.
    //

    LxtCheckErrnoFailure(renameat(DirFd1, "b", DirFd2, "d/e/f/b"), EINVAL);

    //
    // Rename folders that have trailing slashes.
    //

    LxtCheckErrnoZeroSuccess(renameat(DirFd1, "b/c/d/", DirFd1, "b/d"));
    LxtCheckErrnoZeroSuccess(renameat(DirFd1, "b/d/", DirFd1, "b/c/d"));
    LxtCheckErrnoZeroSuccess(renameat(DirFd1, "b/c/d/", DirFd1, "b/d/"));
    LxtCheckErrnoZeroSuccess(renameat(DirFd1, "b/d/", DirFd1, "b/c/d/"));

    //
    // Invalid parameter variations.
    //

    LxtCheckErrnoFailure(renameat(-1, "b", DirFd2, "d/e/f/b"), EBADF);
    LxtCheckErrnoFailure(renameat(DirFd1, NULL, DirFd2, "d/e/f/b"), EFAULT);
    LxtCheckErrnoFailure(renameat(DirFd1, "b", -1, "d/e/f/b"), EBADF);
    LxtCheckErrnoFailure(renameat(DirFd1, "b", DirFd2, NULL), EFAULT);

ErrorExit:
    if (Fd1 >= 0)
    {
        LxtClose(Fd1);
    }

    unlinkat(DirFd2, "d/e/f/" FS_RENAMEAT_TEST_FILE, 0);
    unlinkat(DirFd2, FS_RENAMEAT_TEST_FILE, 0);
    unlinkat(AT_FDCWD, FS_RENAMEAT_TEST_FILE2, 0);

    return Result;
}

int LxtFsRenameDirCommon(char* BaseDir)

/*++

Description:

    This routine tests the rename system call for directories.

Arguments:

    BaseDir - Supplies the top directory to use for the test.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int Attempts;
    int Attempts2;
    pid_t ChildPid;
    int Fd;
    char RenameTestDir[PATH_MAX];
    char RenameTestDirChild[PATH_MAX];
    char RenameTestDirGrandchild[PATH_MAX];
    char RenameTestDirSlash[PATH_MAX];
    char RenameTestDirSlash2[PATH_MAX];
    char RenameTestDirSlashLink[PATH_MAX];
    char RenameTestDirSlashLinkSlash[PATH_MAX];
    char RenameTestDirSlashLink2[PATH_MAX];
    char RenameTestDir2[PATH_MAX];
    char RenameTestDir2Child[PATH_MAX];
    char RenameTestDir2Child2[PATH_MAX];
    char RenameTestDir2Slash[PATH_MAX];
    char RenameTestDir3[PATH_MAX];
    char RenameTestDir3Slash[PATH_MAX];
    char RenameTestFile[PATH_MAX];
    int Result;
    int Status;

    ChildPid = -1;
    Fd = -1;
    sprintf(RenameTestDir, "%s%s", BaseDir, FS_RENAME_TEST_DIR);
    sprintf(RenameTestDirChild, "%s%s", BaseDir, FS_RENAME_TEST_DIR_CHILD);
    sprintf(RenameTestDirGrandchild, "%s%s", BaseDir, FS_RENAME_TEST_DIR_GRANDCHILD);
    sprintf(RenameTestDirSlash, "%s%s", BaseDir, FS_RENAME_TEST_DIR_SLASH);
    sprintf(RenameTestDirSlash2, "%s%s", BaseDir, FS_RENAME_TEST_DIR_SLASH2);
    sprintf(RenameTestDirSlashLink, "%s%s", BaseDir, FS_RENAME_TEST_DIR_SLASH_LINK);
    sprintf(RenameTestDirSlashLinkSlash, "%s%s/", BaseDir, FS_RENAME_TEST_DIR_SLASH_LINK);
    sprintf(RenameTestDirSlashLink2, "%s%s", BaseDir, FS_RENAME_TEST_DIR_SLASH_LINK2);
    sprintf(RenameTestDir2, "%s%s", BaseDir, FS_RENAME_TEST_DIR2);
    sprintf(RenameTestDir2Child, "%s%s", BaseDir, FS_RENAME_TEST_DIR2_CHILD);
    sprintf(RenameTestDir2Child2, "%s%s", BaseDir, FS_RENAME_TEST_DIR2_CHILD2);
    sprintf(RenameTestDir2Slash, "%s%s////", BaseDir, FS_RENAME_TEST_DIR2);
    sprintf(RenameTestDir3, "%s%s", BaseDir, FS_RENAME_TEST_DIR3);
    sprintf(RenameTestDir3Slash, "%s%s/", BaseDir, FS_RENAME_TEST_DIR3);
    sprintf(RenameTestFile, "%s%s", BaseDir, FS_RENAME_TEST_FILE);

    //
    // Renaming a directory to be its own parent should fail with ENOTEMPTY.
    //

    LxtCheckResult(LxtFsCreateTestDir(RenameTestDir));
    LxtCheckErrnoZeroSuccess(mkdir(RenameTestDirChild, 0777));
    LxtCheckErrnoFailure(rename(RenameTestDirChild, RenameTestDir), ENOTEMPTY);

    //
    // Or ancestor.
    //

    LxtCheckErrnoZeroSuccess(mkdir(RenameTestDirGrandchild, 0777));
    LxtCheckErrnoFailure(rename(RenameTestDirGrandchild, RenameTestDir), ENOTEMPTY);

    //
    // Renaming it over an existing directory should succeed.
    //

    LxtCheckErrnoZeroSuccess(mkdir(RenameTestDir2, 0777));
    LxtCheckErrnoZeroSuccess(rename(RenameTestDirChild, RenameTestDir2));

    //
    // Renaming to a non-existent directory where the target directory
    // contains trailing slash(es).
    //

    LxtCheckErrnoZeroSuccess(rename(RenameTestDir2, RenameTestDir3Slash));
    LxtCheckErrnoZeroSuccess(rename(RenameTestDir3, RenameTestDir2Slash));

    //
    // Renaming a file to its parent should also fail.
    //

    LxtCheckErrno(Fd = creat(RenameTestDirChild, 0666));
    LxtCheckErrnoZeroSuccess(close(Fd));
    Fd = -1;
    LxtCheckErrnoFailure(rename(RenameTestDirChild, RenameTestDir), ENOTEMPTY);

    //
    // Renaming a file over a directory should fail with EISDIR.
    //

    LxtCheckErrnoFailure(rename(RenameTestDirChild, RenameTestDir2), EISDIR);

    //
    // And directory over file should fail with ENOTDIR.
    //

    LxtCheckErrnoFailure(rename(RenameTestDir2, RenameTestDirChild), ENOTDIR);

    //
    // The following test cases deal with the old name containing a trailing slash.
    //
    // Renaming a directory to a nonexistent directory should succeed.
    //

    LxtCheckErrnoZeroSuccess(mkdir(RenameTestDirSlash, 0777));
    LxtCheckErrnoZeroSuccess(rename(RenameTestDirSlash, RenameTestDirSlash2));

    //
    // Renaming a symlink with a trailing slash should fail with ENOTDIR.
    //

    LxtCheckErrnoZeroSuccess(symlink(RenameTestDirSlash2, RenameTestDirSlashLink));
    LxtCheckErrnoFailure(rename(RenameTestDirSlashLinkSlash, RenameTestDirSlashLink2), ENOTDIR);

    //
    // The following tests reproduce a bug in lxcore when renaming a directory
    // fails due to open handles to a descendant of that directory. Since this
    // scenario would not fail on real Linux in the first place, this test
    // will not pass on real Linux.
    //
    // Once lxcore is able to rename directories with open handles to a
    // descendant, this test will need to be updated.
    //

    LxtCheckErrnoZeroSuccess(rmdir(RenameTestDir2Child2));
    LxtCheckErrnoZeroSuccess(rmdir(RenameTestDir2));
    LxtLogInfo("This test will not pass on real Linux.");

    //
    // Keep the child open so renaming the directory will fail on lxcore.
    //

    LxtCheckErrno(Fd = open(RenameTestDirChild, O_RDONLY));
    LxtCheckErrnoFailure(rename(RenameTestDir, RenameTestDir2), EACCES);

    //
    // Attempt to unlink the child while it is still open.
    //

    LxtCheckErrnoZeroSuccess(unlink(RenameTestDirChild));
    LxtCheckErrnoZeroSuccess(close(Fd));
    Fd = -1;

    //
    // Repeat, but this time use the file as a rename target.
    //

    LxtCheckErrno(Fd = creat(RenameTestDirChild, 0666));
    LxtCheckErrnoZeroSuccess(close(Fd));
    Fd = -1;
    LxtCheckErrno(Fd = creat(RenameTestFile, 0666));
    LxtCheckErrnoZeroSuccess(close(Fd));
    Fd = -1;
    LxtCheckErrno(Fd = open(RenameTestDirChild, O_RDONLY));
    LxtCheckErrnoFailure(rename(RenameTestDir, RenameTestDir2), EACCES);
    LxtCheckErrnoZeroSuccess(rename(RenameTestFile, RenameTestDirChild));
    LxtCheckClose(Fd);

ErrorExit:
    LXT_SYNCHRONIZATION_POINT_END();

    if (Fd >= 0)
    {
        close(Fd);
    }

    unlink(RenameTestDirSlashLink);
    rmdir(RenameTestDirSlash);
    rmdir(RenameTestDirSlash2);
    rmdir(RenameTestDirGrandchild);
    unlink(RenameTestDirChild);
    unlink(RenameTestDir2Child);
    unlink(RenameTestFile);
    rmdir(RenameTestDirChild);
    rmdir(RenameTestDir);
    rmdir(RenameTestDir2Child2);
    rmdir(RenameTestDir2);

    return Result;
}

void LxtFsTestCleanup(const char* TestDir, const char* DrvFsDir, bool UseDrvFs)

/*++

Description:

    This routine cleans up directories and mounts created by LxtFsTestSetup.

Arguments:

    TestDir - Supplies the test parent directory.

    DrvFsDir - Supplies the DrvFs test directory.

    UseDrvFs - Supplies a value which indicates whether DrvFs was used for the
        test.

Return Value:

    None.

--*/

{

    char DrvFsPath[PATH_MAX];

    //
    // Clean up DrvFs if needed, and remount with default options.
    //
    // N.B. Make sure the cwd isn't in the dir, as this prevents unmounting.
    //

    if (UseDrvFs != false)
    {
        chdir("/");
        umount(TestDir);
        snprintf(DrvFsPath, sizeof(DrvFsPath), "%s%s", FS_DRVFS_PREFIX, DrvFsDir);
        rmdir(DrvFsPath);
        umount(FS_DRVFS_PREFIX);
        LxtFsMountDrvFs(FS_DRVFS_DRIVE, FS_DRVFS_PREFIX, NULL);
    }

    rmdir(TestDir);
    return;
}

int LxtFsTestSetup(PLXT_ARGS Args, const char* TestDir, const char* DrvFsDir, bool UseDrvFs)

/*++

Description:

    This routine sets up the test environment for tests that can run on either
    lxfs or drvfs. It also populates the global FS info structure.

Arguments:

    Args - Supplies the arguments.

    TestDir - Supplies the root directory for the tests.

    DrvFsDir - Supplies the DrvFs directory to use for testing. This must
        start with a slash, and be relative from the root of the DrvFs mount.

    UseDrvFs - Supplies a pa value indicating whether DrvFs is being used.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    char DrvFsPath[PATH_MAX];
    int Index;
    int ParentId;
    int Result;

    //
    // Do nothing if the caller just wants to show help.
    //

    if (Args->HelpRequested != false)
    {
        Result = 0;
        goto ErrorExit;
    }

    //
    // Create the test dir.
    //

    LxtCheckErrnoZeroSuccess(mkdir(TestDir, 0777));
    if (UseDrvFs == FALSE)
    {
        LxtCheckResult(LxtFsGetFsInfo(TestDir, &g_LxtFsInfo));
        Result = 0;
        goto ErrorExit;
    }

    //
    // If drvfs is used, metadata support is required. Unmount and remount
    // drvfs with metadata support.
    //
    // N.B. Make sure the cwd isn't inside DrvFs as this prevents unmounting.
    //

    LxtCheckErrnoZeroSuccess(chdir("/"));
    LxtCheckErrnoZeroSuccess(umount(FS_DRVFS_PREFIX));
    LxtCheckResult(ParentId = MountGetMountId(FS_DRVFS_PREFIX));
    LxtCheckResult(LxtFsMountDrvFs(FS_DRVFS_DRIVE, FS_DRVFS_PREFIX, "metadata,case=dir"));

    LxtCheckResult(LxtFsCheckDrvFsMount(FS_DRVFS_DRIVE, FS_DRVFS_PREFIX, "metadata,case=dir", ParentId, "/"));

    //
    // The tests can't be run on the root directory of the volume. Therefore,
    // create a directory to run the tests in and bind mount it to the test
    // root.
    //

    snprintf(DrvFsPath, sizeof(DrvFsPath), "%s%s", FS_DRVFS_PREFIX, DrvFsDir);
    LxtLogInfo("mkdir(%s, 0777)", DrvFsPath);
    LxtCheckErrnoZeroSuccess(mkdir(DrvFsPath, 0777));
    LxtCheckResult(ParentId = MountGetMountId(TestDir));
    LxtCheckErrnoZeroSuccess(mount(DrvFsPath, TestDir, NULL, MS_BIND, NULL));

    LxtCheckResult(LxtFsCheckDrvFsMount(FS_DRVFS_DRIVE, TestDir, "metadata,case=dir", ParentId, DrvFsDir));

    LxtCheckResult(LxtFsGetFsInfo(TestDir, &g_LxtFsInfo));

ErrorExit:
    return Result;
}

int LxtFsTimestampCheckCurrent(const struct timespec* Timestamp)

/*++

Description:

    This routine checks if a timestamp is close to the current time.

Arguments:

    Timestamp - Supplies the timestamp to check.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    struct timespec CurrentTime;
    int Result;

    //
    // File system timestamp updates use the coarse clock.
    //

    LxtCheckErrnoZeroSuccess(clock_gettime(CLOCK_REALTIME_COARSE, &CurrentTime));

    if (LxtFsUtimeDoTimesMatch(Timestamp, &CurrentTime, FS_FAT_MODIFIED_TIME_PRECISION) == false)
    {

        Result = LXT_RESULT_FAILURE;
        goto ErrorExit;
    }

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    return Result;
}

int LxtFsTimestampCheckEqual(const struct timespec* Timestamp1, const struct timespec* Timestamp2)

/*++

Description:

    This routine checks whether two timestamps are equal.

Arguments:

    Timestamp1 - Supplies the first timestamp.

    Timestamp2 - Supplies the second timestamp.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int Result;

    if (LxtFsUtimeDoTimesMatch(Timestamp1, Timestamp2, 0) == false)
    {
        Result = LXT_RESULT_FAILURE;
        goto ErrorExit;
    }

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    return Result;
}

int LxtFsTimestampCheckGreater(const struct timespec* Timestamp1, const struct timespec* Timestamp2)

/*++

Description:

    This routine checks if timestamp 1 is larger than timestamp 2.

Arguments:

    Timestamp1 - Supplies the first timestamp.

    Timestamp2 - Supplies the second timestamp.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int Result;

    if (LxtFsTimestampDiff(Timestamp1, Timestamp2) <= 0)
    {
        LxtLogError("Time %lld.%.9ld not greater than time %lld.%.9ld", Timestamp1, Timestamp2);

        Result = LXT_RESULT_FAILURE;
        goto ErrorExit;
    }

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    return Result;
}

int LxtFsTimestampCheckUpdate(const char* Path, struct stat* PreviousStat, int Flags)

/*++

Description:

    This routine checks if the specified timestamps have been updated.

Arguments:

    Path - Supplies the path of the file to check.

    PreviousStat - Supplies the previous timestamps, and gets updated with the
        current timestamp on success.

    Flags - Supplies the flags.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int Result;
    struct stat Stat;

    LxtCheckErrnoZeroSuccess(lstat(Path, &Stat));
    if ((Flags & FS_TIMESTAMP_ACCESS) != 0)
    {
        LxtCheckResult(LxtFsTimestampCheckGreater(&Stat.st_atim, &PreviousStat->st_atim));

        LxtCheckResult(LxtFsTimestampCheckCurrent(&Stat.st_atim));
    }
    else
    {
        LxtCheckResult(LxtFsTimestampCheckEqual(&Stat.st_atim, &PreviousStat->st_atim));
    }

    if ((Flags & FS_TIMESTAMP_MODIFY) != 0)
    {
        LxtCheckResult(LxtFsTimestampCheckGreater(&Stat.st_mtim, &PreviousStat->st_mtim));

        LxtCheckResult(LxtFsTimestampCheckCurrent(&Stat.st_mtim));
    }
    else
    {
        LxtCheckResult(LxtFsTimestampCheckEqual(&Stat.st_mtim, &PreviousStat->st_mtim));
    }

    if ((Flags & FS_TIMESTAMP_CHANGE) != 0)
    {
        LxtCheckResult(LxtFsTimestampCheckGreater(&Stat.st_ctim, &PreviousStat->st_ctim));

        LxtCheckResult(LxtFsTimestampCheckCurrent(&Stat.st_ctim));
    }
    else
    {
        LxtCheckResult(LxtFsTimestampCheckEqual(&Stat.st_ctim, &PreviousStat->st_ctim));
    }

    *PreviousStat = Stat;

ErrorExit:
    return Result;
}

int LxtFsTimestampCommon(const char* BaseDir, int Flags)

/*++

Description:

    This routine tests timestamp updates made by various file operations.

    N.B. These timestamp updates were verified on Linux with the "strictatime"
         mount flag, so they include all the correct access time updates. WSL
         does not currently implement access time updates.

Arguments:

    Args - Supplies the command line arguments.

    Flags - Supplies the flags.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int AccessTime;
    int Fd;
    char LinkPath[PATH_MAX];
    char* Map;
    char Path[PATH_MAX];
    void* PointerResult;
    int Result;
    struct stat Stat;
    struct stat Stat2;
    struct stat Stat3;
    struct stat Stat4;
    char Temp[PATH_MAX];

    //
    // Since WSL doesn't update access time, only include the access time
    // checks when requested.
    //

    AccessTime = 0;
    if ((Flags & FS_TIMESTAMP_NOATIME) == 0)
    {
        AccessTime = FS_TIMESTAMP_ACCESS;
    }

    snprintf(Path, sizeof(Path), "%s/%s", BaseDir, "test");
    snprintf(LinkPath, sizeof(LinkPath), "%s/%s", BaseDir, "testlink");
    Fd = -1;
    LxtCheckErrnoZeroSuccess(stat(BaseDir, &Stat));

    //
    // Creating and removing items in a directory should update the time of
    // the directory. The items themselves should initialize to all times set
    // to the current time.
    //
    // First create a directory.
    //

    usleep(FS_TIMESTAMP_SLEEP_TIME);
    LxtLogInfo("Create directory...");
    LxtCheckErrnoZeroSuccess(mkdir(Path, 0777));
    LxtCheckResult(LxtFsTimestampCheckUpdate(BaseDir, &Stat, (FS_TIMESTAMP_MODIFY | FS_TIMESTAMP_CHANGE)));

    LxtCheckErrnoZeroSuccess(lstat(Path, &Stat2));
    LxtCheckResult(LxtFsTimestampCheckCurrent(&Stat2.st_atim));
    LxtCheckResult(LxtFsTimestampCheckEqual(&Stat2.st_atim, &Stat2.st_mtim));
    LxtCheckResult(LxtFsTimestampCheckEqual(&Stat2.st_atim, &Stat2.st_ctim));

    usleep(FS_TIMESTAMP_SLEEP_TIME);
    LxtLogInfo("Remove directory...");
    LxtCheckErrnoZeroSuccess(rmdir(Path));
    LxtCheckResult(LxtFsTimestampCheckUpdate(BaseDir, &Stat, (FS_TIMESTAMP_MODIFY | FS_TIMESTAMP_CHANGE)));

    //
    // Create a socket file.
    //

    usleep(FS_TIMESTAMP_SLEEP_TIME);
    LxtLogInfo("Create socket...");
    LxtCheckErrnoZeroSuccess(mknod(Path, S_IFSOCK | 0666, 0));
    LxtCheckResult(LxtFsTimestampCheckUpdate(BaseDir, &Stat, (FS_TIMESTAMP_MODIFY | FS_TIMESTAMP_CHANGE)));

    LxtCheckErrnoZeroSuccess(lstat(Path, &Stat2));
    LxtCheckResult(LxtFsTimestampCheckCurrent(&Stat2.st_atim));
    LxtCheckResult(LxtFsTimestampCheckEqual(&Stat2.st_atim, &Stat2.st_mtim));
    LxtCheckResult(LxtFsTimestampCheckEqual(&Stat2.st_atim, &Stat2.st_ctim));

    usleep(FS_TIMESTAMP_SLEEP_TIME);
    LxtLogInfo("Remove socket...");
    LxtCheckErrnoZeroSuccess(unlink(Path));
    LxtCheckResult(LxtFsTimestampCheckUpdate(BaseDir, &Stat, (FS_TIMESTAMP_MODIFY | FS_TIMESTAMP_CHANGE)));

    //
    // Create a fifo file.
    //

    usleep(FS_TIMESTAMP_SLEEP_TIME);
    LxtLogInfo("Create fifo...");
    LxtCheckErrnoZeroSuccess(mknod(Path, S_IFIFO | 0666, 0));
    LxtCheckResult(LxtFsTimestampCheckUpdate(BaseDir, &Stat, (FS_TIMESTAMP_MODIFY | FS_TIMESTAMP_CHANGE)));

    LxtCheckErrnoZeroSuccess(lstat(Path, &Stat2));
    LxtCheckResult(LxtFsTimestampCheckCurrent(&Stat2.st_atim));
    LxtCheckResult(LxtFsTimestampCheckEqual(&Stat2.st_atim, &Stat2.st_mtim));
    LxtCheckResult(LxtFsTimestampCheckEqual(&Stat2.st_atim, &Stat2.st_ctim));

    usleep(FS_TIMESTAMP_SLEEP_TIME);
    LxtLogInfo("Remove fifo...");
    LxtCheckErrnoZeroSuccess(unlink(Path));
    LxtCheckResult(LxtFsTimestampCheckUpdate(BaseDir, &Stat, (FS_TIMESTAMP_MODIFY | FS_TIMESTAMP_CHANGE)));
    //
    // Create a character device file.
    //

    usleep(FS_TIMESTAMP_SLEEP_TIME);
    LxtLogInfo("Create character device...");
    LxtCheckErrnoZeroSuccess(mknod(Path, S_IFCHR | 0666, 0));
    LxtCheckResult(LxtFsTimestampCheckUpdate(BaseDir, &Stat, (FS_TIMESTAMP_MODIFY | FS_TIMESTAMP_CHANGE)));

    LxtCheckErrnoZeroSuccess(lstat(Path, &Stat2));
    LxtCheckResult(LxtFsTimestampCheckCurrent(&Stat2.st_atim));
    LxtCheckResult(LxtFsTimestampCheckEqual(&Stat2.st_atim, &Stat2.st_mtim));
    LxtCheckResult(LxtFsTimestampCheckEqual(&Stat2.st_atim, &Stat2.st_ctim));

    usleep(FS_TIMESTAMP_SLEEP_TIME);
    LxtLogInfo("Remove character device...");
    LxtCheckErrnoZeroSuccess(unlink(Path));
    LxtCheckResult(LxtFsTimestampCheckUpdate(BaseDir, &Stat, (FS_TIMESTAMP_MODIFY | FS_TIMESTAMP_CHANGE)));

    //
    // Create a file.
    //

    usleep(FS_TIMESTAMP_SLEEP_TIME);
    LxtLogInfo("Create file...");
    LxtCheckErrno(Fd = creat(Path, 0666));
    LxtCheckResult(LxtFsTimestampCheckUpdate(BaseDir, &Stat, (FS_TIMESTAMP_MODIFY | FS_TIMESTAMP_CHANGE)));

    LxtCheckErrnoZeroSuccess(lstat(Path, &Stat2));
    LxtCheckResult(LxtFsTimestampCheckCurrent(&Stat2.st_atim));
    LxtCheckResult(LxtFsTimestampCheckEqual(&Stat2.st_atim, &Stat2.st_mtim));
    LxtCheckResult(LxtFsTimestampCheckEqual(&Stat2.st_atim, &Stat2.st_ctim));
    LxtCheckClose(Fd);
    LxtCheckResult(LxtFsTimestampCheckUpdate(BaseDir, &Stat, 0));
    LxtCheckResult(LxtFsTimestampCheckUpdate(Path, &Stat2, 0));

    //
    // Create a hard link.
    //
    // N.B. The hard link has the same time stamps as the original file, and
    //      ctime gets updated both when the link is created and unlinked.
    //

    usleep(FS_TIMESTAMP_SLEEP_TIME);
    LxtLogInfo("Create hard link...");
    LxtCheckErrnoZeroSuccess(link(Path, LinkPath));
    LxtCheckResult(LxtFsTimestampCheckUpdate(BaseDir, &Stat, (FS_TIMESTAMP_MODIFY | FS_TIMESTAMP_CHANGE)));

    LxtCheckResult(LxtFsTimestampCheckUpdate(LinkPath, &Stat2, FS_TIMESTAMP_CHANGE));

    LxtCheckResult(LxtFsTimestampCheckUpdate(Path, &Stat2, 0));
    usleep(FS_TIMESTAMP_SLEEP_TIME);
    LxtLogInfo("Remove hard link...");
    LxtCheckErrnoZeroSuccess(unlink(LinkPath));
    LxtCheckResult(LxtFsTimestampCheckUpdate(BaseDir, &Stat, (FS_TIMESTAMP_MODIFY | FS_TIMESTAMP_CHANGE)));

    LxtCheckResult(LxtFsTimestampCheckUpdate(Path, &Stat2, FS_TIMESTAMP_CHANGE));

    //
    // Create a symbolic link.
    //

    usleep(FS_TIMESTAMP_SLEEP_TIME);
    LxtLogInfo("Create symlink...");
    LxtCheckErrnoZeroSuccess(symlink(Path, LinkPath));
    LxtCheckResult(LxtFsTimestampCheckUpdate(BaseDir, &Stat, (FS_TIMESTAMP_MODIFY | FS_TIMESTAMP_CHANGE)));

    LxtCheckErrnoZeroSuccess(lstat(LinkPath, &Stat2));
    LxtCheckResult(LxtFsTimestampCheckCurrent(&Stat2.st_atim));
    LxtCheckResult(LxtFsTimestampCheckEqual(&Stat2.st_atim, &Stat2.st_mtim));
    LxtCheckResult(LxtFsTimestampCheckEqual(&Stat2.st_atim, &Stat2.st_ctim));

    //
    // Readlink updates the access time.
    //

    usleep(FS_TIMESTAMP_SLEEP_TIME);
    LxtLogInfo("Readlink...");
    LxtCheckErrno(readlink(LinkPath, Temp, sizeof(Temp)));
    LxtCheckResult(LxtFsTimestampCheckUpdate(LinkPath, &Stat2, AccessTime));
    LxtCheckResult(LxtFsTimestampCheckUpdate(BaseDir, &Stat, 0));

    //
    // Remove the link.
    //

    usleep(FS_TIMESTAMP_SLEEP_TIME);
    LxtLogInfo("Remove symlink...");
    LxtCheckErrnoZeroSuccess(unlink(LinkPath));
    LxtCheckResult(LxtFsTimestampCheckUpdate(BaseDir, &Stat, (FS_TIMESTAMP_MODIFY | FS_TIMESTAMP_CHANGE)));

    //
    // Chmod updates the change time.
    //
    // N.B. For every operation, also check that the timestamp of the parent
    //      directory is not updated.
    //
    // N.B. Timestamps are updated even if the values aren't actually changed.
    //

    LxtCheckErrnoZeroSuccess(lstat(Path, &Stat2));
    usleep(FS_TIMESTAMP_SLEEP_TIME);
    LxtLogInfo("Chmod...");
    LxtCheckErrnoZeroSuccess(chmod(Path, 0600));
    LxtCheckResult(LxtFsTimestampCheckUpdate(Path, &Stat2, FS_TIMESTAMP_CHANGE));
    LxtCheckResult(LxtFsTimestampCheckUpdate(BaseDir, &Stat, 0));
    usleep(FS_TIMESTAMP_SLEEP_TIME);
    LxtCheckErrnoZeroSuccess(chmod(Path, 0600));
    LxtCheckResult(LxtFsTimestampCheckUpdate(Path, &Stat2, FS_TIMESTAMP_CHANGE));
    LxtCheckResult(LxtFsTimestampCheckUpdate(BaseDir, &Stat, 0));

    //
    // Chown updates the change time.
    //

    usleep(FS_TIMESTAMP_SLEEP_TIME);
    LxtLogInfo("Chown...");
    LxtCheckErrnoZeroSuccess(chown(Path, 1000, 1001));
    LxtCheckResult(LxtFsTimestampCheckUpdate(Path, &Stat2, FS_TIMESTAMP_CHANGE));
    LxtCheckResult(LxtFsTimestampCheckUpdate(BaseDir, &Stat, 0));
    usleep(FS_TIMESTAMP_SLEEP_TIME);
    LxtCheckErrnoZeroSuccess(chown(Path, 1000, 1001));
    LxtCheckResult(LxtFsTimestampCheckUpdate(Path, &Stat2, FS_TIMESTAMP_CHANGE));
    LxtCheckResult(LxtFsTimestampCheckUpdate(BaseDir, &Stat, 0));

    //
    // Stat does not change any timestamps (not even atime).
    //
    // N.B. The check function uses stat so just doing that twice is sufficient
    //      to test this.
    //

    usleep(FS_TIMESTAMP_SLEEP_TIME);
    LxtLogInfo("Stat...");
    LxtCheckResult(LxtFsTimestampCheckUpdate(Path, &Stat2, 0));
    LxtCheckResult(LxtFsTimestampCheckUpdate(Path, &Stat2, 0));
    LxtCheckResult(LxtFsTimestampCheckUpdate(BaseDir, &Stat, 0));

    //
    // Setxattr updates the change time.
    //

    usleep(FS_TIMESTAMP_SLEEP_TIME);
    LxtLogInfo("Setxattr...");
    LxtCheckErrnoZeroSuccess(setxattr(Path, "user.test", "value", 5, XATTR_CREATE));

    LxtCheckResult(LxtFsTimestampCheckUpdate(Path, &Stat2, FS_TIMESTAMP_CHANGE));
    LxtCheckResult(LxtFsTimestampCheckUpdate(BaseDir, &Stat, 0));
    usleep(FS_TIMESTAMP_SLEEP_TIME);
    LxtCheckErrnoZeroSuccess(setxattr(Path, "user.test", "value2", 6, XATTR_REPLACE));

    LxtCheckResult(LxtFsTimestampCheckUpdate(Path, &Stat2, FS_TIMESTAMP_CHANGE));
    LxtCheckResult(LxtFsTimestampCheckUpdate(BaseDir, &Stat, 0));

    //
    // Getxattr does not change any timestamps (not even atime).
    //

    usleep(FS_TIMESTAMP_SLEEP_TIME);
    LxtLogInfo("Getxattr...");
    LxtCheckErrno(getxattr(Path, "user.test", Temp, sizeof(Temp)));
    LxtCheckResult(LxtFsTimestampCheckUpdate(Path, &Stat2, 0));
    LxtCheckResult(LxtFsTimestampCheckUpdate(BaseDir, &Stat, 0));

    //
    // listxattr does not change any timestamps (not even atime).
    //

    usleep(FS_TIMESTAMP_SLEEP_TIME);
    LxtLogInfo("Listxattr...");
    LxtCheckErrno(listxattr(Path, Temp, sizeof(Temp)));
    LxtCheckResult(LxtFsTimestampCheckUpdate(Path, &Stat2, 0));
    LxtCheckResult(LxtFsTimestampCheckUpdate(BaseDir, &Stat, 0));

    //
    // Removexattr updates the change time.
    //

    usleep(FS_TIMESTAMP_SLEEP_TIME);
    LxtLogInfo("Removexattr...");
    LxtCheckErrno(removexattr(Path, "user.test"));
    LxtCheckResult(LxtFsTimestampCheckUpdate(Path, &Stat2, FS_TIMESTAMP_CHANGE));
    LxtCheckResult(LxtFsTimestampCheckUpdate(BaseDir, &Stat, 0));

    //
    // Rename to the same directory (reusing LinkPath for this since it doesn't
    // exist at this point). This change the parent and target timestamps.
    //

    usleep(FS_TIMESTAMP_SLEEP_TIME);
    LxtLogInfo("Rename (same directory)...");
    LxtCheckErrnoZeroSuccess(rename(Path, LinkPath));
    LxtCheckResult(LxtFsTimestampCheckUpdate(LinkPath, &Stat2, FS_TIMESTAMP_CHANGE));
    LxtCheckResult(LxtFsTimestampCheckUpdate(BaseDir, &Stat, (FS_TIMESTAMP_MODIFY | FS_TIMESTAMP_CHANGE)));

    //
    // Rename to a different directory.
    //

    snprintf(Temp, sizeof(Temp), "%s/%s", BaseDir, "targetdir");
    LxtCheckErrnoZeroSuccess(mkdir(Temp, 0777));
    LxtCheckErrnoZeroSuccess(lstat(Temp, &Stat3));
    usleep(FS_TIMESTAMP_SLEEP_TIME);
    LxtLogInfo("Rename (different directory)...");
    snprintf(Temp, sizeof(Temp), "%s/%s", BaseDir, "targetdir/target");
    LxtCheckErrnoZeroSuccess(rename(LinkPath, Temp));
    LxtCheckResult(LxtFsTimestampCheckUpdate(Temp, &Stat2, FS_TIMESTAMP_CHANGE));
    LxtCheckResult(LxtFsTimestampCheckUpdate(BaseDir, &Stat, (FS_TIMESTAMP_MODIFY | FS_TIMESTAMP_CHANGE)));

    snprintf(Temp, sizeof(Temp), "%s/%s", BaseDir, "targetdir");
    LxtCheckResult(LxtFsTimestampCheckUpdate(Temp, &Stat3, (FS_TIMESTAMP_MODIFY | FS_TIMESTAMP_CHANGE)));

    //
    // Create a file to overwrite with a rename, and create a link to that
    // file. The overwrite should count as an unlink, and therefore update the
    // change time.
    //

    LxtCheckErrno(Fd = creat(Path, 0666));
    LxtCheckClose(Fd);
    LxtCheckErrnoZeroSuccess(link(Path, LinkPath));
    LxtCheckErrnoZeroSuccess(lstat(Path, &Stat4));
    LxtCheckEqual(Stat4.st_nlink, 2, "%d");
    usleep(FS_TIMESTAMP_SLEEP_TIME);
    LxtLogInfo("Rename (overwrite target)...");
    snprintf(Temp, sizeof(Temp), "%s/%s", BaseDir, "targetdir/target");
    LxtCheckErrnoZeroSuccess(rename(Temp, Path));
    LxtCheckResult(LxtFsTimestampCheckUpdate(Path, &Stat2, FS_TIMESTAMP_CHANGE));
    LxtCheckResult(LxtFsTimestampCheckUpdate(BaseDir, &Stat, (FS_TIMESTAMP_MODIFY | FS_TIMESTAMP_CHANGE)));

    snprintf(Temp, sizeof(Temp), "%s/%s", BaseDir, "targetdir");
    LxtCheckResult(LxtFsTimestampCheckUpdate(Temp, &Stat3, (FS_TIMESTAMP_MODIFY | FS_TIMESTAMP_CHANGE)));

    LxtCheckResult(LxtFsTimestampCheckUpdate(LinkPath, &Stat4, FS_TIMESTAMP_CHANGE));

    LxtCheckEqual(Stat4.st_nlink, 1, "%d");

    //
    // Open this file (which by itself doesn't update any timestamps).
    //

    usleep(FS_TIMESTAMP_SLEEP_TIME);
    LxtLogInfo("Open...");
    LxtCheckErrno(Fd = open(Path, O_RDWR));
    LxtCheckResult(LxtFsTimestampCheckUpdate(Path, &Stat2, 0));
    LxtCheckResult(LxtFsTimestampCheckUpdate(BaseDir, &Stat, 0));

    //
    // Write updates the mtime and ctime (unless no data is written).
    //
    // N.B. This happens regardless of whether the write extends the file or
    //      not.
    //

    usleep(FS_TIMESTAMP_SLEEP_TIME);
    LxtLogInfo("Write...");
    LxtCheckErrno(write(Fd, "test\0", 5));
    LxtCheckResult(LxtFsTimestampCheckUpdate(Path, &Stat2, (FS_TIMESTAMP_MODIFY | FS_TIMESTAMP_CHANGE)));

    LxtCheckResult(LxtFsTimestampCheckUpdate(BaseDir, &Stat, 0));
    usleep(FS_TIMESTAMP_SLEEP_TIME);
    LxtCheckErrno(write(Fd, "", 0));
    LxtCheckResult(LxtFsTimestampCheckUpdate(Path, &Stat2, 0));
    LxtCheckResult(LxtFsTimestampCheckUpdate(BaseDir, &Stat, 0));

    //
    // Sync doesn't update any timestamps.
    //

    usleep(FS_TIMESTAMP_SLEEP_TIME);
    LxtLogInfo("Sync...");
    LxtCheckErrnoZeroSuccess(fsync(Fd));
    LxtCheckResult(LxtFsTimestampCheckUpdate(Path, &Stat2, 0));
    LxtCheckResult(LxtFsTimestampCheckUpdate(BaseDir, &Stat, 0));

    //
    // Seek doesn't update any timestamps.
    //

    usleep(FS_TIMESTAMP_SLEEP_TIME);
    LxtLogInfo("Seek...");
    LxtCheckErrno(lseek(Fd, 0, SEEK_SET));
    LxtCheckResult(LxtFsTimestampCheckUpdate(Path, &Stat2, 0));
    LxtCheckResult(LxtFsTimestampCheckUpdate(BaseDir, &Stat, 0));

    //
    // Read updates the atime (even if no data is read, even with a zero size
    // buffer).
    //

    usleep(FS_TIMESTAMP_SLEEP_TIME);
    LxtLogInfo("Read...");
    LxtCheckErrno(read(Fd, Temp, 2));
    LxtCheckResult(LxtFsTimestampCheckUpdate(Path, &Stat2, AccessTime));
    LxtCheckResult(LxtFsTimestampCheckUpdate(BaseDir, &Stat, 0));
    usleep(FS_TIMESTAMP_SLEEP_TIME);
    LxtCheckErrno(read(Fd, Temp, sizeof(Temp)));
    LxtCheckResult(LxtFsTimestampCheckUpdate(Path, &Stat2, AccessTime));
    LxtCheckResult(LxtFsTimestampCheckUpdate(BaseDir, &Stat, 0));
    usleep(FS_TIMESTAMP_SLEEP_TIME);
    LxtCheckErrnoZeroSuccess(read(Fd, Temp, sizeof(Temp)));
    LxtCheckResult(LxtFsTimestampCheckUpdate(Path, &Stat2, AccessTime));
    LxtCheckResult(LxtFsTimestampCheckUpdate(BaseDir, &Stat, 0));
    usleep(FS_TIMESTAMP_SLEEP_TIME);
    LxtCheckErrno(lseek(Fd, 0, SEEK_SET));
    LxtCheckErrnoZeroSuccess(read(Fd, Temp, 0));
    LxtCheckResult(LxtFsTimestampCheckUpdate(Path, &Stat2, AccessTime));
    LxtCheckResult(LxtFsTimestampCheckUpdate(BaseDir, &Stat, 0));

    //
    // This is the only ioctl supported by LxFs; it doesn't update any
    // timestamps.
    //

    LxtCheckErrno(lseek(Fd, 0, SEEK_SET));
    usleep(FS_TIMESTAMP_SLEEP_TIME);
    LxtLogInfo("Ioctl (FIONREAD)...");
    LxtCheckErrnoZeroSuccess(ioctl(Fd, FIONREAD, Temp));
    LxtCheckResult(LxtFsTimestampCheckUpdate(Path, &Stat2, 0));
    LxtCheckResult(LxtFsTimestampCheckUpdate(BaseDir, &Stat, 0));

    //
    // Mmap updates the access time immediately regardless of protection or
    // other flags. The write/modify time is updated when the mapping is first
    // written to, which WSL currently does not do correctly.
    //

    usleep(FS_TIMESTAMP_SLEEP_TIME);
    LxtLogInfo("Mmap...");
    LxtCheckNullErrno(Map = mmap(NULL, 5, PROT_NONE, MAP_PRIVATE, Fd, 0));

    LxtCheckResult(LxtFsTimestampCheckUpdate(Path, &Stat2, AccessTime));
    LxtCheckResult(LxtFsTimestampCheckUpdate(BaseDir, &Stat, 0));
    usleep(FS_TIMESTAMP_SLEEP_TIME);
    LxtCheckErrnoZeroSuccess(munmap(Map, 5));
    LxtCheckResult(LxtFsTimestampCheckUpdate(Path, &Stat2, 0));
    LxtCheckResult(LxtFsTimestampCheckUpdate(BaseDir, &Stat, 0));
    usleep(FS_TIMESTAMP_SLEEP_TIME);
    LxtCheckNullErrno(Map = mmap(NULL, 5, PROT_READ | PROT_WRITE, MAP_SHARED, Fd, 0));

    LxtCheckResult(LxtFsTimestampCheckUpdate(Path, &Stat2, AccessTime));
    LxtCheckResult(LxtFsTimestampCheckUpdate(BaseDir, &Stat, 0));

    usleep(FS_TIMESTAMP_SLEEP_TIME);
    LxtLogInfo("Value = %s", Map);
    LxtCheckResult(LxtFsTimestampCheckUpdate(Path, &Stat2, 0));
    LxtCheckResult(LxtFsTimestampCheckUpdate(BaseDir, &Stat, 0));
    usleep(FS_TIMESTAMP_SLEEP_TIME);
    memcpy(Map, "1234", 4);

    //
    // TODO_LX: Enable this check one WSL correctly does write timestamps for
    //          mapped files.
    //

    // LxtCheckResult(LxtFsTimestampCheckUpdate(Path,
    //                                         &Stat2,
    //                                         (FS_TIMESTAMP_CHANGE |
    //                                          FS_TIMESTAMP_MODIFY)));

    LxtCheckResult(LxtFsTimestampCheckUpdate(BaseDir, &Stat, 0));
    usleep(FS_TIMESTAMP_SLEEP_TIME);
    memcpy(Map, "abcd", 4);
    LxtCheckResult(LxtFsTimestampCheckUpdate(Path, &Stat2, 0));
    LxtCheckResult(LxtFsTimestampCheckUpdate(BaseDir, &Stat, 0));
    usleep(FS_TIMESTAMP_SLEEP_TIME);
    LxtCheckErrnoZeroSuccess(msync(Map, 5, MS_SYNC));
    LxtCheckResult(LxtFsTimestampCheckUpdate(Path, &Stat2, 0));
    LxtCheckResult(LxtFsTimestampCheckUpdate(BaseDir, &Stat, 0));
    usleep(FS_TIMESTAMP_SLEEP_TIME);
    LxtCheckErrnoZeroSuccess(munmap(Map, 5));
    LxtCheckResult(LxtFsTimestampCheckUpdate(Path, &Stat2, 0));
    LxtCheckResult(LxtFsTimestampCheckUpdate(BaseDir, &Stat, 0));

    //
    // Fallocate updates change time, and write time only if the file's length
    // is changed.
    //

    usleep(FS_TIMESTAMP_SLEEP_TIME);
    LxtLogInfo("Fallocate...");
    LxtCheckErrnoZeroSuccess(fallocate(Fd, 0, 0, 1024));
    LxtCheckResult(LxtFsTimestampCheckUpdate(Path, &Stat2, (FS_TIMESTAMP_MODIFY | FS_TIMESTAMP_CHANGE)));

    LxtCheckResult(LxtFsTimestampCheckUpdate(BaseDir, &Stat, 0));
    usleep(FS_TIMESTAMP_SLEEP_TIME);
    LxtCheckErrnoZeroSuccess(fallocate(Fd, 0, 0, 1024));
    LxtCheckResult(LxtFsTimestampCheckUpdate(Path, &Stat2, FS_TIMESTAMP_CHANGE));
    LxtCheckResult(LxtFsTimestampCheckUpdate(BaseDir, &Stat, 0));
    usleep(FS_TIMESTAMP_SLEEP_TIME);
    LxtCheckErrnoZeroSuccess(fallocate(Fd, FALLOC_FL_KEEP_SIZE, 0, 2048));
    LxtCheckResult(LxtFsTimestampCheckUpdate(Path, &Stat2, FS_TIMESTAMP_CHANGE));
    LxtCheckResult(LxtFsTimestampCheckUpdate(BaseDir, &Stat, 0));
    LxtCheckClose(Fd);

    //
    // Truncate updates modify time and change time, even if the size did
    // not change.
    //

    usleep(FS_TIMESTAMP_SLEEP_TIME);
    LxtLogInfo("Truncate...");
    LxtCheckErrnoZeroSuccess(truncate(Path, 2));
    LxtCheckResult(LxtFsTimestampCheckUpdate(Path, &Stat2, (FS_TIMESTAMP_MODIFY | FS_TIMESTAMP_CHANGE)));

    LxtCheckResult(LxtFsTimestampCheckUpdate(BaseDir, &Stat, 0));
    usleep(FS_TIMESTAMP_SLEEP_TIME);
    LxtCheckErrnoZeroSuccess(truncate(Path, 2));
    LxtCheckResult(LxtFsTimestampCheckUpdate(Path, &Stat2, (FS_TIMESTAMP_MODIFY | FS_TIMESTAMP_CHANGE)));

    LxtCheckResult(LxtFsTimestampCheckUpdate(BaseDir, &Stat, 0));

    //
    // Opening with O_TRUNC updates modify time and change time, even if the
    // size does not change.
    //

    usleep(FS_TIMESTAMP_SLEEP_TIME);
    LxtLogInfo("Open (O_TRUNC)...");
    LxtCheckErrno(Fd = open(Path, O_RDWR | O_TRUNC));
    LxtCheckResult(LxtFsTimestampCheckUpdate(Path, &Stat2, (FS_TIMESTAMP_MODIFY | FS_TIMESTAMP_CHANGE)));

    LxtCheckResult(LxtFsTimestampCheckUpdate(BaseDir, &Stat, 0));
    LxtCheckClose(Fd);
    usleep(FS_TIMESTAMP_SLEEP_TIME);
    LxtCheckErrno(Fd = open(Path, O_RDWR | O_TRUNC));
    LxtCheckResult(LxtFsTimestampCheckUpdate(Path, &Stat2, (FS_TIMESTAMP_MODIFY | FS_TIMESTAMP_CHANGE)));

    LxtCheckResult(LxtFsTimestampCheckUpdate(BaseDir, &Stat, 0));
    LxtCheckClose(Fd);

    //
    // Unlink the file.
    //

    usleep(FS_TIMESTAMP_SLEEP_TIME);
    LxtLogInfo("Remove file...");
    LxtCheckErrnoZeroSuccess(unlink(LinkPath));
    LxtCheckResult(LxtFsTimestampCheckUpdate(BaseDir, &Stat, (FS_TIMESTAMP_MODIFY | FS_TIMESTAMP_CHANGE)));

    //
    // Note that:
    // - utimensat updates the change time to the current time; this is test
    //   by the utime test.
    //

ErrorExit:
    if (Fd >= 0)
    {
        close(Fd);
    }

    snprintf(Temp, sizeof(Temp), "%s/%s", BaseDir, "targetdir/target");
    unlink(Temp);
    snprintf(Temp, sizeof(Temp), "%s/%s", BaseDir, "targetdir");
    rmdir(Temp);
    rmdir(Path);
    unlink(Path);
    unlink(LinkPath);
    return Result;
}

long long LxtFsTimestampDiff(const struct timespec* Timestamp1, const struct timespec* Timestamp2)

/*++

Description:

    This routine gets the difference between two timestamps, in nanoseconds.

Arguments:

    Timestamp1 - Supplies the first timestamp.

    Timestamp2 - Supplies the second timestamp.

Return Value:

    The value of Timestamp1 - Timestamp2, in nanoseconds

--*/

{

    int Result;
    long long TimestampNs1;
    long long TimestampNs2;

    TimestampNs1 = (Timestamp1->tv_sec * FS_NS_PER_SEC) + Timestamp1->tv_nsec;
    TimestampNs2 = (Timestamp2->tv_sec * FS_NS_PER_SEC) + Timestamp2->tv_nsec;
    return TimestampNs1 - TimestampNs2;
}

int LxtFsUtimeBasicCommon(const char* BaseDir, int Flags)

/*++

Routine Description:

    This routine executes basic test functions, including setting timestamps
    to a range of values including UTIME_NOW or UTIME_OMIT, on a range of
    different ways to specify the target file, including relative, absolute,
    descriptor, via symbolic link, on a symbolic link, and validating that
    the expected outcome occurs.

Arguments:

    BaseDir - Supplies the directory containing the test files.

    Flags - Supplies the flags.

Return Value:

    0 if all variations complete successfully, -1 if they do not.

--*/

{

    int AllowedAccessVariance;
    int AllowedChangeVariance;
    int AllowedModifiedVariance;
    char ChildFileFullPath[100];
    PBASIC_TEST_CASE CurrentTest;
    struct timespec CurrentTime;
    int DirFd;
    struct timespec ExpectedAccessTime;
    struct timespec ExpectedChangeTime;
    struct timespec ExpectedModifiedTime;
    char LinkFullPath[100];
    int NameVariation;
    int NameVariationLast;
    struct stat NoChangeStatBufferOld;
    struct stat NoChangeStatBufferNew;
    int Result;
    const char* SetFilename;
    int SetFlags;
    struct timespec* SetTime;
    struct stat StatBuffer;
    unsigned int TestCase;
    const char* ValidateFilename;
    const char* ValidateNoChangeFilename;

    //
    // Initialize locals.
    //

    DirFd = -1;
    Result = 0;
    SetFlags = 0;
    ValidateNoChangeFilename = NULL;
    snprintf(ChildFileFullPath, sizeof(ChildFileFullPath), "%s/%s", BaseDir, FS_UTIME_TESTFILE);

    snprintf(LinkFullPath, sizeof(LinkFullPath), "%s/%s", BaseDir, FS_UTIME_TESTLINK);

    //
    // Sleep for 2 seconds to make sure that the initial ctime of the test file
    // wouldn't pass the current time test if utimensat doesn't update it.
    //

    LxtLogInfo("Sleeping...");
    sleep(3);

    //
    // Try different combinations of specifying file names.
    //

    NameVariationLast = NameVariationMax;
    if ((Flags & FS_UTIME_NO_SYMLINKS) != 0)
    {
        NameVariationLast = NameVariationFatMax;
    }

    for (NameVariation = 0; NameVariation <= NameVariationLast; NameVariation += 1)
    {
        DirFd = -1;
        switch (NameVariation)
        {
        case NameVariationFullName:
            SetFilename = ChildFileFullPath;
            SetFlags = 0;
            ValidateFilename = ChildFileFullPath;
            ValidateNoChangeFilename = LinkFullPath;
            break;
        case NameVariationCwdRelative:
            LxtCheckErrnoZeroSuccess(chdir(BaseDir));
            DirFd = AT_FDCWD;
            SetFilename = FS_UTIME_TESTFILE;
            SetFlags = 0;
            ValidateFilename = ChildFileFullPath;
            ValidateNoChangeFilename = LinkFullPath;
            break;
        case NameVariationRelative:
            LxtCheckErrno(DirFd = open(BaseDir, O_DIRECTORY | O_RDONLY, 0));
            SetFilename = FS_UTIME_TESTFILE;
            SetFlags = 0;
            ValidateFilename = ChildFileFullPath;
            ValidateNoChangeFilename = LinkFullPath;
            break;
        case NameVariationDescriptor:
            LxtCheckErrno(DirFd = open(ChildFileFullPath, O_RDWR, 0));
            SetFilename = NULL;
            SetFlags = 0;
            ValidateFilename = ChildFileFullPath;
            ValidateNoChangeFilename = LinkFullPath;
            break;
        case NameVariationFullFileViaLink:
            SetFilename = LinkFullPath;
            SetFlags = 0;
            ValidateFilename = ChildFileFullPath;
            ValidateNoChangeFilename = LinkFullPath;
            break;
        case NameVariationCwdRelativeViaLink:
            LxtCheckErrnoZeroSuccess(chdir(BaseDir));
            DirFd = AT_FDCWD;
            SetFilename = FS_UTIME_TESTLINK;
            SetFlags = 0;
            ValidateFilename = ChildFileFullPath;
            ValidateNoChangeFilename = LinkFullPath;
            break;
        case NameVariationRelativeViaLink:
            LxtCheckErrno(DirFd = open(BaseDir, O_DIRECTORY | O_RDONLY, 0));
            SetFilename = FS_UTIME_TESTLINK;
            SetFlags = 0;
            ValidateFilename = ChildFileFullPath;
            ValidateNoChangeFilename = LinkFullPath;
            break;
        case NameVariationDescriptorViaLink:
            LxtCheckErrno(DirFd = open(LinkFullPath, O_RDWR, 0));
            SetFilename = NULL;
            SetFlags = 0;
            ValidateFilename = ChildFileFullPath;
            ValidateNoChangeFilename = LinkFullPath;
            break;
        case NameVariationFullFileOnLink:
            SetFilename = LinkFullPath;
            SetFlags = AT_SYMLINK_NOFOLLOW;
            ValidateFilename = LinkFullPath;
            ValidateNoChangeFilename = ChildFileFullPath;
            break;
        case NameVariationCwdRelativeOnLink:
            LxtCheckErrnoZeroSuccess(chdir(BaseDir));
            DirFd = AT_FDCWD;
            SetFilename = FS_UTIME_TESTLINK;
            SetFlags = AT_SYMLINK_NOFOLLOW;
            ValidateFilename = LinkFullPath;
            ValidateNoChangeFilename = ChildFileFullPath;
            break;
        case NameVariationRelativeOnLink:
            LxtCheckErrno(DirFd = open(BaseDir, O_DIRECTORY | O_RDONLY, 0));
            SetFilename = FS_UTIME_TESTLINK;
            SetFlags = AT_SYMLINK_NOFOLLOW;
            ValidateFilename = LinkFullPath;
            ValidateNoChangeFilename = ChildFileFullPath;
            break;
        }

        LxtLogInfo(
            "Name variation %i, SetFilename = %s, ValidateFileName = %s,"
            " ValidateNoChangeFileName = %s",
            NameVariation,
            SetFilename,
            ValidateFilename,
            ValidateNoChangeFilename);

        //
        // Check that different times of timestamp operations can succeed.
        //

        for (TestCase = 0; TestCase < LXT_COUNT_OF(BasicTestCases); TestCase += 1)
        {
            LxtLogInfo("Test case %i", TestCase);

            CurrentTest = &BasicTestCases[TestCase];
            SetTime = CurrentTest->SetTime;
            if (SetTime[0].tv_nsec == UTIME_NOW && SetTime[1].tv_nsec == UTIME_NOW && SetTime[0].tv_sec == 0 && SetTime[1].tv_sec == 0)
            {
                SetTime = NULL;
            }

            if ((Flags & FS_UTIME_NO_SYMLINKS) == 0)
            {
                LxtCheckErrnoZeroSuccess(lstat(ValidateNoChangeFilename, &NoChangeStatBufferOld));
            }

            if (SetFilename != NULL)
            {
                LxtCheckErrnoZeroSuccess(utimensat(DirFd, SetFilename, SetTime, SetFlags));
            }
            else
            {
                LxtCheckErrnoZeroSuccess(futimens(DirFd, SetTime));
            }

            LxtCheckErrnoZeroSuccess(lstat(ValidateFilename, &StatBuffer));
            LxtCheckErrnoZeroSuccess(clock_gettime(CLOCK_REALTIME_COARSE, &CurrentTime));

            ExpectedAccessTime = CurrentTest->ExpectTime[0];
            AllowedAccessVariance = 0;
            ExpectedModifiedTime = CurrentTest->ExpectTime[1];
            AllowedModifiedVariance = 0;
            ExpectedChangeTime = CurrentTime;
            AllowedChangeVariance = FS_FAT_MODIFIED_TIME_PRECISION;
            if (ExpectedAccessTime.tv_nsec == UTIME_NOW)
            {
                ExpectedAccessTime = CurrentTime;
                AllowedAccessVariance = FS_FAT_MODIFIED_TIME_PRECISION;
            }

            if (ExpectedModifiedTime.tv_nsec == UTIME_NOW)
            {
                ExpectedModifiedTime = CurrentTime;
                AllowedModifiedVariance = FS_FAT_MODIFIED_TIME_PRECISION;
            }

            if ((Flags & FS_UTIME_FAT) != 0)
            {
                LxtFsUtimeRoundToFatAccessTime(&ExpectedAccessTime);
                LxtFsUtimeRoundToFatModifiedTime(&ExpectedModifiedTime);

                //
                // Since FAT32 doesn't support change time, WSL reports the
                // same value as the modified time.
                //

                ExpectedChangeTime = ExpectedModifiedTime;
                AllowedChangeVariance = AllowedModifiedVariance;
            }
            else if ((Flags & FS_UTIME_NT_PRECISION) != 0)
            {
                LxtFsUtimeRoundToNt(&ExpectedAccessTime);
                LxtFsUtimeRoundToNt(&ExpectedModifiedTime);
                LxtFsUtimeRoundToNt(&ExpectedChangeTime);
            }

            //
            // Calling utime causes the ctime to be set to the current time,
            // so test that too.
            //

            if ((LxtFsUtimeDoTimesMatch(&StatBuffer.st_atim, &ExpectedAccessTime, AllowedAccessVariance) == false) ||
                (LxtFsUtimeDoTimesMatch(&StatBuffer.st_mtim, &ExpectedModifiedTime, AllowedModifiedVariance) == false) ||
                (LxtFsUtimeDoTimesMatch(&StatBuffer.st_ctim, &ExpectedChangeTime, AllowedChangeVariance) == false))
            {

                LxtLogError(
                    "times do not match expected values for file %s, "
                    "TestCase %i, NameVariation %i",
                    ValidateFilename,
                    TestCase,
                    NameVariation);

                LxtLogError(
                    "atime set: %lld.%.9ld, expected: %lld.%.9ld%s, "
                    "actual: %lld.%.9ld",
                    CurrentTest->SetTime[0].tv_sec,
                    CurrentTest->SetTime[0].tv_nsec,
                    ExpectedAccessTime.tv_sec,
                    ExpectedAccessTime.tv_nsec,
                    (CurrentTest->ExpectTime[0].tv_nsec == UTIME_NOW ? " (UTIME_NOW)" : ""),
                    StatBuffer.st_atim.tv_sec,
                    StatBuffer.st_atim.tv_nsec);

                LxtLogError(
                    "mtime set: %lld.%.9ld, expected: %lld.%.9ld%s, "
                    "actual: %lld.%.9ld",
                    CurrentTest->SetTime[1].tv_sec,
                    CurrentTest->SetTime[1].tv_nsec,
                    ExpectedModifiedTime.tv_sec,
                    ExpectedModifiedTime.tv_nsec,
                    (CurrentTest->ExpectTime[1].tv_nsec == UTIME_NOW ? " (UTIME_NOW)" : ""),
                    StatBuffer.st_mtim.tv_sec,
                    StatBuffer.st_mtim.tv_nsec);

                LxtLogError(
                    "ctime expected: %lld.%.9ld (UTIME_NOW), actual: %lld.%.9ld",
                    ExpectedChangeTime.tv_sec,
                    ExpectedChangeTime.tv_nsec,
                    StatBuffer.st_ctim.tv_sec,
                    StatBuffer.st_ctim.tv_nsec);

                Result = -1;
                goto ErrorExit;
            }

            if ((Flags & FS_UTIME_NO_SYMLINKS) == 0)
            {
                LxtCheckErrnoZeroSuccess(lstat(ValidateNoChangeFilename, &NoChangeStatBufferNew));

                //
                // This sometimes fails on real Linux since the access timestamp
                // is updated when a link is read. With the default relatime mount
                // option, this only happens the first time, since after that
                // atime > mtime and won't be updated.
                //
                // TODO_LX: This test should probably be revisited when proper
                //          atime updating happens on WSL.
                //

                LxtCheckMemoryEqual(&NoChangeStatBufferOld, &NoChangeStatBufferNew, sizeof(NoChangeStatBufferOld));
            }
        }

        if (DirFd >= 0)
        {
            LxtCheckClose(DirFd);
        }
    }

ErrorExit:
    if (DirFd >= 0)
    {
        close(DirFd);
    }

    return Result;
}

void LxtFsUtimeCleanupTestFiles(const char* BaseDir)

/*++

Description:

    This routine cleans up the utime test files.

Arguments:

    BaseDir - Supplies the directory containing the test files.

Return Value:

    None.

--*/

{

    char FilePath[100];
    char LinkPath[100];

    snprintf(FilePath, sizeof(FilePath), "%s/%s", BaseDir, FS_UTIME_TESTFILE);
    unlink(FilePath);
    snprintf(LinkPath, sizeof(LinkPath), "%s/%s", BaseDir, FS_UTIME_TESTLINK);
    unlink(LinkPath);
    rmdir(BaseDir);

ErrorExit:
    return;
}

int LxtFsUtimeCreateTestFiles(const char* BaseDir, int Flags)

/*++

Description:

    This routine creates test files for the utime tests.

Arguments:

    BaseDir - Supplies the directory containing the test files.

    Flags - Supplies the flags.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int Fd;
    char FilePath[100];
    char LinkPath[100];
    int Result;

    //
    // Make a directory, test file, and test link.
    //

    LxtCheckErrnoZeroSuccess(mkdir(BaseDir, 0777));
    LxtCheckResult(LxtFsGetFsInfo(BaseDir, &g_LxtFsInfo));
    snprintf(FilePath, sizeof(FilePath), "%s/%s", BaseDir, FS_UTIME_TESTFILE);
    LxtCheckErrno(Fd = creat(FilePath, 0666));
    LxtCheckClose(Fd);
    if ((Flags & FS_UTIME_NO_SYMLINKS) == 0)
    {
        snprintf(LinkPath, sizeof(LinkPath), "%s/%s", BaseDir, FS_UTIME_TESTLINK);
        LxtCheckErrnoZeroSuccess(symlink(FilePath, LinkPath));
    }

ErrorExit:
    return Result;
}

bool LxtFsUtimeDoTimesMatch(const struct timespec* Actual, const struct timespec* Expected, int AllowedVarianceSeconds)

/*++

Routine Description:

    This routine compares two timespec structures for equality.

Arguments:

    Actual - Supplies a pointer to the times encountered on the file.

    Expected - Supplies a pointer to the times expected on the file.

    AllowedVarianceSeconds - The amount of seconds that the actual time may
        be behind the expected time.

Return Value:

    TRUE if the timestamps match, FALSE if they do not.

--*/

{

    unsigned long long FullExpectedTime;
    unsigned long long FullTime;

    FullTime = (Actual->tv_sec * FS_NS_PER_SEC) + Actual->tv_nsec;
    FullExpectedTime = (Expected->tv_sec * FS_NS_PER_SEC) + Expected->tv_nsec;
    if ((FullTime <= FullExpectedTime) && (FullTime >= (FullExpectedTime - (AllowedVarianceSeconds * FS_NS_PER_SEC))))
    {

        return true;
    }

    //
    // In VM mode, also allow variance to the future to allow for clock skew between
    // the host and guest.
    //

    if ((g_LxtFsInfo.FsType == LxtFsTypePlan9) && (FullTime <= (FullExpectedTime + (AllowedVarianceSeconds * FS_NS_PER_SEC))))
    {

        return true;
    }

    LxtLogError(
        "Time %lld.%.9ld not within %ds window of expected time %lld.%.9ld",
        Actual->tv_sec,
        Actual->tv_nsec,
        AllowedVarianceSeconds,
        Expected->tv_sec,
        Expected->tv_nsec);

    return false;
}

void LxtFsUtimeRoundToFatAccessTime(struct timespec* Timespec)

/*++

Description:

    This routine rounds a timespec to the nearest day, which is what FAT32
    uses for the last access time.

Arguments:

    Timespec - Supplies the time value.

Return Value:

    None.

--*/

{

    struct timespec Now;
    time_t Time;
    struct tm TimeInfo;

    //
    // When using the current time, there is a slight chance of this test
    // failing when the time set for the file and the time when the check
    // is done straddle midnight.
    //

    clock_gettime(CLOCK_REALTIME_COARSE, &Now);
    if (Timespec->tv_nsec == UTIME_NOW)
    {
        *Timespec = Now;
    }

    //
    // FAT32 stores only the day for the last access time, and it stores the
    // local time. Here's what happens:
    // - When the file time is set, Windows converts it to a local time. It
    //   uses the current DST setting, not the DST setting that was active at
    //   the specified time.
    // - The date portion of the local time is stored as the file time.
    // - When reading the time, Windows converts it back to UTC. Again, this is
    //   done using the current DST setting.
    // - This means the returned time looks like 2017-06-26 17:00:00, based on
    //   the UTC offset of the current timezone.
    //
    // Determine the UTC offset of the current time, which ensures we use the
    // current DST value.
    //

    Time = Now.tv_sec;
    localtime_r(&Time, &TimeInfo);

    //
    // Convert the specified time to local time, and round to the nearest day.
    //

    Timespec->tv_sec += TimeInfo.tm_gmtoff;
    Timespec->tv_sec /= FS_SECONDS_PER_DAY;
    Timespec->tv_sec *= FS_SECONDS_PER_DAY;

    //
    // Convert back to UTC.
    //

    Timespec->tv_sec -= TimeInfo.tm_gmtoff;

    //
    // Since FAT only stores the day, nsec is always zero.
    //

    Timespec->tv_nsec = 0;
    return;
}

void LxtFsUtimeRoundToFatModifiedTime(struct timespec* Timespec)

/*++

Description:

    This routine rounds a timespec to the next highest multiple of two seconds,
    which FAT does for the last write time.

Arguments:

    Timespec - Supplies the time value.

Return Value:

    None.

--*/

{

    time_t Time;
    struct tm TimeInfo;

    if (Timespec->tv_nsec == UTIME_NOW)
    {
        clock_gettime(CLOCK_REALTIME_COARSE, Timespec);
    }

    if (((Timespec->tv_sec % FS_FAT_MODIFIED_TIME_PRECISION) != 0) || (Timespec->tv_nsec != 0))
    {

        Timespec->tv_sec += FS_FAT_MODIFIED_TIME_PRECISION;
    }

    Timespec->tv_nsec = 0;
    Timespec->tv_sec /= FS_FAT_MODIFIED_TIME_PRECISION;
    Timespec->tv_sec *= FS_FAT_MODIFIED_TIME_PRECISION;
    return;
}

void LxtFsUtimeRoundToNt(struct timespec* Timespec)

/*++

Description:

    This routine rounds a timespec to NT precision.

Arguments:

    Timespec - Supplies the time value.

Return Value:

    None.

--*/

{

    //
    // WSL rounds up.
    //

    if (Timespec->tv_nsec != UTIME_NOW)
    {
        Timespec->tv_nsec += (FS_NS_PER_NT_UNIT - 1);
        Timespec->tv_nsec /= FS_NS_PER_NT_UNIT;
        Timespec->tv_nsec *= FS_NS_PER_NT_UNIT;
    }

    return;
}

int LxtFsWritevCommon(char* TestFile)

/*++

Description:

    This routine tests the writev system call.

Arguments:

    Args - Supplies the test arguments.

    TestFile - Supplies the file to test on.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    char Buffer[100];
    char Buffer2[100];
    int Bytes;
    int FileDescriptor = -1;
    int Result;
    char ContentA[] = "I am your father! Noooo!";
    char ContentB[] = "Go big or go home.";
    struct iovec Iov[3];

    LxtCheckErrno(FileDescriptor = open(TestFile, O_RDWR | O_CREAT | O_TRUNC, S_IRWXU));
    Iov[0].iov_base = ContentA;
    Iov[0].iov_len = sizeof(ContentA);
    Iov[1].iov_base = ContentB;
    Iov[1].iov_len = sizeof(ContentB);
    LxtCheckErrno(Bytes = writev(FileDescriptor, Iov, 2));
    LxtCheckEqual(Bytes, sizeof(ContentA) + sizeof(ContentB), "%d");
    LxtCheckErrno(close(FileDescriptor));

    //
    // Validate the data with read
    //

    LxtCheckErrno(FileDescriptor = open(TestFile, O_RDWR, S_IRWXU));
    memset(Buffer, 0, sizeof(Buffer));
    LxtCheckErrno(Bytes = read(FileDescriptor, Buffer, sizeof(ContentA)));
    LxtCheckEqual(Bytes, sizeof(ContentA), "%d");
    LxtCheckMemoryEqual(Buffer, ContentA, sizeof(ContentA));
    memset(Buffer, 0, sizeof(Buffer));
    LxtCheckErrno(Bytes = read(FileDescriptor, Buffer, sizeof(ContentB)));
    LxtCheckEqual(Bytes, sizeof(ContentB), "%d");
    LxtCheckMemoryEqual(Buffer, ContentB, sizeof(ContentB));
    LxtCheckErrno(close(FileDescriptor));

    //
    // Validate the data with readv
    //

    LxtCheckErrno(FileDescriptor = open(TestFile, O_RDWR, S_IRWXU));
    memset(Iov, 0, sizeof(Iov));
    memset(Buffer, 0, sizeof(Buffer));
    memset(Buffer2, 0, sizeof(Buffer2));
    Iov[0].iov_base = Buffer;
    Iov[0].iov_len = sizeof(ContentA);
    Iov[1].iov_base = Buffer2;
    Iov[1].iov_len = sizeof(ContentB);
    LxtCheckErrno(Bytes = readv(FileDescriptor, Iov, 2));
    LxtCheckEqual(Bytes, sizeof(ContentA) + sizeof(ContentB), "%d");
    LxtCheckMemoryEqual(Iov[0].iov_base, ContentA, sizeof(ContentA));
    LxtCheckMemoryEqual(Iov[1].iov_base, ContentB, sizeof(ContentB));
    LxtCheckErrno(close(FileDescriptor));

    //
    // Validate that readv\writev returns the right error code when an invalid
    // count is passed.
    //

    LxtLogInfo("invalid vector count");
    LxtCheckErrno(FileDescriptor = open(TestFile, O_RDWR, S_IRWXU));

#ifdef __x86_64__

#define READV_SYSCALL_NR 19

#elif __arm__

#define READV_SYSCALL_NR 65

#endif

    LxtCheckErrnoFailure(syscall(READV_SYSCALL_NR, FileDescriptor, Iov, -1), EINVAL);
    LxtCheckErrnoFailure(syscall(READV_SYSCALL_NR, FileDescriptor, Iov, -1), EINVAL);
    LxtCheckErrno(close(FileDescriptor));

    //
    // Test a zero-length iovector at the beginning of the array, it should be skipped.
    //

    LxtCheckErrno(FileDescriptor = open(TestFile, O_RDWR | O_CREAT | O_TRUNC, S_IRWXU));
    memset(Iov, 0, sizeof(Iov));
    Iov[0].iov_base = ContentA;
    Iov[1].iov_base = ContentB;
    Iov[1].iov_len = sizeof(ContentB);
    LxtCheckErrno(Bytes = writev(FileDescriptor, Iov, 2));
    LxtCheckEqual(Bytes, sizeof(ContentB), "%d");
    LxtCheckErrno(close(FileDescriptor));

    LxtCheckErrno(FileDescriptor = open(TestFile, O_RDWR, S_IRWXU));
    memset(Buffer, 0, sizeof(Buffer));
    LxtCheckErrno(Bytes = read(FileDescriptor, Buffer, sizeof(ContentB)));
    LxtCheckEqual(Bytes, sizeof(ContentB), "%d");
    LxtCheckMemoryEqual(Buffer, ContentB, sizeof(ContentB));
    LxtCheckErrno(close(FileDescriptor));

    //
    // Test an invalid buffer after a valid buffer. The writev call should return the
    // number of bytes written until the invalid buffer.
    //
    // N.B. The plan 9 client does not follow this behavior, and will write nothing.
    //

    LxtCheckErrno(FileDescriptor = open(TestFile, O_RDWR | O_CREAT | O_TRUNC, S_IRWXU));
    memset(Iov, 0, sizeof(Iov));
    Iov[0].iov_base = ContentA;
    Iov[0].iov_len = sizeof(ContentA);
    Iov[1].iov_base = NULL;
    Iov[1].iov_len = sizeof(ContentB);
    Iov[2].iov_base = ContentB;
    Iov[2].iov_len = sizeof(ContentB);
    LxtLogInfo("%d", g_LxtFsInfo.FsType);
    if (g_LxtFsInfo.FsType == LxtFsTypePlan9)
    {
        LxtCheckErrnoFailure(Bytes = writev(FileDescriptor, Iov, 3), EFAULT);
    }
    else
    {
        LxtCheckErrno(Bytes = writev(FileDescriptor, Iov, 3));
        LxtCheckEqual(Bytes, sizeof(ContentA), "%d");
        LxtCheckErrno(close(FileDescriptor));

        LxtCheckErrno(FileDescriptor = open(TestFile, O_RDWR, S_IRWXU));
        memset(Buffer, 0, sizeof(Buffer));
        LxtCheckErrno(Bytes = read(FileDescriptor, Buffer, sizeof(ContentA)));
        LxtCheckEqual(Bytes, sizeof(ContentA), "%d");
        LxtCheckErrno(Bytes = read(FileDescriptor, Buffer, sizeof(ContentB)));
        LxtCheckEqual(Bytes, 0, "%d");
    }

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    if (FileDescriptor != -1)
    {
        close(FileDescriptor);
    }

    unlink(TestFile);
    return Result;
}

int LxtFsDirSeekCommon(const char* BaseDir)

/*++

Description:

    This routine tests the seek operation on a directory.

Arguments:

    BaseDir - Supplies the directory to use. for the test. The directory should
        not already exist.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    char Buffer[4096];
    int DirFd;
    int Result;
    int Size;
    int FirstSize;

    //
    // Create the base directory.
    //

    LxtCheckErrnoZeroSuccess(mkdir(BaseDir, 0777));
    LxtCheckErrno(DirFd = open(BaseDir, O_RDONLY | O_DIRECTORY));
    LxtCheckErrno(Size = syscall(SYS_getdents64, DirFd, Buffer, sizeof(Buffer)));

    //
    // Directory should at least have one entry.
    //

    if (Size == 0)
    {
        LxtLogError("Directory is expected to at least have one entry.");
        Result = EINVAL;
        goto ErrorExit;
    }

    //
    // Store the value returned from the first call to 'getdents'.
    //

    FirstSize = Size;

    //
    // Once the end of directory has reached, getdents should return 0.
    //

    LxtCheckErrno(Size = syscall(SYS_getdents64, DirFd, Buffer, sizeof(Buffer)));
    if (Size != 0)
    {
        LxtLogError(
            "getdents should return 0 when end of directory is reached, "
            "but it returned: %d.",
            Size);

        Result = EINVAL;
        goto ErrorExit;
    }

    LxtCheckErrno(lseek(DirFd, 0, SEEK_SET));
    LxtCheckErrno(Size = syscall(SYS_getdents64, DirFd, Buffer, sizeof(Buffer)));

    //
    // Directory should at least have one entry.
    //

    if (Size == 0)
    {
        LxtLogError(
            "lseek on a dir should rewind the cursor position, but it "
            "did not.");

        Result = EINVAL;
        goto ErrorExit;
    }

    if (Size != FirstSize)
    {
        LxtLogError(
            "getdents value should not have changed from first call. "
            "First getdents: %d, Second getdents: %d",
            FirstSize,
            Size);

        Result = EINVAL;
        goto ErrorExit;
    }

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    if (DirFd >= 0)
    {
        close(DirFd);
    }

    rmdir(BaseDir);
    return Result;
}
