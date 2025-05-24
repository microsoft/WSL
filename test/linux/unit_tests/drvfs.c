/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    drvfs.c

Abstract:

    This file contains tests for the drvfs file system plugin.

    The drvfs tests are kept separate from other file system tests to make it
    possible to run the tests in "fallback mode," the mode drvfs operates in
    when NtQueryInformationByName or FILE_STAT_INFORMATION are not supported
    by the underlying file system.

    Additionally, most of these tests do not pass on native Linux, so keeping
    the drvfs tests apart makes it easier to validate the other file system
    tests on native Linux.

--*/

#include "lxtcommon.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/syscall.h>
#include <sys/inotify.h>
#include <sys/mount.h>
#include <sys/signalfd.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <dirent.h>
#include <stdlib.h>
#include <stdio.h>
#include <libmount/libmount.h>
#include "lxtfs.h"
#include "lxtmount.h"

#define LXT_NAME_FORMAT "drvfs%d"

#define DRVFS_DRIVE "C:"
#define DRVFS_FAT_MOUNT_POINT "lxss_fat"
#define DRVFS_FAT_DRIVE "C:/" DRVFS_FAT_MOUNT_POINT
#define DRVFS_UNC_PATH "//localhost/C$"
#define DRVFS_FAT_TEST_MODE (3)
#define DRVFS_SMB_TEST_MODE (4)
#define DRVFS_METADATA_TEST_MODE (5)
#define DRVFS_REFS_TEST_MODE (6)
#define DRVFS_REFS_MOUNT_POINT "lxss_refs"
#define DRVFS_REFS_DRIVE "C:/" DRVFS_REFS_MOUNT_POINT
#define DRVFS_FS_TYPE "drvfs"
#define DRVFS_MOUNT_OPTIONS (MS_NOATIME)
#define DRVFS_PREFIX "/mnt/c"
#define DRVFS_CS_PREFIX DRVFS_PREFIX "/casesensitive"
#define DRVFS_BASIC_PREFIX DRVFS_PREFIX "/basictest"
#define DRVFS_RENAME_PREFIX DRVFS_PREFIX "/renametest"
#define DRVFS_REPARSE_PREFIX DRVFS_PREFIX "/reparsetest"
#define DRVFS_MOUNT_TEST_DIR "/data/mount_test"
#define DRVFS_ACCESS_TEST_DIR DRVFS_PREFIX "/drvfstest"
#define DRVFS_ACCESS_RWX_TEST_FILE DRVFS_ACCESS_TEST_DIR "/rwx"
#define DRVFS_ACCESS_READONLY_TEST_FILE DRVFS_ACCESS_TEST_DIR "/readonly"
#define DRVFS_ACCESS_WRITEONLY_TEST_FILE DRVFS_ACCESS_TEST_DIR "/writeonly"
#define DRVFS_ACCESS_EXECUTEONLY_TEST_FILE DRVFS_ACCESS_TEST_DIR "/executeonly"
#define DRVFS_ACCESS_EXECUTEONLY_TEST_DIR DRVFS_ACCESS_TEST_DIR "/executeonlydir"
#define DRVFS_ACCESS_READONLYATTR_TEST_FILE DRVFS_ACCESS_TEST_DIR "/readonlyattr"
#define DRVFS_ACCESS_READONLYATTRDEL_TEST_FILE DRVFS_ACCESS_TEST_DIR "/readonlyattrdel"
#define DRVFS_ACCESS_EXECUTEONLY_TEST_DIR_CHILD DRVFS_ACCESS_EXECUTEONLY_TEST_DIR "/child"
#define DRVFS_ACCESS_EXECUTEONLY_TEST_DIR_CHILD2 DRVFS_ACCESS_EXECUTEONLY_TEST_DIR "/child2"
#define DRVFS_ACCESS_EXECUTEONLY_TEST_DIR_CHILD_LINK DRVFS_ACCESS_EXECUTEONLY_TEST_DIR "/link"
#define DRVFS_ACCESS_READONLY_TEST_DIR DRVFS_ACCESS_TEST_DIR "/noexecutedir"
#define DRVFS_INOTIFY_TEST_BASE_DIR DRVFS_PREFIX "/inotify_test/"
#define DRVFS_UTIME_TEST_DIR DRVFS_PREFIX "/utimensat_test"
#define DRVFS_WRITEV_TEST_DIR DRVFS_PREFIX "/writev_test"
#define DRVFS_RENAMEAT_TEST_DIR DRVFS_PREFIX "/renameat_test"
#define DRVFS_CASE_INSENSITIVE_TEST_DIR DRVFS_PREFIX "/case_insensitive_test"
#define DRVFS_UNSUPPORTED_TEST_DIR DRVFS_PREFIX "/unsupported_test"
#define DRVFS_HARDLINK_TEST_DIR DRVFS_PREFIX "/hardlink_test"
#define DRVFS_DELETELOOP_PREFIX DRVFS_PREFIX "/deleteloop"
#define DRVFS_GETDENTS_PREFIX DRVFS_PREFIX "/getdents"
#define DRVFS_SYMLINK_TEST_DIR DRVFS_PREFIX "/symlink"
#define DRVFS_METADATA_TEST_DIR DRVFS_PREFIX "/metadatatest"
#define DRVFS_ESCAPE_TEST_DIR DRVFS_PREFIX "/escaped"
#define DRVFS_ESCAPE_TEST_CHILD_NAME "\\:\b\u8a9e\uefff\uf025\uf100\ufb00\uf02f"
#define DRVFS_ESCAPE_TEST_CHILD DRVFS_ESCAPE_TEST_DIR "/" DRVFS_ESCAPE_TEST_CHILD_NAME
#define DRVFS_ESCAPE_TEST_CHILD_ESCAPED DRVFS_ESCAPE_TEST_DIR "/\uf05c\uf03a\uf008\u8a9e\uefff\uf025\uf100\ufb00\uf02f"

//
// The following macros are used by TestInotifyComprehensiveDrvfs.
//

#define DRVFS_INOTIFY_DIR_1 "a/"
#define DRVFS_INOTIFY_DIR_2 "a/b/"
#define DRVFS_INOTIFY_DIR_3 "a/b/c/"
#define DRVFS_INOTIFY_FILE_1_NAME_ONLY "a.txt"
#define DRVFS_INOTIFY_FILE_2_NAME_ONLY "b.txt"
#define DRVFS_INOTIFY_FILE_3_NAME_ONLY "c.txt"
#define DRVFS_INOTIFY_FILE_1 DRVFS_INOTIFY_DIR_3 DRVFS_INOTIFY_FILE_1_NAME_ONLY
#define DRVFS_INOTIFY_FILE_2 DRVFS_INOTIFY_DIR_3 DRVFS_INOTIFY_FILE_2_NAME_ONLY
#define DRVFS_INOTIFY_FILE_3 DRVFS_INOTIFY_DIR_3 DRVFS_INOTIFY_FILE_3_NAME_ONLY

//
// The following macros are used by TestInotifyStressUnlinkRenameDrvfs.
//

#define DRVFS_INOTIFY_STRESS_DIR "stress/"
#define DRVFS_INOTIFY_STRESS_NUM_FILES 2
#define DRVFS_INOTIFY_STRESS_NUM_TESTS 1000

#define S_IRUGO (S_IRUSR | S_IRGRP | S_IROTH)
#define S_IWUGO (S_IWUSR | S_IWGRP | S_IWOTH)
#define S_IXUGO (S_IXUSR | S_IXGRP | S_IXOTH)

#define DRVFS_EXECVE_TEST_RESULT (123)

int DrvFsCheckMode(const char* Filename, mode_t ExpectedMode);

int DrvFsCheckStat(const char* Filename, uid_t ExpectedUid, gid_t ExpectedGid, mode_t ExpectedMode, dev_t ExpectedDevice);

int DrvFsParseArgs(int Argc, char* Argv[], LXT_ARGS* Args);

LXT_VARIATION_HANDLER DrvFsTestAccess;

LXT_VARIATION_HANDLER DrvFsTestBadMetadata;

LXT_VARIATION_HANDLER DrvFsTestBasic;

LXT_VARIATION_HANDLER DrvFsTestBlockCount;

LXT_VARIATION_HANDLER DrvFsTestCaseSensitivity;

LXT_VARIATION_HANDLER DrvFsTestCaseSensitivityRoot;

LXT_VARIATION_HANDLER DrvFsTestDeleteCurrentWorkingDirectory;

LXT_VARIATION_HANDLER DrvFsTestDeleteLoop;

LXT_VARIATION_HANDLER DrvFsTestDeleteOpenFile;

LXT_VARIATION_HANDLER DrvFsTestEscapedNames;

LXT_VARIATION_HANDLER DrvFsTestExecve;

LXT_VARIATION_HANDLER DrvFsTestFatCaseInsensitive;

LXT_VARIATION_HANDLER DrvFsTestFatJunction;

LXT_VARIATION_HANDLER DrvFsTestFatUnsupported;

LXT_VARIATION_HANDLER DrvFsTestFatUtimensat;

LXT_VARIATION_HANDLER DrvFsTestFatWslPath;

LXT_VARIATION_HANDLER DrvFsTestFstat;

LXT_VARIATION_HANDLER DrvFsTestGetDents64Alignment;

LXT_VARIATION_HANDLER DrvFsTestGetDentsAlignment;

LXT_VARIATION_HANDLER DrvFsTestHardLinks;

LXT_VARIATION_HANDLER DrvFsTestHiddenLxFsDirs;

int DrvFsTestHiddenLxFsDirsHelper(const char* Child, BOOLEAN DirectChild);

LXT_VARIATION_HANDLER DrvFsTestInotifyBasic;

LXT_VARIATION_HANDLER DrvFsTestInotifyEpoll;

LXT_VARIATION_HANDLER DrvFsTestInotifyPosixUnlinkRename;

LXT_VARIATION_HANDLER DrvFsTestInotifyStressUnlinkRename;

LXT_VARIATION_HANDLER DrvFsTestInotifyUnmountBind;

LXT_VARIATION_HANDLER DrvFsTestLookupPath;

LXT_VARIATION_HANDLER DrvFsTestMetadata;

LXT_VARIATION_HANDLER DrvFsTestReFsWslPath;

LXT_VARIATION_HANDLER DrvFsTestRename;

LXT_VARIATION_HANDLER DrvFsTestRenameAt;

LXT_VARIATION_HANDLER DrvFsTestRenameDir;

LXT_VARIATION_HANDLER DrvFsTestReopenUnlinked;

LXT_VARIATION_HANDLER DrvFsTestReparse;

LXT_VARIATION_HANDLER DrvFsTestSeek;

LXT_VARIATION_HANDLER DrvFsTestDirSeek;

int DrvFsTestSeekHelper(int Fd, off_t Offset, int Whence, off_t ExpectedOffset, const char* TestData, int TestDataSize);

int DrvFsTestSetup(PLXT_ARGS Args, int TestMode);

LXT_VARIATION_HANDLER DrvFsTestSmbUtimensat;

LXT_VARIATION_HANDLER DrvFsTestSmbUnsupported;

LXT_VARIATION_HANDLER DrvFsTestSmbWslPath;

LXT_VARIATION_HANDLER DrvFsTestSymlink;

int DrvFsTestSymlinkHelper(char* Target, char* Path);

int DrvFsTestUnsupportedCommon(int TestMode);

LXT_VARIATION_HANDLER DrvFsTestUtimensat;

int DrvFsTestUtimensatCommon(int Flags);

LXT_VARIATION_HANDLER DrvFsTestWritev;

//
// Globals.
//

static const LXT_VARIATION g_LxtVariations[] = {
    {"DrvFs - basic", DrvFsTestBasic},
    {"DrvFs - lookup by path", DrvFsTestLookupPath},
    {"DrvFs - writev", DrvFsTestWritev},
    {"DrvFs - rename", DrvFsTestRename},
    {"DrvFs - renameat", DrvFsTestRenameAt},
    {"DrvFs - rename directory", DrvFsTestRenameDir},
    {"DrvFs - deleting an open file", DrvFsTestDeleteOpenFile},
    {"DrvFs - deleting the working directory", DrvFsTestDeleteCurrentWorkingDirectory},
    {"DrvFs - case-sensitivity", DrvFsTestCaseSensitivity},
    {"DrvFs - case-sensitivity (drive root)", DrvFsTestCaseSensitivityRoot},
    {"DrvFs - reparse points", DrvFsTestReparse},
    {"DrvFs - access checks", DrvFsTestAccess},
    {"DrvFs - execve", DrvFsTestExecve},
    {"DrvFs - hidden lxfs directories", DrvFsTestHiddenLxFsDirs},
    {"DrvFs - inotify with epoll", DrvFsTestInotifyEpoll},
    {"DrvFs - inotify watching basic paths", DrvFsTestInotifyBasic},
    {"DrvFs - inotify unmounting of a bind mount", DrvFsTestInotifyUnmountBind},
    {"DrvFs - inotify POSIX unlink/rename", DrvFsTestInotifyPosixUnlinkRename},
    {"DrvFs - inotify stress test with unlink and rename", DrvFsTestInotifyStressUnlinkRename},
    {"DrvFs - utimensat", DrvFsTestUtimensat},
    {"DrvFs - hard links", DrvFsTestHardLinks},
    {"DrvFs - block count", DrvFsTestBlockCount},
    {"DrvFs - fstat", DrvFsTestFstat},
    {"DrvFs - reopen unlinked file", DrvFsTestReopenUnlinked},
    {"DrvFs - delete loop", DrvFsTestDeleteLoop},
    {"DrvFs - seek", DrvFsTestSeek},
    {"DrvFs - dir seek", DrvFsTestDirSeek},
#ifdef __NR_getdents
    {"DrvFs - getdents alignment", DrvFsTestGetDentsAlignment},
#endif
    {"DrvFs - getdents64 alignment", DrvFsTestGetDents64Alignment},
    {"DrvFs - LX and NT symlink creation", DrvFsTestSymlink},
    {"DrvFs - escaped names", DrvFsTestEscapedNames}};

//
// The following variations apply to FAT volumes.
//

static const LXT_VARIATION g_LxtFatVariations[] = {
    {"DrvFs - basic", DrvFsTestBasic},
    {"DrvFs - lookup by path", DrvFsTestLookupPath},
    {"DrvFs - writev", DrvFsTestWritev},
    {"DrvFs - rename", DrvFsTestRename},
    {"DrvFs - renameat", DrvFsTestRenameAt},
    {"DrvFs - inotify with epoll", DrvFsTestInotifyEpoll},
    {"DrvFs - inotify unmounting of a bind mount", DrvFsTestInotifyUnmountBind},
    {"DrvFs - block count", DrvFsTestBlockCount},
    {"DrvFs - FAT32 case-insensitive", DrvFsTestFatCaseInsensitive},
    {"DrvFs - FAT32 unsupported features", DrvFsTestFatUnsupported},
    {"DrvFs - FAT32 utimensat", DrvFsTestFatUtimensat},
    {"DrvFs - FAT32 mount point junction", DrvFsTestFatJunction},
    {"DrvFs - fstat", DrvFsTestFstat},
    {"DrvFs - delete loop", DrvFsTestDeleteLoop},
    {"DrvFs - seek", DrvFsTestSeek},
#ifdef __NR_getdents
    {"DrvFs - getdents alignment", DrvFsTestGetDentsAlignment},
#endif
    {"DrvFs - getdents64 alignment", DrvFsTestGetDents64Alignment},
    {"DrvFs - escaped names", DrvFsTestEscapedNames},
    {"DrvFs - wslpath NTFS directory mount", DrvFsTestFatWslPath}};

//
// The following variations apply to SMB shares.
//

static const LXT_VARIATION g_LxtSmbVariations[] = {
    {"DrvFs - basic", DrvFsTestBasic},
    {"DrvFs - lookup by path", DrvFsTestLookupPath},
    {"DrvFs - writev", DrvFsTestWritev},
    {"DrvFs - rename", DrvFsTestRename},
    {"DrvFs - renameat", DrvFsTestRenameAt},
    {"DrvFs - hard links", DrvFsTestHardLinks},
    {"DrvFs - SMB case-insensitive", DrvFsTestFatCaseInsensitive},
    {"DrvFs - SMB unsupported features", DrvFsTestSmbUnsupported},
    {"DrvFs - SMB utimensat", DrvFsTestSmbUtimensat},
    {"DrvFs - fstat", DrvFsTestFstat},
    {"DrvFs - delete loop", DrvFsTestDeleteLoop},
    {"DrvFs - seek", DrvFsTestSeek},
#ifdef __NR_getdents
    {"DrvFs - getdents alignment", DrvFsTestGetDentsAlignment},
#endif
    {"DrvFs - getdents64 alignment", DrvFsTestGetDents64Alignment},
    {"DrvFs - escaped names", DrvFsTestEscapedNames},
    {"DrvFs - wslpath UNC", DrvFsTestSmbWslPath}};

//
// The following variations apply when metadata is on.
//

static const LXT_VARIATION g_LxtMetadataVariations[] = {
    {"DrvFs - basic", DrvFsTestBasic},
    {"DrvFs - lookup by path", DrvFsTestLookupPath},
    {"DrvFs - writev", DrvFsTestWritev},
    {"DrvFs - rename", DrvFsTestRename},
    {"DrvFs - renameat", DrvFsTestRenameAt},
    {"DrvFs - rename directory", DrvFsTestRenameDir},
    {"DrvFs - deleting an open file", DrvFsTestDeleteOpenFile},
    {"DrvFs - deleting the working directory", DrvFsTestDeleteCurrentWorkingDirectory},
    {"DrvFs - case-sensitivity", DrvFsTestCaseSensitivity},
    {"DrvFs - case-sensitivity (drive root)", DrvFsTestCaseSensitivityRoot},
    {"DrvFs - reparse points", DrvFsTestReparse},
    {"DrvFs - access checks", DrvFsTestAccess},
    {"DrvFs - execve", DrvFsTestExecve},
    {"DrvFs - hidden lxfs directories", DrvFsTestHiddenLxFsDirs},
    {"DrvFs - inotify with epoll", DrvFsTestInotifyEpoll},
    {"DrvFs - inotify watching basic paths", DrvFsTestInotifyBasic},
    {"DrvFs - inotify unmounting of a bind mount", DrvFsTestInotifyUnmountBind},
    {"DrvFs - inotify POSIX unlink/rename", DrvFsTestInotifyPosixUnlinkRename},
    {"DrvFs - inotify stress test with unlink and rename", DrvFsTestInotifyStressUnlinkRename},
    {"DrvFs - utimensat", DrvFsTestUtimensat},
    {"DrvFs - hard links", DrvFsTestHardLinks},
    {"DrvFs - block count", DrvFsTestBlockCount},
    {"DrvFs - fstat", DrvFsTestFstat},
    {"DrvFs - reopen unlinked file", DrvFsTestReopenUnlinked},
    {"DrvFs - delete loop", DrvFsTestDeleteLoop},
    {"DrvFs - seek", DrvFsTestSeek},
    {"DrvFs - dir seek", DrvFsTestDirSeek},
#ifdef __NR_getdents
    {"DrvFs - getdents alignment", DrvFsTestGetDentsAlignment},
#endif
    {"DrvFs - getdents64 alignment", DrvFsTestGetDents64Alignment},
    {"DrvFs - LX and NT symlink creation", DrvFsTestSymlink},
    {"DrvFs - bad metadata", DrvFsTestBadMetadata},
    {"DrvFs - metadata", DrvFsTestMetadata},
    {"DrvFs - escaped names", DrvFsTestEscapedNames}};

static const LXT_VARIATION g_LxtReFsVariations[] = {
    {"DrvFs - basic", DrvFsTestBasic},
    {"DrvFs - lookup by path", DrvFsTestLookupPath},
    {"DrvFs - writev", DrvFsTestWritev},
    {"DrvFs - rename", DrvFsTestRename},
    {"DrvFs - renameat", DrvFsTestRenameAt},
    {"DrvFs - rename directory", DrvFsTestRenameDir},
    {"DrvFs - deleting an open file", DrvFsTestDeleteOpenFile},
    {"DrvFs - deleting the working directory", DrvFsTestDeleteCurrentWorkingDirectory},
    {"DrvFs - case-sensitivity", DrvFsTestCaseSensitivity},
    {"DrvFs - case-sensitivity (drive root)", DrvFsTestCaseSensitivityRoot},
    {"DrvFs - hidden lxfs directories", DrvFsTestHiddenLxFsDirs},
    {"DrvFs - inotify with epoll", DrvFsTestInotifyEpoll},
    {"DrvFs - inotify unmounting of a bind mount", DrvFsTestInotifyUnmountBind},
    {"DrvFs - inotify POSIX unlink/rename", DrvFsTestInotifyPosixUnlinkRename},
    {"DrvFs - utimensat", DrvFsTestUtimensat},
    {"DrvFs - hard links", DrvFsTestHardLinks},
    {"DrvFs - block count", DrvFsTestBlockCount},
    {"DrvFs - fstat", DrvFsTestFstat},
    {"DrvFs - reopen unlinked file", DrvFsTestReopenUnlinked},
    {"DrvFs - delete loop", DrvFsTestDeleteLoop},
    {"DrvFs - seek", DrvFsTestSeek},
    {"DrvFs - dir seek", DrvFsTestDirSeek},
#ifdef __NR_getdents
    {"DrvFs - getdents alignment", DrvFsTestGetDentsAlignment},
#endif
    {"DrvFs - getdents64 alignment", DrvFsTestGetDents64Alignment},
    {"DrvFs - LX and NT symlink creation", DrvFsTestSymlink},
    //{"DrvFs - escaped names", DrvFsTestEscapedNames},// TODO: enable this variation when lxutil is fixed
    {"DrvFs - wslpath ReFS", DrvFsTestReFsWslPath}};

static char* VfsAccessLxssDir;
static int DrvFsTestMode;

int DrvfsTestEntry(int Argc, char* Argv[])

/*++
--*/

{

    LXT_ARGS Args;
    int Result;

    if ((Argc == 2) && (strcmp(Argv[1], "execvetest") == 0))
    {
        return DRVFS_EXECVE_TEST_RESULT;
    }

    LXT_SYNCHRONIZATION_POINT_INIT();
    LxtCheckResult(DrvFsParseArgs(Argc, Argv, &Args));

ErrorExit:
    LXT_SYNCHRONIZATION_POINT_DESTROY();
    LxtUninitialize();
    return !LXT_SUCCESS(Result);
}

int DrvFsCheckMode(const char* Filename, mode_t ExpectedMode)

/*++

Description:

    This routine checks if a file's mode matches the expected value.

Arguments:

    Filename - Supplies the file name.

    ExpectedMode - Supplies the mode.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int Result;
    struct stat Stat;

    LxtCheckErrnoZeroSuccess(lstat(Filename, &Stat));
    LxtLogInfo("%s: mode 0%o", Filename, Stat.st_mode);
    LxtCheckEqual(Stat.st_mode, ExpectedMode, "0%o");

ErrorExit:
    return Result;
}

int DrvFsCheckStat(const char* Filename, uid_t ExpectedUid, gid_t ExpectedGid, mode_t ExpectedMode, dev_t ExpectedDevice)

/*++

Description:

    This routine checks whether the attributes of a file match the expected
    values.

Arguments:

    Filename - Supplies the name of the file.

    ExpectedUid - Supplies the expected value for the uid.

    ExpectedGid - Supplies the expected value for the uid.

    ExpectedMode - Supplies the expected value for the mode.

    ExpectedDevice - Supplies the expected value for the device.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int Result;
    struct stat Stat;

    LxtCheckErrnoZeroSuccess(lstat(Filename, &Stat));
    LxtCheckEqual(Stat.st_uid, ExpectedUid, "%d");
    LxtCheckEqual(Stat.st_gid, ExpectedGid, "%d");
    LxtCheckEqual(Stat.st_mode, ExpectedMode, "0%o");
    LxtCheckEqual(Stat.st_rdev, ExpectedDevice, "0x%lx");

ErrorExit:
    return Result;
}

int DrvFsParseArgs(int Argc, char* Argv[], LXT_ARGS* Args)

/*++

Routine Description:

    This routine parses command line arguments for the vfsaccess tests.

Arguments:

    Argc - Supplies the number of arguments.

    Argv - Supplies an array of arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int ArgvIndex;
    char Name[100];
    int Result;
    int TestMode;
    int ValidArguments;
    unsigned int VariationCount;
    const LXT_VARIATION* Variations;

    Result = LXT_RESULT_FAILURE;
    Variations = g_LxtVariations;
    VariationCount = LXT_COUNT_OF(g_LxtVariations);
    TestMode = 0;
    ValidArguments = 0;
    if (Argc < 1)
    {
        goto ErrorExit;
    }

    for (ArgvIndex = 1; ArgvIndex < Argc; ++ArgvIndex)
    {
        if (Argv[ArgvIndex][0] != '-')
        {
            printf("Unexpected character %s", Argv[ArgvIndex]);
            goto ErrorExit;
        }

        switch (Argv[ArgvIndex][1])
        {
        case 'v':
        case 'l':

            //
            // This was already taken care of by LxtInitialize.
            //

            ++ArgvIndex;

            break;

        case 'd':
            ++ArgvIndex;
            if (ArgvIndex < Argc)
            {
                ValidArguments = 1;
                VfsAccessLxssDir = Argv[ArgvIndex];
            }

            break;

        case 'm':
            ++ArgvIndex;
            if (ArgvIndex < Argc)
            {
                ValidArguments = -1;
                TestMode = atoi(Argv[ArgvIndex]);
                switch (TestMode)
                {
                case DRVFS_FAT_TEST_MODE:
                    LxtLogInfo("Running FAT variations.");
                    Variations = g_LxtFatVariations;
                    VariationCount = LXT_COUNT_OF(g_LxtFatVariations);
                    break;

                case DRVFS_SMB_TEST_MODE:
                    LxtLogInfo("Running SMB variations.");
                    Variations = g_LxtSmbVariations;
                    VariationCount = LXT_COUNT_OF(g_LxtSmbVariations);
                    break;

                case DRVFS_METADATA_TEST_MODE:
                    LxtLogInfo("Running metadata variations.");
                    Variations = g_LxtMetadataVariations;
                    VariationCount = LXT_COUNT_OF(g_LxtMetadataVariations);
                    break;

                case DRVFS_REFS_TEST_MODE:
                    LxtLogInfo("Running ReFs variations.");
                    Variations = g_LxtReFsVariations;
                    VariationCount = LXT_COUNT_OF(g_LxtReFsVariations);
                    break;
                }
            }

            break;

        case 'h':
        case 'a':
            break;

        default:
            goto ErrorExit;
        }
    }

    //
    // If -c was not specified, just run the tests
    //

    ValidArguments = 1;
    DrvFsTestMode = TestMode;
    snprintf(Name, sizeof(Name), LXT_NAME_FORMAT, TestMode);
    LxtCheckResult(LxtInitialize(Argc, Argv, Args, Name));
    LxtCheckResult(DrvFsTestSetup(Args, TestMode));
    LxtCheckResult(LxtRunVariations(Args, Variations, VariationCount));

ErrorExit:

    //
    // Remount drvfs normally.
    //

    chdir("/");
    umount(DRVFS_PREFIX);
    LxtFsMountDrvFs(DRVFS_DRIVE, DRVFS_PREFIX, NULL);
    if (ValidArguments == 0)
    {
        printf("\nuse: %s <One of the below arguments>\n", Argv[0]);
        printf("\t-d : lxfs directory\n");
        printf("\t-m : test mode\n");
    }

    return Result;
}

int DrvFsTestAccess(PLXT_ARGS Args)

/*++

Description:

    This routine tests access permissions on DrvFs.

    N.B. Test files with proper permissions are created by the TAEF DLL.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    ssize_t Bytes;
    char Buf[100] = {0};
    int Fd;
    int Result;

    //
    // File with read/write/execute access.
    //

    Fd = -1;
    LxtCheckResult(DrvFsCheckMode(DRVFS_ACCESS_RWX_TEST_FILE, S_IFREG | S_IRUGO | S_IWUGO | S_IXUGO));

    LxtCheckErrno(Fd = open(DRVFS_ACCESS_RWX_TEST_FILE, O_RDWR));
    LxtCheckErrnoZeroSuccess(close(Fd));
    Fd = -1;

    //
    // Readonly file. This file can only be opened for read, and writing should
    // fail. O_PATH always works.
    //

    LxtCheckResult(DrvFsCheckMode(DRVFS_ACCESS_READONLY_TEST_FILE, S_IFREG | S_IRUGO));

    LxtCheckErrno(Fd = open(DRVFS_ACCESS_READONLY_TEST_FILE, O_RDONLY));
    LxtCheckErrno(Bytes = read(Fd, Buf, sizeof(Buf)));
    LxtCheckEqual(Bytes, 0L, "%ld");
    LxtCheckErrnoFailure(write(Fd, Buf, sizeof(Buf)), EBADF);
    LxtCheckErrnoZeroSuccess(close(Fd));
    LxtCheckErrnoFailure(Fd = open(DRVFS_ACCESS_READONLY_TEST_FILE, O_WRONLY), EACCES);

    LxtCheckErrnoFailure(Fd = open(DRVFS_ACCESS_READONLY_TEST_FILE, O_RDWR), EACCES);

    LxtCheckErrno(Fd = open(DRVFS_ACCESS_READONLY_TEST_FILE, O_PATH));
    LxtCheckErrnoFailure(read(Fd, Buf, sizeof(Buf)), EBADF);
    LxtCheckErrnoFailure(write(Fd, Buf, sizeof(Buf)), EBADF);
    LxtCheckErrnoZeroSuccess(close(Fd));
    Fd = -1;

    //
    // File with read/write/execute access, but the read-only attribute set.
    // This file can only be opened for read, and writing should fail.
    // O_PATH always works.
    //

    LxtCheckResult(DrvFsCheckMode(DRVFS_ACCESS_READONLYATTR_TEST_FILE, S_IFREG | S_IRUGO | S_IXUGO));

    LxtCheckErrno(Fd = open(DRVFS_ACCESS_READONLY_TEST_FILE, O_RDONLY));
    LxtCheckErrno(Bytes = read(Fd, Buf, sizeof(Buf)));
    LxtCheckEqual(Bytes, 0L, "%ld");
    LxtCheckErrnoFailure(write(Fd, Buf, sizeof(Buf)), EBADF);
    LxtCheckErrnoZeroSuccess(close(Fd));
    LxtCheckErrnoFailure(Fd = open(DRVFS_ACCESS_READONLY_TEST_FILE, O_WRONLY), EACCES);

    LxtCheckErrnoFailure(Fd = open(DRVFS_ACCESS_READONLY_TEST_FILE, O_RDWR), EACCES);

    LxtCheckErrno(Fd = open(DRVFS_ACCESS_READONLY_TEST_FILE, O_PATH));
    LxtCheckErrnoFailure(read(Fd, Buf, sizeof(Buf)), EBADF);
    LxtCheckErrnoFailure(write(Fd, Buf, sizeof(Buf)), EBADF);
    LxtCheckErrnoZeroSuccess(close(Fd));
    Fd = -1;

    //
    // Second file with the read-only attribute set, used to check if that
    // file can be deleted.
    //

    LxtCheckResult(DrvFsCheckMode(DRVFS_ACCESS_READONLYATTRDEL_TEST_FILE, S_IFREG | S_IRUGO | S_IXUGO));

    LxtCheckErrnoZeroSuccess(unlink(DRVFS_ACCESS_READONLYATTRDEL_TEST_FILE));

    //
    // Writeonly file. This file can only be opened for write and read should
    // fail. O_PATH always works.
    //

    LxtCheckResult(DrvFsCheckMode(DRVFS_ACCESS_WRITEONLY_TEST_FILE, S_IFREG | S_IWUGO));

    LxtCheckErrno(Fd = open(DRVFS_ACCESS_WRITEONLY_TEST_FILE, O_WRONLY));
    LxtCheckErrnoFailure(read(Fd, Buf, sizeof(Buf)), EBADF);
    LxtCheckErrno(Bytes = write(Fd, Buf, sizeof(Buf)));
    LxtCheckEqual(Bytes, 100L, "%ld");
    LxtCheckErrnoZeroSuccess(close(Fd));
    LxtCheckErrnoFailure(Fd = open(DRVFS_ACCESS_WRITEONLY_TEST_FILE, O_RDONLY), EACCES);

    LxtCheckErrnoFailure(Fd = open(DRVFS_ACCESS_WRITEONLY_TEST_FILE, O_RDWR), EACCES);

    LxtCheckErrno(Fd = open(DRVFS_ACCESS_READONLY_TEST_FILE, O_PATH));
    LxtCheckErrnoFailure(read(Fd, Buf, sizeof(Buf)), EBADF);
    LxtCheckErrnoFailure(write(Fd, Buf, sizeof(Buf)), EBADF);
    LxtCheckErrnoZeroSuccess(close(Fd));
    Fd = -1;

    //
    // Directory with add/delete file and traverse permissions. Opening a file
    // in the directory should succeed, but opening the directory itself should
    // not. Adding and deleting a file should work, but adding a subdirectory
    // should not. O_PATH always works.
    //

    LxtCheckResult(DrvFsCheckMode(DRVFS_ACCESS_EXECUTEONLY_TEST_DIR, S_IFDIR | S_IWUGO | S_IXUGO));

    LxtCheckResult(DrvFsCheckMode(DRVFS_ACCESS_EXECUTEONLY_TEST_DIR_CHILD, S_IFREG | S_IRUGO));

    LxtCheckErrnoFailure(Fd = open(DRVFS_ACCESS_EXECUTEONLY_TEST_DIR, O_RDONLY | O_DIRECTORY), EACCES);

    LxtCheckErrno(Fd = open(DRVFS_ACCESS_EXECUTEONLY_TEST_DIR_CHILD, O_RDONLY));

    LxtCheckErrnoZeroSuccess(close(Fd));
    LxtCheckErrno(Fd = open(DRVFS_ACCESS_EXECUTEONLY_TEST_DIR, O_PATH | O_DIRECTORY));

    LxtCheckErrnoFailure(syscall(SYS_getdents64, Fd, Buf, sizeof(Buf)), EBADF);
    LxtCheckErrnoZeroSuccess(close(Fd));
    Fd = -1;

    //
    // Creating a readonly file should work, but the mode specified should not
    // take effect, unless metadata is being used.
    //

    LxtCheckErrno(Fd = open(DRVFS_ACCESS_EXECUTEONLY_TEST_DIR_CHILD2, O_WRONLY | O_CREAT | O_EXCL, S_IRUSR));

    LxtCheckErrnoZeroSuccess(close(Fd));
    Fd = -1;
    if (DrvFsTestMode == DRVFS_METADATA_TEST_MODE)
    {
        LxtCheckResult(DrvFsCheckMode(DRVFS_ACCESS_EXECUTEONLY_TEST_DIR_CHILD2, S_IFREG | 0400));
    }
    else
    {
        LxtCheckResult(DrvFsCheckMode(DRVFS_ACCESS_EXECUTEONLY_TEST_DIR_CHILD2, S_IFREG | S_IRUGO | S_IXUGO));
    }

    LxtCheckErrnoZeroSuccess(unlink(DRVFS_ACCESS_EXECUTEONLY_TEST_DIR_CHILD2));

    //
    // Creating a writable file should work, but the mode specified should not
    // take effect, unless metadata is being used.
    //

    LxtCheckErrno(Fd = open(DRVFS_ACCESS_EXECUTEONLY_TEST_DIR_CHILD2, O_WRONLY | O_CREAT | O_EXCL, S_IWUSR));

    LxtCheckErrnoZeroSuccess(close(Fd));
    Fd = -1;
    if (DrvFsTestMode == DRVFS_METADATA_TEST_MODE)
    {
        LxtCheckResult(DrvFsCheckMode(DRVFS_ACCESS_EXECUTEONLY_TEST_DIR_CHILD2, S_IFREG | 0200));
    }
    else
    {
        LxtCheckResult(DrvFsCheckMode(DRVFS_ACCESS_EXECUTEONLY_TEST_DIR_CHILD2, S_IFREG | S_IRUGO | S_IWUGO | S_IXUGO));
    }

    LxtCheckErrnoZeroSuccess(unlink(DRVFS_ACCESS_EXECUTEONLY_TEST_DIR_CHILD2));

    //
    // Creating a link should work, but creating a directory should not.
    //

    LxtCheckErrno(symlink(DRVFS_ACCESS_EXECUTEONLY_TEST_DIR_CHILD, DRVFS_ACCESS_EXECUTEONLY_TEST_DIR_CHILD_LINK));

    LxtCheckResult(DrvFsCheckMode(DRVFS_ACCESS_EXECUTEONLY_TEST_DIR_CHILD_LINK, S_IFLNK | S_IRUGO | S_IWUGO | S_IXUGO));

    LxtCheckErrnoFailure(mkdir(DRVFS_ACCESS_EXECUTEONLY_TEST_DIR_CHILD2, S_IRWXU), EACCES);

    //
    // Directory with only list permissions.
    //
    // Although traverse permissions were not set on this directory, Windows
    // still acts as if it can be traversed due to the bypass traverse checking
    // privilege, and LX should report the directory as traversable.
    //

    LxtCheckResult(DrvFsCheckMode(DRVFS_ACCESS_READONLY_TEST_DIR, S_IFDIR | S_IRUGO | S_IXUGO));

    //
    // Test chmod on DrvFs. Setting any write bit on the read-only attribute
    // file will clear the read-only attribute, which gets reported back with
    // all write bits set because DrvFs doesn't distinguish user, group and
    // other. Other bits cannot be affected by chmod, unless metadata is
    // enabled.
    //
    // N.B. The Windows side of this test will further verify the read-only
    //      attribute.
    //

    LxtCheckErrnoZeroSuccess(chmod(DRVFS_ACCESS_READONLYATTR_TEST_FILE, S_IWUSR));

    if (DrvFsTestMode == DRVFS_METADATA_TEST_MODE)
    {
        LxtCheckResult(DrvFsCheckMode(DRVFS_ACCESS_READONLYATTR_TEST_FILE, S_IFREG | 0200));
    }
    else
    {
        LxtCheckResult(DrvFsCheckMode(DRVFS_ACCESS_READONLYATTR_TEST_FILE, S_IFREG | S_IRUGO | S_IWUGO | S_IXUGO));
    }

    //
    // Conversely, removing all write bits will set the read-only attribute.
    //

    LxtCheckErrnoZeroSuccess(chmod(DRVFS_ACCESS_RWX_TEST_FILE, 0));

    if (DrvFsTestMode == DRVFS_METADATA_TEST_MODE)
    {
        LxtCheckResult(DrvFsCheckMode(DRVFS_ACCESS_RWX_TEST_FILE, S_IFREG));
    }
    else
    {
        LxtCheckResult(DrvFsCheckMode(DRVFS_ACCESS_RWX_TEST_FILE, S_IFREG | S_IRUGO | S_IXUGO));
    }

ErrorExit:
    if (Fd >= 0)
    {
        close(Fd);
    }

    unlink(DRVFS_ACCESS_EXECUTEONLY_TEST_DIR_CHILD2);
    unlink(DRVFS_ACCESS_EXECUTEONLY_TEST_DIR_CHILD_LINK);
    return Result;
}

int DrvFsTestBadMetadata(PLXT_ARGS Args)

/*++

Description:

    This routine tests files that have invalid metadata attributes.

    N.B. These files were created by the Windows side of the test.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int Result;

    //
    // Any bad metadata field will be ignored. The other fields are used.
    //

    LxtCheckResult(DrvFsCheckStat(DRVFS_METADATA_TEST_DIR "/baduid", 0, 3001, S_IFREG | 0644, 0));

    LxtCheckResult(DrvFsCheckStat(DRVFS_METADATA_TEST_DIR "/badgid", 3000, 0, S_IFREG | 0644, 0));

    //
    // NTFS does not return EffectiveAccess if a file has mode metadata, so
    // the access will be zero if it is corrupt.
    //

    LxtCheckResult(DrvFsCheckStat(DRVFS_METADATA_TEST_DIR "/badmode", 3000, 3001, S_IFREG, 0));

    //
    // If the file type in the mode doesn't match the actual file type, this
    // also gets treated as invalid
    //

    LxtCheckResult(DrvFsCheckStat(DRVFS_METADATA_TEST_DIR "/badtype1", 3000, 3001, S_IFREG, 0));

    LxtCheckResult(DrvFsCheckStat(DRVFS_METADATA_TEST_DIR "/badtype2", 3000, 3001, S_IFREG, 0));

    //
    // If the file is not a device, the device ID should not be reported even
    // if it's present in the metadata.
    //

    LxtCheckResult(DrvFsCheckStat(DRVFS_METADATA_TEST_DIR "/nondevice", 3000, 3001, S_IFREG | 0644, 0));

    //
    // Changing metadata on a file with corrupt metadata should work.
    //

    LxtCheckErrnoZeroSuccess(chown(DRVFS_METADATA_TEST_DIR "/baduid", 1000, -1));
    LxtCheckResult(DrvFsCheckStat(DRVFS_METADATA_TEST_DIR "/baduid", 1000, 3001, S_IFREG | 0644, 0));

    //
    // Also when the field being changed is not the corrupt one.
    //

    LxtCheckErrnoZeroSuccess(chown(DRVFS_METADATA_TEST_DIR "/badgid", 1000, -1));
    LxtCheckResult(DrvFsCheckStat(DRVFS_METADATA_TEST_DIR "/badgid", 1000, 0, S_IFREG | 0644, 0));

ErrorExit:
    return Result;
}

int DrvFsTestBasic(PLXT_ARGS Args)

/*++

Description:

    This routine tests basic drvfs functionality (reading/writing).

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    char Buffer[100];
    pid_t ChildPid;
    int Fd;
    struct stat FStat;
    ssize_t Size;
    struct stat Stat;
    int Result;

    Fd = -1;

    //
    // Create a test directory.
    //

    LxtCheckErrnoZeroSuccess(mkdir(DRVFS_BASIC_PREFIX, 0777));
    LxtCheckErrnoFailure(mkdir(DRVFS_BASIC_PREFIX, 0777), EEXIST);

    //
    // Verify the directory file size is equal to the file-system block-size,
    // and that block count is zero.
    //

    LxtCheckErrno(stat(DRVFS_BASIC_PREFIX, &Stat));
    LxtCheckGreater(Stat.st_size, 0ull, "%llu");
    LxtCheckEqual(Stat.st_size, (unsigned long long)Stat.st_blksize, "%llu");
    LxtCheckEqual(Stat.st_blocks, 0l, "%ld");

    //
    // Create a file and write to it.
    //

    LxtCheckErrnoFailure(Fd = open(DRVFS_BASIC_PREFIX "/test", O_RDWR), ENOENT);

    LxtCheckErrno(Fd = open(DRVFS_BASIC_PREFIX "/test", O_CREAT | O_RDWR, 0666));
    LxtCheckErrno(Size = write(Fd, "hello", 5));
    LxtCheckEqual(Size, 5, "%ld");

    //
    // Check stat results.
    //
    // N.B. Block count is reported as zero because this file is small enough
    //      to have its contents packed into the MFT by NTFS. This is not the
    //      case for FAT.
    //

    LxtCheckErrnoZeroSuccess(stat(DRVFS_BASIC_PREFIX "/test", &Stat));
    LxtCheckEqual(Stat.st_size, 5, "%lld");
    LxtCheckGreater(Stat.st_ino, 0, "%llu");
    if (DrvFsTestMode == DRVFS_FAT_TEST_MODE)
    {
        LxtCheckEqual(Stat.st_blocks, 2, "%d");
    }
    else
    {
        LxtCheckEqual(Stat.st_blocks, 0, "%d");
    }

    LxtCheckEqual(Stat.st_nlink, 1, "%d");
    LxtCheckEqual(Stat.st_rdev, 0, "%d");

    //
    // Check fstat has the same results.
    //

    LxtCheckErrnoZeroSuccess(stat(DRVFS_BASIC_PREFIX "/test", &Stat));
    LxtCheckErrnoZeroSuccess(fstat(Fd, &FStat));
    LxtCheckMemoryEqual(&Stat, &FStat, sizeof(Stat));

    //
    // Read back the data.
    //

    memset(Buffer, 0, sizeof(Buffer));
    LxtCheckErrno(lseek(Fd, 0, SEEK_SET));
    LxtCheckErrno(Size = read(Fd, Buffer, sizeof(Buffer)));
    LxtCheckEqual(Size, 5, "%lld");
    LxtCheckStringEqual(Buffer, "hello");
    LxtCheckClose(Fd);

    //
    // Test using O_APPEND.
    //

    LxtCheckErrno(Fd = open(DRVFS_BASIC_PREFIX "/test", O_RDWR | O_APPEND, 0666));

    LxtCheckErrno(Size = write(Fd, "foo", 3));
    LxtCheckEqual(Size, 3, "%ld");
    LxtCheckErrno(lseek(Fd, 0, SEEK_SET));
    LxtCheckErrno(Size = write(Fd, "bar", 3));
    LxtCheckEqual(Size, 3, "%ld");
    memset(Buffer, 0, sizeof(Buffer));
    LxtCheckErrno(lseek(Fd, 0, SEEK_SET));
    LxtCheckErrno(Size = read(Fd, Buffer, sizeof(Buffer)));
    LxtCheckEqual(Size, 11, "%lld");
    LxtCheckStringEqual(Buffer, "hellofoobar");
    LxtCheckClose(Fd);
    LxtCheckErrnoZeroSuccess(stat(DRVFS_BASIC_PREFIX "/test", &Stat));
    LxtCheckEqual(Stat.st_size, 11, "%lld");

    //
    // Creating/removing items relative to the current working directory.
    //

    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        LxtCheckErrnoZeroSuccess(chdir(DRVFS_BASIC_PREFIX));
        LxtCheckErrnoZeroSuccess(mkdir("a", 0777));
        LxtCheckErrnoZeroSuccess(access("a", F_OK));
        LxtCheckErrnoZeroSuccess(access(DRVFS_BASIC_PREFIX "/a", F_OK));
        LxtCheckErrnoZeroSuccess(rmdir("a"));
        LxtCheckErrnoFailure(access("a", F_OK), ENOENT);
        LxtCheckErrnoFailure(access(DRVFS_BASIC_PREFIX "/a", F_OK), ENOENT);
        exit(0);
    }

    LxtCheckResult(LxtWaitPidPoll(ChildPid, 0));

    //
    // Creating regular files with mknod should always work even without
    // metadata, though without metadata the exact mode won't be preserved.
    //

    LxtCheckErrnoZeroSuccess(mknod(DRVFS_BASIC_PREFIX "/node", S_IFREG | 0600, 0));
    LxtCheckErrnoZeroSuccess(stat(DRVFS_BASIC_PREFIX "/node", &Stat));
    if (DrvFsTestMode == DRVFS_METADATA_TEST_MODE)
    {
        LxtCheckEqual(Stat.st_mode, S_IFREG | 0600, "0%o");
    }
    else
    {
        LxtCheckEqual(Stat.st_mode, S_IFREG | 0777, "0%o");
    }

    //
    // Check that cleanup works.
    //

    LxtCheckErrnoZeroSuccess(unlink(DRVFS_BASIC_PREFIX "/node"));
    LxtCheckErrnoZeroSuccess(unlink(DRVFS_BASIC_PREFIX "/test"));
    LxtCheckErrnoZeroSuccess(rmdir(DRVFS_BASIC_PREFIX));

ErrorExit:
    if (Fd >= 0)
    {
        close(Fd);
    }

    unlink(DRVFS_BASIC_PREFIX "/node");
    unlink(DRVFS_BASIC_PREFIX "/test");
    rmdir(DRVFS_BASIC_PREFIX);

    return Result;
}

int DrvFsTestBlockCount(PLXT_ARGS Args)

/*++

Description:

    This routine tests the block count reported for files.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    char* Buffer;
    size_t BufferSize;
    int Fd;
    int Result;
    struct stat Stat;
    ssize_t Written;

    Buffer = 0;
    Fd = -1;
    LxtCheckErrnoZeroSuccess(mkdir(DRVFS_BASIC_PREFIX, 0777));

    //
    // Create the file.
    //

    LxtCheckErrno(Fd = creat(DRVFS_BASIC_PREFIX "/testfile", 0666));

    //
    // Block count should be zero for an empty file.
    //

    LxtCheckErrnoZeroSuccess(fstat(Fd, &Stat));
    LxtCheckEqual(Stat.st_size, 0, "%lld");
    LxtCheckEqual(Stat.st_blocks, 0, "%ld");

    //
    // Write data to the file. Make sure it's a whole number of NTFS blocks.
    //

    BufferSize = 2 * Stat.st_blksize;
    Buffer = malloc(BufferSize);
    LxtCheckNotEqual(Buffer, NULL, "%p");
    memset(Buffer, 0, BufferSize);
    LxtCheckErrno(Written = write(Fd, Buffer, BufferSize));
    LxtCheckEqual(Written, BufferSize, "%ld");

    //
    // Verify the block count, which should use 512 byte blocks regardless of
    // the reported block size.
    //
    // N.B. NTFS can decide to allocate more blocks, so an exact test isn't
    //      possible.
    //

    LxtCheckErrnoZeroSuccess(fstat(Fd, &Stat));
    LxtCheckEqual(Stat.st_size, BufferSize, "%lld");
    LxtCheckGreaterOrEqual(Stat.st_blocks, BufferSize / 512, "%ld");

    //
    // Write one more byte so the size isn't divisible by 512.
    //

    LxtCheckErrno(Written = write(Fd, Buffer, 1));
    LxtCheckEqual(Written, 1, "%ld");

    //
    // Verify the block count. Windows will have grown the file using its own
    // internal logic, so the exact block count is hard to predict.
    //

    LxtCheckErrnoZeroSuccess(fstat(Fd, &Stat));
    LxtCheckEqual(Stat.st_size, BufferSize + 1, "%lld");
    LxtCheckGreaterOrEqual(Stat.st_blocks, ((BufferSize + Stat.st_blksize) / 512), "%ld");

ErrorExit:
    if (Fd >= 0)
    {
        close(Fd);
    }

    unlink(DRVFS_BASIC_PREFIX "/testfile");
    rmdir(DRVFS_BASIC_PREFIX);
    return Result;
}

int DrvFsTestCaseSensitivity(PLXT_ARGS Args)

/*++

Routine Description:

    This routine tests the case-sensitivity support of DrvFS.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int Fd;
    int Result;
    struct stat StatA;
    struct stat StatB;

    LxtCheckErrnoZeroSuccess(mkdir(DRVFS_CS_PREFIX, 0777));

    //
    // Create 2 sub-directories differing only by case.
    //

    LxtCheckResult(mkdir(DRVFS_CS_PREFIX "/dir", 0777));
    LxtCheckResult(mkdir(DRVFS_CS_PREFIX "/Dir", 0777));

    //
    // Create 2 files differing only by case.
    //

    LxtCheckResult(Fd = open(DRVFS_CS_PREFIX "/file", O_RDWR | O_CREAT | O_EXCL, 0777));

    close(Fd);
    LxtCheckResult(Fd = open(DRVFS_CS_PREFIX "/File", O_RDWR | O_CREAT | O_EXCL, 0777));

    close(Fd);

    //
    // Stat both files and make sure the inode numbers are different.
    //

    LxtCheckErrnoZeroSuccess(stat(DRVFS_CS_PREFIX "/file", &StatA));
    LxtCheckErrnoZeroSuccess(stat(DRVFS_CS_PREFIX "/File", &StatB));
    LxtCheckNotEqual(StatA.st_ino, StatB.st_ino, "%llu");
    Result = 0;

ErrorExit:
    unlink(DRVFS_CS_PREFIX "/File");
    unlink(DRVFS_CS_PREFIX "/file");
    rmdir(DRVFS_CS_PREFIX "/Dir");
    rmdir(DRVFS_CS_PREFIX "/dir");
    rmdir(DRVFS_CS_PREFIX);

    return Result;
}

int DrvFsTestCaseSensitivityRoot(PLXT_ARGS Args)

/*++

Description:

    This routine tests whether drvfs correctly disables case sensitivity in the
    root of a drive for various operations.

    N.B. This test requires administrator privileges in Windows.

    N.B. Since the tests are run with case=dir, the root behaves as a case
         insensitive file system.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int Fd;
    int Result;
    struct stat Stat1;
    struct stat Stat2;

    //
    // Attempt to create two files that differ by case.
    //

    LxtCheckErrno(Fd = creat(DRVFS_PREFIX "/testfile", 0666));
    LxtCheckErrnoZeroSuccess(fstat(Fd, &Stat1));
    LxtCheckNotEqual(Stat1.st_ino, 0, "%llu");
    LxtCheckClose(Fd);
    LxtCheckErrnoFailure(open(DRVFS_PREFIX "/TestFile", O_CREAT | O_EXCL, 0666), EEXIST);

    //
    // Without O_EXCL, O_CREAT succeeds because this directory is case
    // insensitive. These should refer to the same file.
    //

    LxtCheckErrno(Fd = open(DRVFS_PREFIX "/TestFile", O_CREAT, 0666));
    LxtCheckErrnoZeroSuccess(fstat(Fd, &Stat2));
    LxtCheckEqual(Stat1.st_ino, Stat2.st_ino, "%llu");
    LxtCheckClose(Fd);

    //
    // O_CREAT with the original name should work, of course.
    //

    LxtCheckErrno(Fd = open(DRVFS_PREFIX "/testfile", O_CREAT, 0666));
    LxtCheckClose(Fd);
    LxtCheckErrnoFailure(mkdir(DRVFS_PREFIX "/TestFile", 0777), EEXIST);
    LxtCheckErrnoFailure(symlink(DRVFS_PREFIX "/testfile", DRVFS_PREFIX "/TestFile"), EEXIST);

    LxtCheckErrnoFailure(link(DRVFS_PREFIX "/testfile", DRVFS_PREFIX "/TestFile"), EEXIST);

    //
    // Verify mknod returns EEXIST because of the case collision.
    //

    LxtCheckErrnoFailure(mknod(DRVFS_PREFIX "/TestFile", S_IFREG | 0666, 0), EEXIST);

    //
    // Verify renaming to a different case.
    //

    LxtCheckErrnoZeroSuccess(mkdir(DRVFS_PREFIX "/testdir", 0777));
    LxtCheckErrnoZeroSuccess(stat(DRVFS_PREFIX "/testdir", &Stat1));
    LxtCheckErrnoZeroSuccess(rename(DRVFS_PREFIX "/testdir", DRVFS_PREFIX "/TestDir"));

    LxtCheckErrnoZeroSuccess(stat(DRVFS_PREFIX "/TestDir", &Stat2));
    LxtCheckEqual(Stat1.st_ino, Stat2.st_ino, "%llu");
    LxtCheckErrnoZeroSuccess(rmdir(DRVFS_PREFIX "/TestDir"));

ErrorExit:
    unlink(DRVFS_PREFIX "/TestFile");
    rmdir(DRVFS_PREFIX "/TestFile");
    unlink(DRVFS_PREFIX "/testfile");
    return Result;
}

int DrvFsTestDeleteCurrentWorkingDirectory(PLXT_ARGS Args)

/*++

Description:

    This routine tests the behavior if the current working directory is
    unlinked for DrvFs.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int Result;

    LxtCheckErrno(LxtFsDeleteCurrentWorkingDirectoryCommon(DRVFS_PREFIX, FS_DELETE_DRVFS));

ErrorExit:
    return Result;
}

int DrvFsTestDeleteLoop(PLXT_ARGS Args)

/*++

Description:

    This routine tests deleting files in a loop with multiple getdents calls.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int Result;

    LxtCheckResult(LxtFsDeleteLoopCommon(DRVFS_DELETELOOP_PREFIX));

ErrorExit:
    return Result;
}

int DrvFsTestDeleteOpenFile(PLXT_ARGS Args)

/*++

Description:

    This routine tests using unlink and rmdir on a DrvFs file/directory that's open.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int Result;

    LxtCheckErrno(LxtFsDeleteOpenFileCommon(DRVFS_PREFIX, FS_DELETE_DRVFS));

ErrorExit:
    return Result;
}

int DrvFsTestEscapedNames(PLXT_ARGS Args)

/*++

Description:

    This routine tests using file names that need to be escaped.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    LXT_CHILD_INFO Child;
    int Fd;
    int Result;
    struct stat Stat;
    struct stat Stat2;

    Fd = -1;
    LxtCheckErrnoZeroSuccess(mkdir(DRVFS_ESCAPE_TEST_DIR, 0777));

    //
    // Check creating a file and make sure it can be accessed.
    //

    LxtCheckErrno(Fd = creat(DRVFS_ESCAPE_TEST_CHILD, 0666));
    LxtCheckResult(LxtCheckFdPath(Fd, DRVFS_ESCAPE_TEST_CHILD));
    LxtCheckErrnoZeroSuccess(stat(DRVFS_ESCAPE_TEST_CHILD, &Stat));
    LxtCheckNotEqual(Stat.st_ino, 0, "%llu");

    //
    // It's possible to use the escaped characters directly.
    //

    LxtCheckErrnoZeroSuccess(stat(DRVFS_ESCAPE_TEST_CHILD_ESCAPED, &Stat2));
    LxtCheckEqual(Stat.st_ino, Stat2.st_ino, "%llu");

    //
    // Make sure the name appears correctly in the directory listing.
    //

    Child.Name = DRVFS_ESCAPE_TEST_CHILD_NAME;
    Child.FileType = DT_REG;
    LxtCheckResult(LxtCheckDirectoryContentsEx(DRVFS_ESCAPE_TEST_DIR, &Child, 1, 0));

    //
    // Check unlinking.
    //

    LxtCheckClose(Fd);
    LxtCheckErrnoZeroSuccess(unlink(DRVFS_ESCAPE_TEST_CHILD));

    //
    // Check various other ways of creating a file.
    //

    LxtCheckErrnoZeroSuccess(mkdir(DRVFS_ESCAPE_TEST_CHILD, 0777));
    LxtCheckErrnoZeroSuccess(rmdir(DRVFS_ESCAPE_TEST_CHILD));
    LxtCheckErrno(Fd = creat(DRVFS_ESCAPE_TEST_DIR "/target", 0666));
    LxtCheckClose(Fd);

    //
    // Test rename with an escape character in the source and target.
    //

    LxtCheckErrnoZeroSuccess(rename(DRVFS_ESCAPE_TEST_DIR "/target", DRVFS_ESCAPE_TEST_CHILD));

    LxtCheckErrnoZeroSuccess(rename(DRVFS_ESCAPE_TEST_CHILD, DRVFS_ESCAPE_TEST_DIR "/target"));

    //
    // Symlinks are not supported on SMB or FAT.
    //

    if ((DrvFsTestMode != DRVFS_FAT_TEST_MODE) && (DrvFsTestMode != DRVFS_SMB_TEST_MODE))
    {

        LxtCheckErrnoZeroSuccess(symlink("target", DRVFS_ESCAPE_TEST_CHILD));
        LxtCheckErrnoZeroSuccess(unlink(DRVFS_ESCAPE_TEST_CHILD));
    }

    //
    // Hard links are not supported on FAT.
    //

    if (DrvFsTestMode != DRVFS_FAT_TEST_MODE)
    {
        LxtCheckErrnoZeroSuccess(link(DRVFS_ESCAPE_TEST_DIR "/target", DRVFS_ESCAPE_TEST_CHILD));

        LxtCheckErrnoZeroSuccess(unlink(DRVFS_ESCAPE_TEST_CHILD));
    }

    //
    // Check mknod if metadata is supported.
    //

    if (DrvFsTestMode == DRVFS_METADATA_TEST_MODE)
    {
        LxtCheckErrnoZeroSuccess(mknod(DRVFS_ESCAPE_TEST_CHILD, S_IFCHR, makedev(1, 3)));

        LxtCheckErrnoZeroSuccess(unlink(DRVFS_ESCAPE_TEST_CHILD));
    }

ErrorExit:
    if (Fd >= 0)
    {
        close(Fd);
    }

    unlink(DRVFS_ESCAPE_TEST_DIR "/target");
    unlink(DRVFS_ESCAPE_TEST_CHILD);
    rmdir(DRVFS_ESCAPE_TEST_CHILD);
    rmdir(DRVFS_ESCAPE_TEST_DIR);
    return Result;
}

int DrvFsTestExecve(PLXT_ARGS Args)

/*++

Description:

    This routine tests execve on DrvFs.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    char* Argv[4];
    pid_t ChildPid;
    char* Envp[1];
    int Result;

    //
    // Check the mode of the files.
    //

    LxtCheckResult(DrvFsCheckMode(DRVFS_ACCESS_EXECUTEONLY_TEST_FILE, S_IFREG | S_IRUGO | S_IXUGO));

    LxtCheckResult(DrvFsCheckMode(DRVFS_ACCESS_READONLY_TEST_FILE, S_IFREG | S_IRUGO));

    //
    // Fork a child.
    //

    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {

        Argv[0] = DRVFS_ACCESS_EXECUTEONLY_TEST_FILE;
        Argv[1] = "drvfs";
        Argv[2] = "execvetest";
        Argv[3] = NULL;
        Envp[0] = NULL;

        //
        // Attempt to execve the read only file, which should fail.
        //

        LxtCheckErrnoFailure(execve(DRVFS_ACCESS_READONLY_TEST_FILE, Argv, Envp), EACCES);

        //
        // Attempt to execve the execute only file, which should succeed even
        // though the user has no read access.
        //

        LxtCheckErrno(execve(DRVFS_ACCESS_EXECUTEONLY_TEST_FILE, Argv, Envp));

        LxtLogError("Execve returned");
        _exit(LXT_RESULT_FAILURE);
    }

    LxtCheckResult(LxtWaitPidPoll(ChildPid, DRVFS_EXECVE_TEST_RESULT << 8));

ErrorExit:
    return Result;
}

int DrvFsTestFatCaseInsensitive(PLXT_ARGS Args)

/*++

Description:

    This routine tests the case-insensitive behavior of FAT.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    const LXT_CHILD_INFO Children[] = {"foo", DT_REG};
    const LXT_CHILD_INFO ChildrenPlan9Smb[] = {"FOO", DT_REG};
    int Fd;
    int Fd2;
    int Result;
    struct stat Stat1;
    struct stat Stat2;

    Fd = -1;
    Fd2 = -1;

    LxtCheckErrnoZeroSuccess(mkdir(DRVFS_CASE_INSENSITIVE_TEST_DIR, 0777));

    //
    // Create a file.
    //

    LxtCheckErrno(Fd = creat(DRVFS_CASE_INSENSITIVE_TEST_DIR "/foo", 0666));
    LxtCheckClose(Fd);

    //
    // Stat the file with its original name and a different case, and verify
    // they are the same file.
    //

    LxtCheckErrno(lstat(DRVFS_CASE_INSENSITIVE_TEST_DIR "/foo", &Stat1));
    LxtCheckErrno(lstat(DRVFS_CASE_INSENSITIVE_TEST_DIR "/FOO", &Stat2));
    LxtCheckEqual(Stat1.st_ino, Stat2.st_ino, "%lld");

    //
    // Check name collisions on create.
    //

    LxtCheckErrnoFailure(Fd2 = open(DRVFS_CASE_INSENSITIVE_TEST_DIR "/FOO", O_CREAT | O_EXCL, 0666), EEXIST);

    LxtCheckErrnoFailure(mkdir(DRVFS_CASE_INSENSITIVE_TEST_DIR "/FOO", 0666), EEXIST);

    //
    // Rename to the same file, which works but has no effect (the case is not
    // changed). This matches real Linux, and normal Windows behavior when
    // renaming on FAT.
    //
    // N.B. With SMB over Plan 9, because Linux doesn't know the file system is
    //      case-insensitive and NTFS does let you change the case on rename,
    //      this actually does change the case.
    //

    LxtCheckErrnoZeroSuccess(rename(DRVFS_CASE_INSENSITIVE_TEST_DIR "/foo", DRVFS_CASE_INSENSITIVE_TEST_DIR "/FOO"));

    //
    // If the file is opened with two different cases, both fd's report the
    // path with the first case. This behavior matches Linux, except Linux
    // "remembers" the first case used after the file is closed because the
    // directory entries are cached.
    //
    // N.B. This is not the case with Plan 9 because Linux doesn't know the
    //      file system is case-insensitive.
    //

    if (g_LxtFsInfo.FsType != LxtFsTypePlan9)
    {
        LxtCheckErrno(Fd = open(DRVFS_CASE_INSENSITIVE_TEST_DIR "/foo", O_RDONLY));
        LxtCheckErrno(Fd2 = open(DRVFS_CASE_INSENSITIVE_TEST_DIR "/FOO", O_RDONLY));
        LxtCheckResult(LxtCheckFdPath(Fd, DRVFS_CASE_INSENSITIVE_TEST_DIR "/foo"));
        LxtCheckResult(LxtCheckFdPath(Fd2, DRVFS_CASE_INSENSITIVE_TEST_DIR "/foo"));
    }

    //
    // Listing the directory shows the file with the correct case.
    //
    // N.B. As remarked above, for SMB on Plan 9, the case will have changed.
    //

    if ((g_LxtFsInfo.FsType == LxtFsTypePlan9) && (DrvFsTestMode == DRVFS_SMB_TEST_MODE))
    {

        LxtCheckResult(LxtCheckDirectoryContentsEx(DRVFS_CASE_INSENSITIVE_TEST_DIR, ChildrenPlan9Smb, LXT_COUNT_OF(Children), 0));
    }
    else
    {
        LxtCheckResult(LxtCheckDirectoryContentsEx(DRVFS_CASE_INSENSITIVE_TEST_DIR, Children, LXT_COUNT_OF(Children), 0));
    }

ErrorExit:
    if (Fd >= 0)
    {
        close(Fd);
    }

    if (Fd2 >= 0)
    {
        close(Fd2);
    }

    unlink(DRVFS_CASE_INSENSITIVE_TEST_DIR "/foo");
    rmdir(DRVFS_CASE_INSENSITIVE_TEST_DIR);
    return Result;
}

int DrvFsTestFatJunction(PLXT_ARGS Args)

/*++

Description:

    This routine tests whether the NTFS mount point for the FAT volume can be
    used as a symlink to the mounted volume.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int Result;

    //
    // This test does not apply to VM mode because Plan 9 doesn't support
    // junction point symlinks.
    //

    if (g_LxtFsInfo.FsType == LxtFsTypePlan9)
    {
        LxtLogInfo("This test is not relevant in VM mode.");
        Result = 0;
        goto ErrorExit;
    }

    //
    // Because the real C: drive was unmounted to mount the FAT volume, mount
    // it in a different location.
    //

    LxtCheckErrnoZeroSuccess(mkdir(DRVFS_MOUNT_TEST_DIR, 0777));
    LxtCheckErrnoZeroSuccess(mount(DRVFS_DRIVE, DRVFS_MOUNT_TEST_DIR, DRVFS_FS_TYPE, DRVFS_MOUNT_OPTIONS, NULL));

    LxtCheckResult(LxtCheckLinkTarget(DRVFS_MOUNT_TEST_DIR "/" DRVFS_FAT_MOUNT_POINT, DRVFS_PREFIX "/"));

ErrorExit:
    umount(DRVFS_MOUNT_TEST_DIR);
    rmdir(DRVFS_MOUNT_TEST_DIR);
    return Result;
}

int DrvFsTestFatUnsupported(PLXT_ARGS Args)

/*++

Description:

    This routine tests unsupported functionality on FAT.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    return DrvFsTestUnsupportedCommon(DRVFS_FAT_TEST_MODE);
}

int DrvFsTestFatUtimensat(PLXT_ARGS Args)

/*++

Description:

    This routine tests the utimensat system call on drvfs for FAT volumes.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    return DrvFsTestUtimensatCommon(FS_UTIME_FAT | FS_UTIME_NO_SYMLINKS);
}

int DrvFsTestFatWslPath(PLXT_ARGS Args)

/*++

Description:

    This routine tests using the wslpath utility against the FAT mount point.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int Result;

    //
    // Previous tests may have messed up the cwd such that getcwd fails, which
    // wslpath won't like.
    //

    LxtCheckErrnoZeroSuccess(chdir("/"));

    //
    // The FAT volume in Windows is mounted on an NTFS directory mount, which
    // makes it an interesting case to test with wslpath.
    //

    LxtCheckResult(LxtCheckWslPathTranslation(DRVFS_FAT_DRIVE, DRVFS_PREFIX, true));
    LxtCheckResult(LxtCheckWslPathTranslation(DRVFS_PREFIX, "C:\\" DRVFS_FAT_MOUNT_POINT, false));

ErrorExit:
    return Result;
}

int DrvFsTestFstat(PLXT_ARGS Args)

/*++

Description:

    This routine tests the fstat system call on drvfs files.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int Fd;
    int OPathFd;
    int Result;
    struct stat Stat1;
    struct stat Stat2;

    Fd = -1;
    OPathFd = -1;

    //
    // Create a test file.
    //

    LxtCheckErrnoZeroSuccess(mkdir(DRVFS_BASIC_PREFIX, 0777));
    LxtCheckErrno(Fd = creat(DRVFS_BASIC_PREFIX "/testfile", 0666));
    LxtCheckErrno(OPathFd = open(DRVFS_BASIC_PREFIX "/testfile", O_PATH));

    //
    // Stat and fstat should have the same result.
    //

    LxtCheckErrnoZeroSuccess(stat(DRVFS_BASIC_PREFIX "/testfile", &Stat1));
    LxtCheckErrnoZeroSuccess(fstat(Fd, &Stat2));
    LxtCheckMemoryEqual(&Stat1, &Stat2, sizeof(Stat1));
    LxtCheckErrnoZeroSuccess(fstat(OPathFd, &Stat2));
    LxtCheckMemoryEqual(&Stat1, &Stat2, sizeof(Stat1));

    //
    // Fstat should still work after unlink.
    //
    // N.B. This currently doesn't work on plan 9.
    //

    LxtCheckErrnoZeroSuccess(unlink(DRVFS_BASIC_PREFIX "/testfile"));
    LxtCheckErrnoFailure(stat(DRVFS_BASIC_PREFIX "/testfile", &Stat2), ENOENT);
    if (g_LxtFsInfo.FsType != LxtFsTypePlan9)
    {
        LxtCheckErrnoZeroSuccess(fstat(Fd, &Stat2));

        //
        // The result should be the same except for link count and time stamps.
        //
        // N.B. On FAT, which does not support posix unlink, the link count will
        //      still be one.
        //

        LxtCheckEqual(Stat1.st_ino, Stat2.st_ino, "%llu");
        LxtCheckEqual(Stat1.st_size, Stat2.st_size, "%llu");
        LxtCheckEqual(Stat1.st_mode, Stat2.st_mode, "0%o");
        if (DrvFsTestMode == DRVFS_FAT_TEST_MODE)
        {
            LxtCheckEqual(Stat1.st_nlink, Stat2.st_nlink, "%lu");
        }
        else
        {
            LxtCheckNotEqual(Stat1.st_nlink, Stat2.st_nlink, "%lu");
            LxtCheckEqual(Stat2.st_nlink, 0l, "%lu");
        }

        //
        // Check fstat on the O_PATH descriptor.
        //

        LxtCheckErrnoZeroSuccess(fstat(OPathFd, &Stat1));
        LxtCheckMemoryEqual(&Stat2, &Stat1, sizeof(Stat1));

        //
        // Fstatat should still work after unlink.
        //

        LxtCheckErrnoZeroSuccess(fstatat(Fd, "", &Stat1, AT_EMPTY_PATH));
        LxtCheckMemoryEqual(&Stat2, &Stat1, sizeof(Stat1));
        LxtCheckErrnoZeroSuccess(fstatat(OPathFd, "", &Stat1, AT_EMPTY_PATH));
        LxtCheckMemoryEqual(&Stat2, &Stat1, sizeof(Stat1));
    }

ErrorExit:
    if (Fd >= 0)
    {
        close(Fd);
    }

    if (OPathFd >= 0)
    {
        close(OPathFd);
    }

    unlink(DRVFS_BASIC_PREFIX "/testfile");
    rmdir(DRVFS_BASIC_PREFIX);
    return Result;
}

int DrvFsTestGetDents64Alignment(PLXT_ARGS Args)

/*++

Description:

    This routine tests whether directory entries are correctly aligned and
    padded.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int Result;

    LxtCheckResult(LxtFsGetDentsAlignmentCommon(DRVFS_GETDENTS_PREFIX, FS_TEST_GETDENTS64));

ErrorExit:
    return Result;
}

int DrvFsTestGetDentsAlignment(PLXT_ARGS Args)

/*++

Description:

    This routine tests whether directory entries are correctly aligned and
    padded.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int Result;

    LxtCheckResult(LxtFsGetDentsAlignmentCommon(DRVFS_GETDENTS_PREFIX, 0));

ErrorExit:
    return Result;
}

int DrvFsTestHardLinks(PLXT_ARGS Args)

/*++

Description:

    This routine tests the creation of hard links.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int Fd;
    int Result;
    struct stat Stat1;
    struct stat Stat2;

    LxtCheckErrnoZeroSuccess(mkdir(DRVFS_HARDLINK_TEST_DIR, 0777));
    LxtCheckErrno(Fd = creat(DRVFS_HARDLINK_TEST_DIR "/target", 0666));
    LxtCheckClose(Fd);
    LxtCheckErrnoZeroSuccess(link(DRVFS_HARDLINK_TEST_DIR "/target", DRVFS_HARDLINK_TEST_DIR "/link"));

    LxtCheckErrnoZeroSuccess(lstat(DRVFS_HARDLINK_TEST_DIR "/target", &Stat1));
    LxtCheckErrnoZeroSuccess(lstat(DRVFS_HARDLINK_TEST_DIR "/link", &Stat2));
    LxtCheckEqual(Stat1.st_ino, Stat2.st_ino, "%lld");

ErrorExit:
    unlink(DRVFS_HARDLINK_TEST_DIR "/link");
    unlink(DRVFS_HARDLINK_TEST_DIR "/target");
    rmdir(DRVFS_HARDLINK_TEST_DIR);
    return Result;
}

int DrvFsTestHiddenLxFsDirs(PLXT_ARGS Args)

/*++

Description:

    This routine tests whether various VoLFs mounts are inaccessible from
    DrvFs.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    char Child[4096];
    int Children;
    DIR* Dir;
    struct dirent* Entry;
    void* PointerResult;
    int Result;
    char TempDirectory[4096];

    Dir = NULL;

    if (g_LxtFsInfo.FsType == LxtFsTypePlan9)
    {
        LxtLogInfo("This test is not relevant in VM mode.");
        Result = 0;
        goto ErrorExit;
    }

    if (VfsAccessLxssDir == NULL)
    {
        LxtLogError("Lxss directory not specified.");
        Result = LXT_RESULT_FAILURE;
        goto ErrorExit;
    }

    LxtCheckResult(DrvFsTestHiddenLxFsDirsHelper("rootfs", TRUE));
    LxtCheckResult(DrvFsTestHiddenLxFsDirsHelper("rootfs/etc", FALSE));
    LxtCheckResult(DrvFsTestHiddenLxFsDirsHelper("rootfs/cache", FALSE));
    LxtCheckResult(DrvFsTestHiddenLxFsDirsHelper("rootfs/data", FALSE));
    LxtCheckResult(DrvFsTestHiddenLxFsDirsHelper("rootfs/home", FALSE));
    LxtCheckResult(DrvFsTestHiddenLxFsDirsHelper("rootfs/mnt", FALSE));
    LxtCheckResult(DrvFsTestHiddenLxFsDirsHelper("rootfs/root", FALSE));

    //
    // Ensure that the temp directory cannot be opened.
    //
    // N.B. There should only be a single directory entry under the temp
    //      directory. The only case when this will not be true is if handles
    //      were leaked by a previous instance and are still open by the driver.
    //

    Children = 0;
    sprintf(TempDirectory, "%s/%s", VfsAccessLxssDir, "temp");
    LxtCheckNullErrno(Dir = opendir(TempDirectory));
    while ((Entry = readdir(Dir)) != NULL)
    {
        if ((strcmp(Entry->d_name, ".") == 0) || (strcmp(Entry->d_name, "..") == 0))
        {

            continue;
        }

        sprintf(Child, "temp/%s", Entry->d_name);
        LxtCheckResult(DrvFsTestHiddenLxFsDirsHelper(Child, TRUE));
        Children += 1;
    }

    LxtCheckEqual(Children, 1, "%d");

ErrorExit:
    if (Dir != NULL)
    {
        closedir(Dir);
    }

    return Result;
}

int DrvFsTestHiddenLxFsDirsHelper(const char* Child, BOOLEAN DirectChild)

/*++

Description:

    This routine tests if the specified child of the LXSS directory is
    inaccessible.

Arguments:

    Child - Supplies the path of the child.

    DirectChild - Supplies a value that indicates whether the child name is
        a direct child.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int Fd;
    char Path[4096];
    int Result;
    struct stat Stat;

    Fd = -1;
    sprintf(Path, "%s/%s", VfsAccessLxssDir, Child);
    LxtLogInfo("Attempting to access %s", Path);

    //
    // Open directly.
    //

    LxtCheckErrnoFailure(open(Path, O_RDONLY), EACCES);

    //
    // Open through openat.
    //

    LxtCheckErrno(Fd = open(VfsAccessLxssDir, O_RDONLY | O_DIRECTORY));
    LxtCheckErrnoFailure(openat(Fd, Child, O_RDONLY), EACCES);
    LxtCheckErrnoZeroSuccess(close(Fd));
    Fd = -1;

    //
    // On a direct child O_PATH and stat should work. On a deeper descendant
    // it will not.
    //

    if (DirectChild != FALSE)
    {
        LxtCheckErrno(Fd = open(Path, O_PATH));
        LxtCheckErrnoZeroSuccess(close(Fd));
        Fd = -1;
        LxtCheckErrnoZeroSuccess(stat(Path, &Stat));
        LxtCheckEqual((Stat.st_mode & ~S_IFMT), 0, "%o");
    }
    else
    {
        LxtCheckErrnoFailure(open(Path, O_PATH), EACCES);
        LxtCheckErrnoFailure(stat(Path, &Stat), EACCES);
    }

ErrorExit:
    if (Fd >= 0)
    {
        close(Fd);
    }

    return Result;
}

int DrvFsTestInotifyBasic(PLXT_ARGS Args)

{

    int Id;
    int Result;
    int Wd[10];

    //
    // Test watching basic Drvfs paths.
    //

    LxtCheckErrno(Id = inotify_init());
    LxtCheckErrno(Wd[0] = inotify_add_watch(Id, DRVFS_PREFIX, IN_ALL_EVENTS));
    LxtCheckErrno(Wd[1] = inotify_add_watch(Id, DRVFS_PREFIX "/Users", IN_ALL_EVENTS));
    LxtCheckErrno(Wd[2] = inotify_add_watch(Id, DRVFS_PREFIX "/Windows", IN_ALL_EVENTS));
    LxtCheckErrno(Wd[3] = inotify_add_watch(Id, DRVFS_PREFIX "/Windows/System32", IN_ALL_EVENTS));
    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    close(Id);
    return Result;
}

int DrvFsTestInotifyEpoll(PLXT_ARGS Args)

{

    //
    // TODO: Investigate why this doesn't work on Plan 9. May be a Linux 9p bug.
    //

    if (g_LxtFsInfo.FsType == LxtFsTypePlan9)
    {
        LxtLogInfo("This test fails in VM mode.");
        return 0;
    }

    return LxtFsInotifyEpollCommon(DRVFS_INOTIFY_TEST_BASE_DIR);
}

int DrvFsTestInotifyPosixUnlinkRename(PLXT_ARGS Args)

{

    return LxtFsInotifyPosixUnlinkRenameCommon(DRVFS_INOTIFY_TEST_BASE_DIR);
}

int DrvFsTestInotifyStressUnlinkRename(PLXT_ARGS Args)

/*++
--*/

{

    int Fd;
    int Id;
    char Buf[10];
    int Result;
    int Index;
    int UnlinkIndex;
    int Tests;
    char BaseDir[PATH_MAX];
    char TestDir[PATH_MAX];
    int ChildPid;
    int SignalFd;
    struct signalfd_siginfo SignalInfo;
    sigset_t SignalMask;
    int Status;
    char TestFiles[DRVFS_INOTIFY_STRESS_NUM_FILES][PATH_MAX];
    bool UseDirs;

    //
    // TODO: Remove once the vb test image has the fix.
    //

    const char* disableTest = getenv("WSL_DISABLE_VB_UNSTABLE_TESTS");
    if (disableTest != NULL && strcmp(disableTest, "1") == 0)
    {
        LxtLogInfo("WSL_DISABLE_VB_UNSTABLE_TESTS set, skipping inotify stress test");
        return 0;
    }

    //
    // Initialize and also do cleanup if the files have not been removed.
    //

    sprintf(BaseDir, "%s", DRVFS_INOTIFY_TEST_BASE_DIR);
    sprintf(TestDir, "%s%s", BaseDir, DRVFS_INOTIFY_STRESS_DIR);
    LxtCheckErrnoZeroSuccess(mkdir(BaseDir, 0777));
    LxtCheckErrnoZeroSuccess(mkdir(TestDir, 0777));
    for (Index = 0; Index < DRVFS_INOTIFY_STRESS_NUM_FILES; Index++)
    {
        sprintf(TestFiles[Index], "%sunlink_%d", TestDir, Index);
    }

    //
    // One thread repeatedly adds and removes inotify watches.
    // Another thread repeatedly creates, modifies, renames, and unlinks the files.
    //

    //
    // LX_TODO: There is a race in some scenarios where a previously deleted
    //          directory is still being torn down and will cause creation of a
    //          file of the same name to fail. In the below loop this can result
    //          in spurious errors.
    //

#if 0
#define WITH_ERROR(_op_) LxtCheckErrno(_op_)
#else
#define WITH_ERROR(_op_) _op_
#endif

    UseDirs = false;
    LXT_SYNCHRONIZATION_POINT_START();
    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        LxtCheckResult(LxtSignalBlock(SIGQUIT));
        sigemptyset(&SignalMask);
        sigaddset(&SignalMask, SIGQUIT);
        LxtCheckErrno(SignalFd = signalfd(-1, &SignalMask, SFD_NONBLOCK));
        while (1)
        {
            for (Index = 0; Index < DRVFS_INOTIFY_STRESS_NUM_FILES; Index++)
            {
                if (UseDirs)
                {
                    WITH_ERROR(mkdir(TestFiles[Index], 0777));
                }
                else
                {
                    WITH_ERROR(Fd = open(TestFiles[Index], O_CREAT | O_RDWR, 0600));
                }

                LXT_SYNCHRONIZATION_POINT();
                if (read(SignalFd, &SignalInfo, sizeof(SignalInfo)) > 0)
                {
                    LxtLogInfo("Exiting child on signal");
                    goto ErrorExit;
                }
                else if (errno != EAGAIN)
                {
                    LxtLogError("Read of signalfd gave unexpected error: %d", errno);
                    Result = -1;
                    goto ErrorExit;
                }

                usleep(random() % 50);
                if (!UseDirs)
                {
                    write(Fd, Buf, 10);
                    close(Fd);
                }

                if (random() % 2 == 0)
                {
                    UnlinkIndex = (Index + 1) % DRVFS_INOTIFY_STRESS_NUM_FILES;
                    WITH_ERROR(rename(TestFiles[Index], TestFiles[UnlinkIndex]));
                }
                else
                {
                    UnlinkIndex = Index;
                }

                if (UseDirs)
                {
                    WITH_ERROR(rmdir(TestFiles[UnlinkIndex]));
                }
                else
                {
                    WITH_ERROR(unlink(TestFiles[UnlinkIndex]));
                }
            }

            UseDirs = !UseDirs;
        }
    }

    for (Tests = 0; Tests < DRVFS_INOTIFY_STRESS_NUM_TESTS; Tests++)
    {
        Id = inotify_init();
        for (Index = 0; Index < DRVFS_INOTIFY_STRESS_NUM_FILES; Index++)
        {
            LXT_SYNCHRONIZATION_POINT();
            inotify_add_watch(Id, TestFiles[Index], IN_ALL_EVENTS);
        }

        close(Id);
    }

    kill(ChildPid, SIGQUIT);
    LXT_SYNCHRONIZATION_POINT();

ErrorExit:
    LXT_SYNCHRONIZATION_POINT_END();

    for (Index = 0; Index < DRVFS_INOTIFY_STRESS_NUM_FILES; Index++)
    {
        rmdir(TestFiles[Index]);
        unlink(TestFiles[Index]);
    }

    rmdir(TestDir);
    rmdir(BaseDir);
    return Result;
}

int DrvFsTestInotifyUnmountBind(PLXT_ARGS Args)

{

    return LxtFsInotifyUnmountBindCommon(DRVFS_INOTIFY_TEST_BASE_DIR);
}

int DrvFsTestLookupPath(PLXT_ARGS Args)

/*++

Description:

    This routine tests path lookup for drive FS, testing corner cases of the
    fast path lookup.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int Fd;
    int Fd2;
    int Result;

    //
    // Make test directories.
    //

    Fd = 0;
    Fd2 = 0;
    LxtCheckErrnoZeroSuccess(mkdir((DRVFS_PREFIX "/a"), 0777));
    LxtCheckErrnoZeroSuccess(mkdir((DRVFS_PREFIX "/a/b"), 0777));
    LxtCheckErrnoZeroSuccess(mkdir((DRVFS_PREFIX "/a/b/c"), 0777));
    LxtCheckErrnoZeroSuccess(mkdir((DRVFS_PREFIX "/a/b/c/d"), 0777));
    LxtCheckErrnoZeroSuccess(mkdir((DRVFS_PREFIX "/a/b/c/d/e"), 0777));
    LxtCheckErrnoZeroSuccess(mkdir((DRVFS_PREFIX "/a/b/c/d/e/f"), 0777));

    //
    // Open a directory, which should use fast path lookup.
    //

    LxtCheckErrno(Fd = open(DRVFS_PREFIX "/a/b/c/d/e", O_RDONLY | O_DIRECTORY));

    LxtCheckResult(LxtCheckFdPath(Fd, DRVFS_PREFIX "/a/b/c/d/e"));

    //
    // Open the parent of the directory.
    //

    LxtCheckErrno(Fd2 = openat(Fd, "..", O_RDONLY | O_DIRECTORY));
    LxtCheckResult(LxtCheckFdPath(Fd2, DRVFS_PREFIX "/a/b/c/d"));
    LxtCheckErrno(close(Fd2));

    //
    // Open two levels up.
    //

    LxtCheckErrno(Fd2 = openat(Fd, "../..", O_RDONLY | O_DIRECTORY));
    LxtCheckResult(LxtCheckFdPath(Fd2, DRVFS_PREFIX "/a/b/c"));
    LxtCheckErrno(close(Fd2));

    //
    // Open three levels up.
    //

    LxtCheckErrno(Fd2 = openat(Fd, "../../..", O_RDONLY | O_DIRECTORY));
    LxtCheckResult(LxtCheckFdPath(Fd2, DRVFS_PREFIX "/a/b"));
    LxtCheckErrno(close(Fd2));

    //
    // Up and down.
    //

    LxtCheckErrno(Fd2 = openat(Fd, "f/../../..", O_RDONLY | O_DIRECTORY));
    LxtCheckResult(LxtCheckFdPath(Fd2, DRVFS_PREFIX "/a/b/c"));
    LxtCheckErrno(close(Fd2));

    //
    // Up beyond the fast path entry.
    //

    LxtCheckErrno(Fd2 = openat(Fd, "../../../../..", O_RDONLY | O_DIRECTORY));
    LxtCheckResult(LxtCheckFdPath(Fd2, DRVFS_PREFIX));
    LxtCheckErrno(close(Fd2));
    LxtCheckErrno(Fd2 = openat(Fd, "../../../../../..", O_RDONLY | O_DIRECTORY));
    LxtCheckResult(LxtCheckFdPath(Fd2, "/mnt"));
    LxtCheckErrno(close(Fd2));

    //
    // Create a file with a relative path
    //

    LxtCheckErrno(Fd2 = openat(Fd, "../../../foo", O_RDWR | O_CREAT, 0666));
    LxtCheckResult(LxtCheckFdPath(Fd2, DRVFS_PREFIX "/a/b/foo"));
    LxtCheckErrno(close(Fd2));

ErrorExit:
    if (Fd > 0)
    {
        close(Fd);
    }

    if (Fd2 > 0)
    {
        close(Fd2);
    }

    unlink(DRVFS_PREFIX "/a/b/foo");
    rmdir(DRVFS_PREFIX "/a/b/c/d/e/f");
    rmdir(DRVFS_PREFIX "/a/b/c/d/e");
    rmdir(DRVFS_PREFIX "/a/b/c/d");
    rmdir(DRVFS_PREFIX "/a/b/c");
    rmdir(DRVFS_PREFIX "/a/b");
    rmdir(DRVFS_PREFIX "/a");
    return Result;
}

int DrvFsTestMetadata(PLXT_ARGS Args)

/*++

Description:

    This routine tests basic metadata functionality.

    N.B. This test uses the metadata directory created by the Windows side.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    pid_t ChildPid;
    int Fd;
    int Result;

    Fd = -1;
    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        LxtCheckErrno(LxtSetfsuid(2000));
        LxtCheckErrno(LxtSetfsgid(2001));

        //
        // Make sure umask won't change the mode values.
        //

        LxtCheckErrno(umask(0));

        //
        // Check if file creation uses the thread's owner information and the
        // specified mode.
        //

        LxtCheckErrno(Fd = creat(DRVFS_METADATA_TEST_DIR "/testfile", 0644));
        LxtCheckClose(Fd);
        LxtCheckResult(DrvFsCheckStat(DRVFS_METADATA_TEST_DIR "/testfile", 2000, 2001, S_IFREG | 0644, 0));

        //
        // Same for a directory.
        //

        LxtCheckErrnoZeroSuccess(mkdir(DRVFS_METADATA_TEST_DIR "/testdir", 0755));
        LxtCheckResult(DrvFsCheckStat(DRVFS_METADATA_TEST_DIR "/testdir", 2000, 2001, S_IFDIR | 0755, 0));

        //
        // Same for symlinks.
        //

        LxtCheckErrnoZeroSuccess(symlink(DRVFS_METADATA_TEST_DIR "/testfile", DRVFS_METADATA_TEST_DIR "/testlink"));

        LxtCheckResult(DrvFsCheckStat(DRVFS_METADATA_TEST_DIR "/testlink", 2000, 2001, S_IFLNK | 0777, 0));

        //
        // And files created by mknod.
        //

        LxtCheckErrnoZeroSuccess(mknod(DRVFS_METADATA_TEST_DIR "/testnodereg", S_IFREG | 0640, 0));

        LxtCheckResult(DrvFsCheckStat(DRVFS_METADATA_TEST_DIR "/testnodereg", 2000, 2001, S_IFREG | 0640, 0));

        LxtCheckErrnoZeroSuccess(mknod(DRVFS_METADATA_TEST_DIR "/testnodefifo", S_IFIFO | 0660, 0));

        LxtCheckResult(DrvFsCheckStat(DRVFS_METADATA_TEST_DIR "/testnodefifo", 2000, 2001, S_IFIFO | 0660, 0));

        LxtCheckErrnoZeroSuccess(mknod(DRVFS_METADATA_TEST_DIR "/testnodesock", S_IFSOCK | 0600, 0));

        LxtCheckResult(DrvFsCheckStat(DRVFS_METADATA_TEST_DIR "/testnodesock", 2000, 2001, S_IFSOCK | 0600, 0));

        exit(0);
    }

    LxtCheckResult(LxtWaitPidPoll(ChildPid, 0));

    //
    // Device files are not tested in the child because the required capability
    // is dropped when setfsuid is used.
    //

    LxtCheckErrnoZeroSuccess(mknod(DRVFS_METADATA_TEST_DIR "/testnodechr", S_IFCHR | 0666, makedev(1, 2)));

    LxtCheckResult(DrvFsCheckStat(DRVFS_METADATA_TEST_DIR "/testnodechr", 0, 0, S_IFCHR | 0666, makedev(1, 2)));

    LxtCheckErrnoZeroSuccess(mknod(DRVFS_METADATA_TEST_DIR "/testnodeblk", S_IFBLK | 0606, makedev(3, 4)));

    LxtCheckResult(DrvFsCheckStat(DRVFS_METADATA_TEST_DIR "/testnodeblk", 0, 0, S_IFBLK | 0606, makedev(3, 4)));

    //
    // Check using chmod.
    //

    LxtCheckErrnoZeroSuccess(chmod(DRVFS_METADATA_TEST_DIR "/testfile", 0400));
    LxtCheckResult(DrvFsCheckStat(DRVFS_METADATA_TEST_DIR "/testfile", 2000, 2001, S_IFREG | 0400, 0));

    //
    // Check using chown.
    //

    LxtCheckErrnoZeroSuccess(chown(DRVFS_METADATA_TEST_DIR "/testfile", 3000, 3001));

    LxtCheckResult(DrvFsCheckStat(DRVFS_METADATA_TEST_DIR "/testfile", 3000, 3001, S_IFREG | 0400, 0));

    //
    // Chown with no changes should succeed.
    //

    LxtCheckErrnoZeroSuccess(chown(DRVFS_METADATA_TEST_DIR "/testfile", -1, -1));
    LxtCheckResult(DrvFsCheckStat(DRVFS_METADATA_TEST_DIR "/testfile", 3000, 3001, S_IFREG | 0400, 0));

    //
    // Set the set-group-id bit on the directory.
    //

    LxtCheckErrnoZeroSuccess(chmod(DRVFS_METADATA_TEST_DIR "/testdir", S_ISGID | 0755));

    LxtCheckResult(DrvFsCheckStat(DRVFS_METADATA_TEST_DIR "/testdir", 2000, 2001, S_IFDIR | S_ISGID | 0755, 0));

    //
    // Check items created in the directory inherit the group id.
    //

    LxtCheckErrno(Fd = creat(DRVFS_METADATA_TEST_DIR "/testdir/childfile", 0600));

    LxtCheckClose(Fd);
    LxtCheckResult(DrvFsCheckStat(DRVFS_METADATA_TEST_DIR "/testdir/childfile", 0, 2001, S_IFREG | 0600, 0));

    LxtCheckErrnoZeroSuccess(mkdir(DRVFS_METADATA_TEST_DIR "/testdir/childdir", 0700));

    LxtCheckResult(DrvFsCheckStat(DRVFS_METADATA_TEST_DIR "/testdir/childdir", 0, 2001, S_IFDIR | S_ISGID | 0700, 0));

    //
    // The set-user-id bit doesn't have that effect.
    //

    LxtCheckErrnoZeroSuccess(chmod(DRVFS_METADATA_TEST_DIR "/testdir", S_ISUID | 0755));

    LxtCheckResult(DrvFsCheckStat(DRVFS_METADATA_TEST_DIR "/testdir", 2000, 2001, S_IFDIR | S_ISUID | 0755, 0));

    LxtCheckErrno(Fd = creat(DRVFS_METADATA_TEST_DIR "/testdir/childfile2", 0600));

    LxtCheckClose(Fd);
    LxtCheckResult(DrvFsCheckStat(DRVFS_METADATA_TEST_DIR "/testdir/childfile2", 0, 0, S_IFREG | 0600, 0));

    LxtCheckErrnoZeroSuccess(mkdir(DRVFS_METADATA_TEST_DIR "/testdir/childdir2", 0700));

    LxtCheckResult(DrvFsCheckStat(DRVFS_METADATA_TEST_DIR "/testdir/childdir2", 0, 0, S_IFDIR | 0700, 0));

    //
    // Test adding metadata to an item that doesn't have it already.
    //
    // N.B. The Windows portion of the test will attempt to read this metadata
    //      using NtQueryEaFile.
    //

    LxtCheckErrnoZeroSuccess(chmod(DRVFS_METADATA_TEST_DIR, 0775));
    LxtCheckResult(DrvFsCheckStat(DRVFS_METADATA_TEST_DIR, 0, 0, S_IFDIR | 0775, 0));

    LxtCheckErrnoZeroSuccess(chown(DRVFS_METADATA_TEST_DIR, 0x11223344, 0x55667788));

    LxtCheckResult(DrvFsCheckStat(DRVFS_METADATA_TEST_DIR, 0x11223344, 0x55667788, S_IFDIR | 0775, 0));

ErrorExit:
    if (Fd >= 0)
    {
        close(Fd);
    }

    unlink(DRVFS_METADATA_TEST_DIR "/testdir/childfile");
    unlink(DRVFS_METADATA_TEST_DIR "/testdir/childfile2");
    rmdir(DRVFS_METADATA_TEST_DIR "/testdir/childdir");
    rmdir(DRVFS_METADATA_TEST_DIR "/testdir/childdir2");
    unlink(DRVFS_METADATA_TEST_DIR "/testnodereg");
    unlink(DRVFS_METADATA_TEST_DIR "/testnodefifo");
    unlink(DRVFS_METADATA_TEST_DIR "/testnodesock");
    unlink(DRVFS_METADATA_TEST_DIR "/testnodechr");
    unlink(DRVFS_METADATA_TEST_DIR "/testnodeblk");
    unlink(DRVFS_METADATA_TEST_DIR "/testlink");
    rmdir(DRVFS_METADATA_TEST_DIR "/testdir");
    unlink(DRVFS_METADATA_TEST_DIR "/testfile");
    return Result;
}

int DrvFsTestReFsWslPath(PLXT_ARGS Args)

/*++

Description:

    This routine tests using the wslpath utility against the FAT mount point.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int Result;

    //
    // Previous tests may have messed up the cwd such that getcwd fails, which
    // wslpath won't like.
    //

    LxtCheckErrnoZeroSuccess(chdir("/"));

    //
    // The ReFS volume in Windows is mounted on an NTFS directory mount, which
    // makes it an interesting case to test with wslpath.
    //

    LxtCheckResult(LxtCheckWslPathTranslation(DRVFS_REFS_DRIVE, DRVFS_PREFIX, true));
    LxtCheckResult(LxtCheckWslPathTranslation(DRVFS_PREFIX, "C:\\" DRVFS_REFS_MOUNT_POINT, false));

ErrorExit:
    return Result;
}

int DrvFsTestRename(PLXT_ARGS Args)

/*++

Description:

    This routine tests some basic rename scenarios on drvfs.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    pid_t ChildPid;
    int Fd;
    int Result;
    struct stat Stat1;
    struct stat Stat2;

    LxtCheckErrnoZeroSuccess(mkdir(DRVFS_RENAME_PREFIX, 0777));

    //
    // Create two files, and rename one over the other.
    //

    LxtCheckErrno(Fd = creat(DRVFS_RENAME_PREFIX "/a", 0777));
    LxtCheckErrnoZeroSuccess(fstat(Fd, &Stat1));
    LxtCheckClose(Fd);
    LxtCheckErrno(Fd = creat(DRVFS_RENAME_PREFIX "/b", 0777));
    LxtCheckErrnoZeroSuccess(fstat(Fd, &Stat2));
    LxtCheckClose(Fd);
    LxtCheckNotEqual(Stat1.st_ino, Stat2.st_ino, "%llu");
    LxtCheckErrnoZeroSuccess(rename(DRVFS_RENAME_PREFIX "/a", DRVFS_RENAME_PREFIX "/b"));

    LxtCheckErrnoZeroSuccess(stat(DRVFS_RENAME_PREFIX "/b", &Stat2));
    LxtCheckEqual(Stat1.st_ino, Stat2.st_ino, "%llu");
    LxtCheckErrnoFailure(access(DRVFS_RENAME_PREFIX "/a", F_OK), ENOENT);
    LxtCheckErrnoZeroSuccess(unlink(DRVFS_RENAME_PREFIX "/b"));

    //
    // Create two read-only files, and rename one over the other.
    //

    //
    // Windows 10 builds are missing a fix for this test to pass.
    //

    const char* disableReadOnlyRenameTest = getenv("WSL_DISABLE_VB_UNSTABLE_TESTS");
    if (disableReadOnlyRenameTest == NULL || strcmp(disableReadOnlyRenameTest, "1") != 0)
    {
        LxtCheckErrno(Fd = open(DRVFS_RENAME_PREFIX "/a", O_CREAT | O_EXCL | O_RDONLY, 0444));

        LxtCheckErrnoZeroSuccess(fstat(Fd, &Stat1));
        LxtCheckClose(Fd);
        LxtCheckErrno(Fd = open(DRVFS_RENAME_PREFIX "/b", O_CREAT | O_EXCL | O_RDONLY, 0444));

        LxtCheckErrnoZeroSuccess(fstat(Fd, &Stat2));
        LxtCheckClose(Fd);
        LxtCheckNotEqual(Stat1.st_ino, Stat2.st_ino, "%llu");
        LxtCheckErrnoZeroSuccess(rename(DRVFS_RENAME_PREFIX "/a", DRVFS_RENAME_PREFIX "/b"));

        LxtCheckErrnoZeroSuccess(stat(DRVFS_RENAME_PREFIX "/b", &Stat2));
        LxtCheckEqual(Stat1.st_ino, Stat2.st_ino, "%llu");
        LxtCheckErrnoFailure(access(DRVFS_RENAME_PREFIX "/a", F_OK), ENOENT);
        LxtCheckErrnoZeroSuccess(unlink(DRVFS_RENAME_PREFIX "/b"));

        //
        // Create two directories and rename one over the other.
        //
        // N.B. This is tested separately because non-POSIX rename on Windows needs
        //      extra steps to supersede the directory.
        //

        LxtCheckErrnoZeroSuccess(mkdir(DRVFS_RENAME_PREFIX "/a", 0777));
        LxtCheckErrnoZeroSuccess(mkdir(DRVFS_RENAME_PREFIX "/a/foo", 0777));
        LxtCheckErrnoZeroSuccess(stat(DRVFS_RENAME_PREFIX "/a", &Stat1));
        LxtCheckErrnoZeroSuccess(mkdir(DRVFS_RENAME_PREFIX "/b", 0777));
        LxtCheckErrnoZeroSuccess(stat(DRVFS_RENAME_PREFIX "/b", &Stat2));
        LxtCheckNotEqual(Stat1.st_ino, Stat2.st_ino, "%llu");
        LxtCheckErrnoZeroSuccess(rename(DRVFS_RENAME_PREFIX "/a", DRVFS_RENAME_PREFIX "/b"));

        LxtCheckErrnoZeroSuccess(stat(DRVFS_RENAME_PREFIX "/b", &Stat2));
        LxtCheckEqual(Stat1.st_ino, Stat2.st_ino, "%llu");

        LxtCheckErrnoFailure(access(DRVFS_RENAME_PREFIX "/a", F_OK), ENOENT);
        LxtCheckErrnoZeroSuccess(access(DRVFS_RENAME_PREFIX "/b/foo", F_OK));
        LxtCheckErrnoZeroSuccess(rmdir(DRVFS_RENAME_PREFIX "/b/foo"));
        LxtCheckErrnoZeroSuccess(rmdir(DRVFS_RENAME_PREFIX "/b"));

        //
        // Rename with different case.
        //

        LxtCheckErrnoZeroSuccess(mkdir(DRVFS_RENAME_PREFIX "/a", 0777));
        LxtCheckErrnoZeroSuccess(stat(DRVFS_RENAME_PREFIX "/a", &Stat1));
        LxtCheckErrnoZeroSuccess(rename(DRVFS_RENAME_PREFIX "/a", DRVFS_RENAME_PREFIX "/A"));

        LxtCheckErrnoZeroSuccess(stat(DRVFS_RENAME_PREFIX "/A", &Stat2));
        LxtCheckEqual(Stat1.st_ino, Stat2.st_ino, "%llu");
        LxtCheckErrnoZeroSuccess(rmdir(DRVFS_RENAME_PREFIX "/A"));
    }
    else
    {
        LxtLogInfo("WSL_DISABLE_VB_UNSTABLE_TESTS set, skipping read-only rename tests");
    }

    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        LxtCheckErrnoZeroSuccess(chdir(DRVFS_RENAME_PREFIX));

        //
        // Repeat, but with relative paths.
        //

        LxtCheckErrno(Fd = creat("a", 0777));
        LxtCheckErrnoZeroSuccess(fstat(Fd, &Stat1));
        LxtCheckClose(Fd);
        LxtCheckErrno(Fd = creat("b", 0777));
        LxtCheckErrnoZeroSuccess(fstat(Fd, &Stat2));
        LxtCheckClose(Fd);
        LxtCheckNotEqual(Stat1.st_ino, Stat2.st_ino, "%llu");
        LxtCheckErrnoZeroSuccess(rename("a", "b"));
        LxtCheckErrnoZeroSuccess(stat("b", &Stat2));
        LxtCheckEqual(Stat1.st_ino, Stat2.st_ino, "%llu");
        LxtCheckErrnoFailure(access("a", F_OK), ENOENT);
        LxtCheckErrnoZeroSuccess(unlink("b"));

        //
        // Rename and delete the working directory.
        //
        // N.B. This will not work correctly on plan 9 and virtiofs.
        //

        if (g_LxtFsInfo.FsType != LxtFsTypePlan9 && g_LxtFsInfo.FsType != LxtFsTypeVirtioFs)
        {
            LxtCheckErrnoZeroSuccess(mkdir("a", 0777));
            LxtCheckErrnoZeroSuccess(chdir("a"));
            LxtCheckErrnoZeroSuccess(mkdir("b", 0777));
            LxtCheckErrnoZeroSuccess(access("b", F_OK));
            LxtCheckErrnoZeroSuccess(access(DRVFS_RENAME_PREFIX "/a/b", F_OK));
            LxtCheckErrnoZeroSuccess(rename(DRVFS_RENAME_PREFIX "/a", DRVFS_RENAME_PREFIX "/b"));

            LxtCheckErrnoZeroSuccess(access("b", F_OK));
            LxtCheckErrnoZeroSuccess(access(DRVFS_RENAME_PREFIX "/b/b", F_OK));
            LxtCheckErrnoFailure(access(DRVFS_RENAME_PREFIX "/a/b", F_OK), ENOENT);

            LxtCheckErrnoZeroSuccess(rmdir("b"));
            LxtCheckErrnoZeroSuccess(rmdir(DRVFS_RENAME_PREFIX "/b"));
            LxtCheckResult(LxtCheckLinkTarget("/proc/self/cwd", DRVFS_RENAME_PREFIX "/b (deleted)"));

            LxtCheckErrnoZeroSuccess(chdir(".."));
            LxtCheckResult(LxtCheckLinkTarget("/proc/self/cwd", DRVFS_RENAME_PREFIX));

            LxtCheckErrnoFailure(access("a", F_OK), ENOENT);
            LxtCheckErrnoFailure(access("b", F_OK), ENOENT);

            //
            // Rename an open directory.
            //
            // N.B. This will not work correctly on plan 9.
            //

            LxtCheckErrnoZeroSuccess(mkdir("a", 0777));
            LxtCheckErrnoZeroSuccess(mkdir("a/b", 0777));
            LxtCheckErrno(Fd = open("a", O_RDONLY | O_DIRECTORY));
            LxtCheckErrnoZeroSuccess(rename("a", "b"));
            LxtCheckErrnoZeroSuccess(faccessat(Fd, "b", F_OK, 0));
            LxtCheckErrnoFailure(access("a/b", F_OK), ENOENT);
            LxtCheckErrnoFailure(access(DRVFS_RENAME_PREFIX "/a/b", F_OK), ENOENT);

            LxtCheckErrnoZeroSuccess(access("b/b", F_OK));
            LxtCheckErrnoZeroSuccess(access(DRVFS_RENAME_PREFIX "/b/b", F_OK));
            LxtCheckErrnoZeroSuccess(rmdir("b/b"));
            LxtCheckErrnoZeroSuccess(rmdir("b"));
        }

        exit(0);
    }

    LxtCheckResult(LxtWaitPidPoll(ChildPid, 0));

ErrorExit:
    rmdir(DRVFS_RENAME_PREFIX "/a/foo");
    rmdir(DRVFS_RENAME_PREFIX "/a/b");
    rmdir(DRVFS_RENAME_PREFIX "/a");
    rmdir(DRVFS_RENAME_PREFIX "/A");
    unlink(DRVFS_RENAME_PREFIX "/a");
    rmdir(DRVFS_RENAME_PREFIX "/b/b");
    rmdir(DRVFS_RENAME_PREFIX "/b");
    unlink(DRVFS_RENAME_PREFIX "/b");
    rmdir(DRVFS_RENAME_PREFIX);

    return Result;
}

int DrvFsTestRenameAt(PLXT_ARGS Args)

/*++

Description:

    This routine tests the renameat system call on drivefs.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int DirFd1;
    int DirFd2;
    int Result;

    DirFd1 = -1;
    DirFd2 = -1;

    //
    // Create a directory structure to use for the test.
    //

    LxtCheckErrnoZeroSuccess(mkdir(DRVFS_RENAMEAT_TEST_DIR, 0777));
    LxtCheckErrnoZeroSuccess(mkdir((DRVFS_RENAMEAT_TEST_DIR "/a"), 0777));
    LxtCheckErrnoZeroSuccess(mkdir((DRVFS_RENAMEAT_TEST_DIR "/a/b"), 0777));
    LxtCheckErrnoZeroSuccess(mkdir((DRVFS_RENAMEAT_TEST_DIR "/a/b/c"), 0777));
    LxtCheckErrnoZeroSuccess(mkdir((DRVFS_RENAMEAT_TEST_DIR "/a/b/c/d"), 0777));
    LxtCheckErrnoZeroSuccess(mkdir((DRVFS_RENAMEAT_TEST_DIR "/a/b/c/d/e"), 0777));
    LxtCheckErrnoZeroSuccess(mkdir((DRVFS_RENAMEAT_TEST_DIR "/a/b/c/d/e/f"), 0777));

    LxtCheckErrno(DirFd1 = open(DRVFS_RENAMEAT_TEST_DIR "/a", O_DIRECTORY));
    LxtCheckErrno(DirFd2 = open(DRVFS_RENAMEAT_TEST_DIR "/a/b/c", O_DIRECTORY));

    LxtCheckErrnoZeroSuccess(chdir(DRVFS_RENAMEAT_TEST_DIR));

    LxtCheckErrno(LxtFsRenameAtCommon(DirFd1, DirFd2));

ErrorExit:
    if (DirFd1 >= 0)
    {
        LxtClose(DirFd1);
    }

    if (DirFd2 >= 0)
    {
        LxtClose(DirFd2);
    }

    rmdir(DRVFS_RENAMEAT_TEST_DIR "/a/b/c/d/e/f");
    rmdir(DRVFS_RENAMEAT_TEST_DIR "/a/b/c/d/e");
    rmdir(DRVFS_RENAMEAT_TEST_DIR "/a/b/c/d");
    rmdir(DRVFS_RENAMEAT_TEST_DIR "/a/b/c");
    rmdir(DRVFS_RENAMEAT_TEST_DIR "/a/b");
    rmdir(DRVFS_RENAMEAT_TEST_DIR "/a");
    rmdir(DRVFS_RENAMEAT_TEST_DIR);
    return Result;
}

int DrvFsTestRenameDir(PLXT_ARGS Args)

/*++

Description:

    This routine tests the rename system call for DrvFs directories.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int Result;

    LxtCheckResult(LxtFsRenameDirCommon(DRVFS_PREFIX));

ErrorExit:
    return Result;
}

int DrvFsTestReopenUnlinked(PLXT_ARGS Args)

/*++

Description:

    This routine tests whether unlinked files can still be accessed

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int Fd;
    int Fd2;
    char Path[100];
    int Result;
    struct stat Stat;

    Fd = -1;
    Fd2 = -1;

    //
    // This functionality is not supported on Plan 9.
    //

    if (g_LxtFsInfo.FsType == LxtFsTypePlan9)
    {
        LxtLogInfo("This test is not supported in VM mode.");
        Result = 0;
        goto ErrorExit;
    }

    LxtCheckErrnoZeroSuccess(mkdir(DRVFS_BASIC_PREFIX, 0777));
    LxtCheckErrno(Fd = creat(DRVFS_BASIC_PREFIX "/testfile", 0666));
    snprintf(Path, sizeof(Path), "/proc/self/fd/%d", Fd);

    //
    // Unlink the file, and try to access it through procfs.
    //

    LxtCheckErrnoZeroSuccess(unlink(DRVFS_BASIC_PREFIX "/testfile"));
    LxtCheckErrnoFailure(access(DRVFS_BASIC_PREFIX "/testfile", F_OK), ENOENT);
    LxtCheckErrnoZeroSuccess(stat(Path, &Stat));
    LxtCheckEqual(Stat.st_nlink, 0, "%lu");

    //
    // Try to reopen through procfs.
    //

    LxtCheckErrno(Fd2 = open(Path, O_RDONLY));

ErrorExit:
    if (Fd2 >= 0)
    {
        close(Fd2);
    }

    if (Fd >= 0)
    {
        close(Fd);
    }

    unlink(DRVFS_BASIC_PREFIX "/testfile");
    rmdir(DRVFS_BASIC_PREFIX);
    return Result;
}

int DrvFsTestReparse(PLXT_ARGS Args)

/*++

Description:

    This routine tests whether drvfs correctly blocks access to reparse points
    other than its own symlinks.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    bool AbsoluteLinkFound;
    bool AppExecLinkFound;
    ssize_t BytesRead;
    char Buffer[100];
    DIR* Dir;
    int DirFd;
    struct dirent* Entry;
    int Fd;
    bool FileLinkFound;
    bool JunctionFound;
    void* Mapping;
    void* PointerResult;
    bool RelativeLinkFound;
    int Result;
    struct stat Stat;
    bool V1LinkFound;

    //
    // First make sure the reparse point does not show up in the directory
    // listing.
    //

    DirFd = -1;
    Fd = -1;
    Mapping = NULL;
    LxtCheckNullErrno(Dir = opendir(DRVFS_REPARSE_PREFIX));
    errno = 0;
    AbsoluteLinkFound = false;
    RelativeLinkFound = false;
    FileLinkFound = false;
    JunctionFound = false;
    V1LinkFound = false;
    AppExecLinkFound = false;
    while ((Entry = readdir(Dir)) != NULL)
    {
        if (strcmp(Entry->d_name, "absolutelink") == 0)
        {
            AbsoluteLinkFound = true;
            LxtCheckEqual(Entry->d_type, DT_LNK, "%d");
        }

        if (strcmp(Entry->d_name, "relativelink") == 0)
        {
            RelativeLinkFound = true;
            LxtCheckEqual(Entry->d_type, DT_LNK, "%d");
        }

        if (strcmp(Entry->d_name, "filelink") == 0)
        {
            FileLinkFound = true;
            LxtCheckEqual(Entry->d_type, DT_LNK, "%d");
        }

        if (strcmp(Entry->d_name, "junction") == 0)
        {
            JunctionFound = true;
            LxtCheckEqual(Entry->d_type, DT_LNK, "%d");
        }

        if (strcmp(Entry->d_name, "v1link") == 0)
        {
            V1LinkFound = true;
            LxtCheckEqual(Entry->d_type, DT_LNK, "%d");
        }

        if (strcmp(Entry->d_name, "appexeclink") == 0)
        {
            AppExecLinkFound = true;
            LxtCheckEqual(Entry->d_type, DT_REG, "%d");
        }
    }

    if (errno != 0)
    {
        LxtLogError("readdir failed, errno %d: %s", errno, strerror(errno));
        Result = LXT_RESULT_FAILURE;
        goto ErrorExit;
    }

    LxtCheckErrnoZeroSuccess(closedir(Dir));
    Dir = NULL;
    LxtCheckTrue(AbsoluteLinkFound);
    LxtCheckTrue(RelativeLinkFound);
    LxtCheckTrue(FileLinkFound);
    LxtCheckTrue(JunctionFound);
    LxtCheckTrue(V1LinkFound);
    LxtCheckTrue(AppExecLinkFound);

    //
    // Check the absolute link can be resolved, and that the target is correct
    // and uses Linux separators.
    //

    LxtCheckResult(LxtCheckLinkTarget(DRVFS_REPARSE_PREFIX "/absolutelink", DRVFS_REPARSE_PREFIX "/test/linktarget"));

    LxtCheckErrnoZeroSuccess(lstat(DRVFS_REPARSE_PREFIX "/absolutelink", &Stat));
    LxtCheckEqual((Stat.st_mode & S_IFMT), S_IFLNK, "0%o");
    LxtCheckEqual(Stat.st_size, strlen(DRVFS_REPARSE_PREFIX "/test/linktarget"), "%ull");

    LxtCheckErrno(DirFd = open(DRVFS_REPARSE_PREFIX "/absolutelink", O_DIRECTORY | O_RDONLY));

    LxtCheckResult(LxtCheckFdPath(DirFd, DRVFS_REPARSE_PREFIX "/test/linktarget"));
    LxtCheckClose(DirFd);

    //
    // Check the relative link can be resolved, and that the target is correct
    // and uses Linux separators.
    //

    LxtCheckResult(LxtCheckLinkTarget(DRVFS_REPARSE_PREFIX "/relativelink", "test/linktarget"));

    LxtCheckErrnoZeroSuccess(lstat(DRVFS_REPARSE_PREFIX "/relativelink", &Stat));
    LxtCheckEqual((Stat.st_mode & S_IFMT), S_IFLNK, "0%o");
    LxtCheckEqual(Stat.st_size, strlen("test/linktarget"), "%ull");

    LxtCheckErrno(DirFd = open(DRVFS_REPARSE_PREFIX "/relativelink", O_DIRECTORY | O_RDONLY));

    LxtCheckResult(LxtCheckFdPath(DirFd, DRVFS_REPARSE_PREFIX "/test/linktarget"));
    LxtCheckClose(DirFd);

    //
    // Check the relative link to a file can be resolved.
    //

    LxtCheckResult(LxtCheckLinkTarget(DRVFS_REPARSE_PREFIX "/filelink", "test/filetarget"));

    LxtCheckErrnoZeroSuccess(lstat(DRVFS_REPARSE_PREFIX "/filelink", &Stat));
    LxtCheckEqual((Stat.st_mode & S_IFMT), S_IFLNK, "0%o");
    LxtCheckEqual(Stat.st_size, strlen("test/filetarget"), "%ull");

    //
    // Check the junction can be resolved, and that the target is correct
    // and uses Linux separators.
    //

    LxtCheckResult(LxtCheckLinkTarget(DRVFS_REPARSE_PREFIX "/junction", DRVFS_REPARSE_PREFIX "/test/linktarget"));

    LxtCheckErrno(DirFd = open(DRVFS_REPARSE_PREFIX "/junction", O_DIRECTORY | O_RDONLY));

    LxtCheckResult(LxtCheckFdPath(DirFd, DRVFS_REPARSE_PREFIX "/test/linktarget"));
    LxtCheckClose(DirFd);

    //
    // Check the target of the V1 link can be resolved, and that its reported
    // size is correct.
    //

    LxtCheckResult(LxtCheckLinkTarget(DRVFS_REPARSE_PREFIX "/v1link", "/v1/symlink/target"));

    LxtCheckErrnoZeroSuccess(lstat(DRVFS_REPARSE_PREFIX "/v1link", &Stat));
    LxtCheckEqual((Stat.st_mode & S_IFMT), S_IFLNK, "0%o");
    LxtCheckEqual(Stat.st_size, strlen("/v1/symlink/target"), "%ull");

    //
    // Check that the back-compat symlink in the drive root can be resolved.
    //
    // N.B. This file explicitly denies FILE_READ_DATA permissions.
    //

    LxtCheckResult(LxtCheckLinkTarget(DRVFS_PREFIX "/Documents and Settings", "/mnt/c/Users"));

    LxtCheckErrnoZeroSuccess(lstat(DRVFS_PREFIX "/Documents and Settings", &Stat));

    LxtCheckEqual((Stat.st_mode & S_IFMT), S_IFLNK, "0%o");
    LxtCheckEqual(Stat.st_size, strlen("/mnt/c/Users"), "%ull");

    //
    // Ensure rename works with an NT link in the path.
    //

    LxtCheckErrno(Fd = creat(DRVFS_REPARSE_PREFIX "/renametest", 0666));
    LxtCheckClose(Fd);
    LxtCheckErrnoZeroSuccess(rename(DRVFS_REPARSE_PREFIX "/renametest", DRVFS_REPARSE_PREFIX "/absolutelink/renametest"));

    LxtCheckErrnoZeroSuccess(rename(DRVFS_REPARSE_PREFIX "/absolutelink/renametest", DRVFS_REPARSE_PREFIX "/relativelink/renametest"));

    //
    // Ensure unlink works with an NT link in the path.
    //

    LxtCheckErrnoZeroSuccess(unlink(DRVFS_REPARSE_PREFIX "/relativelink/renametest"));

    LxtCheckErrnoFailure(access(DRVFS_REPARSE_PREFIX "/relativelink/renametest", F_OK), ENOENT);

    LxtCheckErrno(Fd = creat(DRVFS_REPARSE_PREFIX "/absolutelink/renametest", 0666));

    LxtCheckClose(Fd);
    LxtCheckErrnoZeroSuccess(unlink(DRVFS_REPARSE_PREFIX "/absolutelink/renametest"));

    LxtCheckErrnoFailure(access(DRVFS_REPARSE_PREFIX "/absolutelink/renametest", F_OK), ENOENT);

    //
    // Although Windows uses a directory for the first link and a file for the
    // second, from WSL it should be possible to delete either using unlink,
    // while rmdir should not work.
    //

    LxtCheckErrnoFailure(rmdir(DRVFS_REPARSE_PREFIX "/relativelink"), ENOTDIR);
    LxtCheckErrnoZeroSuccess(unlink(DRVFS_REPARSE_PREFIX "/relativelink"));
    LxtCheckErrnoFailure(faccessat(AT_FDCWD, DRVFS_REPARSE_PREFIX "/relativelink", F_OK, AT_SYMLINK_NOFOLLOW), ENOENT);

    LxtCheckErrnoFailure(rmdir(DRVFS_REPARSE_PREFIX "/filelink"), ENOTDIR);
    LxtCheckErrnoZeroSuccess(unlink(DRVFS_REPARSE_PREFIX "/filelink"));
    LxtCheckErrnoFailure(faccessat(AT_FDCWD, DRVFS_REPARSE_PREFIX "/filelink", F_OK, AT_SYMLINK_NOFOLLOW), ENOENT);

    //
    // App execution aliases are treated as regular files but have generated
    // contents with a fake PE header for interop purposes.
    //

    LxtCheckErrnoZeroSuccess(stat(DRVFS_REPARSE_PREFIX "/appexeclink", &Stat));
    LxtCheckEqual(Stat.st_mode & S_IFMT, S_IFREG, "0%o");
    LxtCheckEqual(Stat.st_size, 2, "%llu");
    LxtCheckErrno(Fd = open(DRVFS_REPARSE_PREFIX "/appexeclink", O_RDONLY));
    LxtCheckErrno(BytesRead = read(Fd, Buffer, sizeof(Buffer)));
    LxtCheckEqual(BytesRead, 2, "%ld");
    LxtCheckMemoryEqual(Buffer, "MZ", 2);

    //
    // Check using mapping as well as this is a different code path in WSL1 and
    // is what execve uses.
    //

    LxtCheckNullErrno(Mapping = mmap(NULL, 2, PROT_READ, MAP_SHARED, Fd, 0));
    LxtCheckMemoryEqual(Mapping, "MZ", 2);

ErrorExit:
    if (Mapping != NULL)
    {
        munmap(Mapping, 2);
    }

    if (Dir != NULL)
    {
        closedir(Dir);
    }

    if (DirFd >= 0)
    {
        close(DirFd);
    }

    if (Fd >= 0)
    {
        close(Fd);
    }

    unlink(DRVFS_REPARSE_PREFIX "/renametest");
    unlink(DRVFS_REPARSE_PREFIX "/absolutelink/renametest");
    unlink(DRVFS_REPARSE_PREFIX "/relativelink/renametest");
    return Result;
}

int DrvFsTestSeek(PLXT_ARGS Args)

/*++

Description:

    This routine tests seeking in drvfs files.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int Fd;
    int Result;
    const char TestData[] = "abcdefghijklmnopqrstuvwxyz0123456789";

    Fd = -1;

    //
    // Create a test file.
    //

    LxtCheckErrnoZeroSuccess(mkdir(DRVFS_BASIC_PREFIX, 0777));
    LxtCheckErrno(Fd = open(DRVFS_BASIC_PREFIX "/testfile", (O_RDWR | O_CREAT), 0666));

    LxtCheckErrno(write(Fd, TestData, sizeof(TestData)));

    //
    // SEEK_SET.
    //

    LxtCheckResult(DrvFsTestSeekHelper(Fd, 0, SEEK_SET, 0, TestData, sizeof(TestData)));

    LxtCheckResult(DrvFsTestSeekHelper(Fd, 100, SEEK_SET, 100, TestData, sizeof(TestData)));

    LxtCheckResult(DrvFsTestSeekHelper(Fd, 10, SEEK_SET, 10, TestData, sizeof(TestData)));

    LxtCheckErrnoFailure(lseek(Fd, -100, SEEK_SET), EINVAL);

    //
    // SEEK_CUR.
    //
    // N.B. Start offset is 15 because of the read above.
    //

    LxtCheckResult(DrvFsTestSeekHelper(Fd, 0, SEEK_CUR, 15, TestData, sizeof(TestData)));

    LxtCheckResult(DrvFsTestSeekHelper(Fd, 5, SEEK_CUR, 25, TestData, sizeof(TestData)));

    LxtCheckResult(DrvFsTestSeekHelper(Fd, -10, SEEK_CUR, 20, TestData, sizeof(TestData)));

    LxtCheckResult(DrvFsTestSeekHelper(Fd, 100, SEEK_CUR, 125, TestData, sizeof(TestData)));

    LxtCheckErrnoFailure(lseek(Fd, -200, SEEK_SET), EINVAL);

    //
    // SEEK_END.
    //

    LxtCheckResult(DrvFsTestSeekHelper(Fd, -10, SEEK_END, sizeof(TestData) - 10, TestData, sizeof(TestData)));

    LxtCheckResult(DrvFsTestSeekHelper(Fd, 10, SEEK_END, sizeof(TestData) + 10, TestData, sizeof(TestData)));

    LxtCheckResult(DrvFsTestSeekHelper(Fd, -sizeof(TestData), SEEK_END, 0, TestData, sizeof(TestData)));

    LxtCheckErrnoFailure(lseek(Fd, -100, SEEK_END), EINVAL);

ErrorExit:
    if (Fd >= 0)
    {
        close(Fd);
    }

    unlink(DRVFS_BASIC_PREFIX "/testfile");
    rmdir(DRVFS_BASIC_PREFIX);
    return Result;
}

int DrvFsTestSeekHelper(int Fd, off_t Offset, int Whence, off_t ExpectedOffset, const char* TestData, int TestDataSize)

/*++

Description:

    This routine is a helper for the seek test.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    off_t ActualOffset;
    char Buffer[5];
    ssize_t BytesRead;
    int Result;

    LxtCheckErrno(ActualOffset = lseek(Fd, Offset, Whence));
    LxtCheckEqual(ActualOffset, ExpectedOffset, "%ld");
    LxtCheckErrno(BytesRead = read(Fd, Buffer, sizeof(Buffer)));
    if (ExpectedOffset >= TestDataSize)
    {
        LxtCheckEqual(BytesRead, 0, "%ld");
    }
    else
    {
        LxtCheckEqual(BytesRead, sizeof(Buffer), "%ld");
        LxtCheckMemoryEqual(Buffer, &TestData[ExpectedOffset], sizeof(Buffer));
    }

ErrorExit:
    return Result;
}

int DrvFsTestDirSeek(PLXT_ARGS Args)

/*++

Description:

    This routine tests seeking in drvfs directory.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int Result;
    LxtCheckResult(LxtFsDirSeekCommon(DRVFS_GETDENTS_PREFIX));

ErrorExit:
    return Result;
}

int DrvFsTestSetup(PLXT_ARGS Args, int TestMode)

/*++

Description:

    This routine performs setup for the drvfs tests.

Arguments:

    Args - Supplies the command line arguments.

    TestMode - Supplies the test mode.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    char Buffer[100];
    char CombinedOptions[100];
    LXT_FS_INFO FsInfo;
    char FsOptions[100];
    char Options[100];
    int ParentId;
    int Result;

    //
    // Don't perform setup if help was requested.
    //

    if (Args->HelpRequested != false)
    {
        Result = 0;
        goto ErrorExit;
    }

    LxtCheckResult(LxtFsGetFsInfo(DRVFS_PREFIX, &FsInfo));

    //
    // For the FAT test, check the FAT volume mount point can be accessed but
    // not resolved (because the FAT volume is not mounted in WSL yet).
    //
    // N.B. This doesn't work on Plan 9 because it doesn't support junction
    //      symlinks.
    //

    if (TestMode == DRVFS_FAT_TEST_MODE)
    {
        LxtCheckErrnoZeroSuccess(faccessat(AT_FDCWD, DRVFS_PREFIX "/" DRVFS_FAT_MOUNT_POINT, F_OK, AT_SYMLINK_NOFOLLOW));

        if (FsInfo.FsType != LxtFsTypePlan9)
        {
            LxtCheckErrnoFailure(readlink(DRVFS_PREFIX "/" DRVFS_FAT_MOUNT_POINT, Buffer, sizeof(Buffer)), EIO);
        }
    }

    //
    // Unmount drvfs first so a new instance will be created, which allows the
    // fallback mode to be set.
    //
    // N.B. Make sure the cwd is not inside drvfs.
    //

    LxtCheckErrnoZeroSuccess(chdir("/"));
    LxtCheckErrnoZeroSuccess(umount(DRVFS_PREFIX));
    ParentId = MountGetMountId(DRVFS_PREFIX);

    //
    // For the FAT tests, fallback options aren't used (FAT doesn't support
    // NtQueryInformationByName or FILE_STAT_INFORMATION anyway).
    //

    switch (TestMode)
    {
    case DRVFS_FAT_TEST_MODE:
        LxtCheckResult(LxtFsMountDrvFs(DRVFS_FAT_DRIVE, DRVFS_PREFIX, "noatime,case=off"));

        LxtCheckResult(LxtFsCheckDrvFsMount(DRVFS_FAT_DRIVE, DRVFS_PREFIX, "case=off", ParentId, "/"));

        Result = LXT_RESULT_SUCCESS;
        break;

    case DRVFS_SMB_TEST_MODE:
        LxtCheckResult(LxtFsMountDrvFs(DRVFS_UNC_PATH, DRVFS_PREFIX, "noatime,case=off"));

        LxtCheckResult(LxtFsCheckDrvFsMount(DRVFS_UNC_PATH, DRVFS_PREFIX, "case=off", ParentId, "/"));

        Result = LXT_RESULT_SUCCESS;
        break;

    case DRVFS_METADATA_TEST_MODE:
        LxtCheckResult(LxtFsMountDrvFs(DRVFS_DRIVE, DRVFS_PREFIX, "noatime,metadata,case=dir"));

        LxtCheckResult(LxtFsCheckDrvFsMount(DRVFS_DRIVE, DRVFS_PREFIX, "metadata,case=dir", ParentId, "/"));

        Result = LXT_RESULT_SUCCESS;
        break;

    case DRVFS_REFS_TEST_MODE:
        LxtCheckResult(LxtFsMountDrvFs(DRVFS_REFS_DRIVE, DRVFS_PREFIX, "noatime,case=dir"));

        LxtCheckResult(LxtFsCheckDrvFsMount(DRVFS_REFS_DRIVE, DRVFS_PREFIX, "case=dir", ParentId, "/"));

        Result = LXT_RESULT_SUCCESS;
        break;

    default:

        //
        // Plan 9 and virtiofs don't support fallback modes, so just remount with default options in that case.
        //

        if (FsInfo.FsType == LxtFsTypePlan9 || FsInfo.FsType == LxtFsTypeVirtioFs)
        {
            LxtCheckResult(LxtFsMountDrvFs(DRVFS_DRIVE, DRVFS_PREFIX, "noatime,case=dir"));

            LxtCheckResult(LxtFsCheckDrvFsMount(DRVFS_DRIVE, DRVFS_PREFIX, "case=dir", ParentId, "/"));
        }
        else
        {

            //
            // Remount with the desired fallback mode.
            //

            snprintf(Options, sizeof(Options), "case=dir,fallback=%d", TestMode);
            LxtCheckErrnoZeroSuccess(mount(DRVFS_DRIVE, DRVFS_PREFIX, DRVFS_FS_TYPE, DRVFS_MOUNT_OPTIONS, Options));

            //
            // Check if drvfs actually used the requested fallback mode. This guards
            // against a preexisting instance (e.g. in another mount namespace)
            // preventing the options from changing, or file system limitations causing
            // drvfs to use a different fallback mode than requested.
            //

            snprintf(FsOptions, sizeof(FsOptions), "rw,%s", Options);
            snprintf(CombinedOptions, sizeof(CombinedOptions), "rw,noatime,%s", Options);

            LxtCheckResult(MountCheckIsMount(DRVFS_PREFIX, ParentId, DRVFS_DRIVE, DRVFS_FS_TYPE, "/", "rw,noatime", FsOptions, CombinedOptions, 0));
        }

        break;
    }

    LxtCheckResult(LxtFsGetFsInfo(DRVFS_PREFIX, &g_LxtFsInfo));

ErrorExit:
    return Result;
}

int DrvFsTestSmbUtimensat(PLXT_ARGS Args)

/*++

Description:

    This routine tests the utimensat system call on drvfs for SMB shares.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    return DrvFsTestUtimensatCommon(FS_UTIME_NO_SYMLINKS);
}

int DrvFsTestSmbUnsupported(PLXT_ARGS Args)

/*++

Description:

    This routine tests unsupported functionality on SMB.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    return DrvFsTestUnsupportedCommon(DRVFS_SMB_TEST_MODE);
}

int DrvFsTestSmbWslPath(PLXT_ARGS Args)

/*++

Description:

    This routine tests using the wslpath utility against the FAT mount point.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int Result;

    //
    // Previous tests may have messed up the cwd such that getcwd fails, which
    // wslpath won't like.
    //

    LxtCheckErrnoZeroSuccess(chdir("/"));

    //
    // Make sure translating UNC paths works.
    //

    LxtCheckResult(LxtCheckWslPathTranslation(DRVFS_UNC_PATH, DRVFS_PREFIX, true));
    LxtCheckResult(LxtCheckWslPathTranslation(DRVFS_PREFIX, "\\\\localhost\\C$", false));

ErrorExit:
    return Result;
}

int DrvFsTestSymlink(PLXT_ARGS Args)

/*++

Description:

    This routine tests symlink creation.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int Fd;
    int Result;

    Fd = 0;
    LxtCheckErrnoZeroSuccess(mkdir(DRVFS_SYMLINK_TEST_DIR, 0777));

    //
    // Create a dir and a file to serve as targets.
    //

    LxtCheckErrnoZeroSuccess(mkdir(DRVFS_SYMLINK_TEST_DIR "/dir", 0777));
    LxtCheckErrno(Fd = creat(DRVFS_SYMLINK_TEST_DIR "/file.txt", 0666));
    LxtCheckClose(Fd);

    //
    // Test the scenarios that should create NT symlinks.
    //
    // Relative link to file.
    //

    LxtCheckResult(DrvFsTestSymlinkHelper("file.txt", DRVFS_SYMLINK_TEST_DIR "/ntlink1"));

    //
    // Relative link to dir.
    //

    LxtCheckResult(DrvFsTestSymlinkHelper("dir", DRVFS_SYMLINK_TEST_DIR "/ntlink2"));

    //
    // Relative links with .. components.
    //

    LxtCheckResult(DrvFsTestSymlinkHelper("..", DRVFS_SYMLINK_TEST_DIR "/ntlink3"));

    LxtCheckResult(DrvFsTestSymlinkHelper("../symlink/file.txt", DRVFS_SYMLINK_TEST_DIR "/ntlink4"));

    LxtCheckResult(DrvFsTestSymlinkHelper("dir/../file.txt", DRVFS_SYMLINK_TEST_DIR "/ntlink5"));

    //
    // Relative link to another NT link (file and dir).
    //

    LxtCheckResult(DrvFsTestSymlinkHelper("ntlink1", DRVFS_SYMLINK_TEST_DIR "/ntlink6"));

    LxtCheckResult(DrvFsTestSymlinkHelper("ntlink2", DRVFS_SYMLINK_TEST_DIR "/ntlink7"));

    //
    // Relative link to a file whose name contains escaped characters.
    //

    LxtCheckErrno(Fd = creat(DRVFS_SYMLINK_TEST_DIR "/foo:bar", 0666));
    LxtCheckClose(Fd);

    LxtCheckResult(DrvFsTestSymlinkHelper("foo:bar", DRVFS_SYMLINK_TEST_DIR "/ntlink8"));

    //
    // Test the scenarios that should create LX symlinks.
    //
    // Absolute link.
    //

    LxtCheckResult(DrvFsTestSymlinkHelper(DRVFS_SYMLINK_TEST_DIR "/file.txt", DRVFS_SYMLINK_TEST_DIR "/lxlink1"));

    //
    // Relative link crossing mount with ..
    //

    LxtCheckResult(DrvFsTestSymlinkHelper("../..", DRVFS_SYMLINK_TEST_DIR "/lxlink2"));

    //
    // Relative link traversing an NT link.
    //

    LxtCheckResult(DrvFsTestSymlinkHelper("ntlink2/../file.txt", DRVFS_SYMLINK_TEST_DIR "/lxlink3"));

    //
    // Relative link to another LX link.
    //

    LxtCheckResult(DrvFsTestSymlinkHelper("lxlink1", DRVFS_SYMLINK_TEST_DIR "/lxlink4"));

    //
    // Target doesn't exist.
    //

    LxtCheckResult(DrvFsTestSymlinkHelper("foo", DRVFS_SYMLINK_TEST_DIR "/lxlink5"));

    //
    // Symlink points to itself.
    //
    // N.B. The main purpose of this check is to make sure this doesn't
    //      deadlock.
    //

    LxtCheckResult(DrvFsTestSymlinkHelper("lxlink6", DRVFS_SYMLINK_TEST_DIR "/lxlink6"));

    //
    // Trying to create a symlink to itself if the target file exists should
    // return EEXIST and not deadlock.
    //

    LxtCheckErrno(Fd = creat(DRVFS_SYMLINK_TEST_DIR "/link_exist", 0777));
    LxtCheckClose(Fd);
    LxtCheckErrnoFailure(symlink("link_exist", DRVFS_SYMLINK_TEST_DIR "/link_exist"), EEXIST);

    //
    // Relative link crossing mount on subdir.
    //

    LxtCheckErrnoZeroSuccess(mount("mytmp", DRVFS_SYMLINK_TEST_DIR "/dir", "tmpfs", 0, NULL));

    LxtCheckResult(DrvFsTestSymlinkHelper("dir/../file.txt", DRVFS_SYMLINK_TEST_DIR "/lxlink7"));

ErrorExit:
    if (Fd >= 0)
    {
        close(Fd);
    }

    unlink(DRVFS_SYMLINK_TEST_DIR "/link_exist");
    umount(DRVFS_SYMLINK_TEST_DIR "/dir");
    return Result;
}

int DrvFsTestSymlinkHelper(char* Target, char* Path)

/*++

Description:

    This routine tests if a symlink can be successfully created and its path
    is returned correctly.

Arguments:

    Path - Supplies the path of the link to create.

    Target - Supplies the link target.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int Result;
    struct stat Stat;

    LxtCheckErrnoZeroSuccess(symlink(Target, Path));
    LxtCheckErrnoZeroSuccess(lstat(Path, &Stat));
    LxtCheckEqual(Stat.st_mode, S_IFLNK | 0777, "0%o");
    LxtCheckResult(LxtCheckLinkTarget(Path, Target));
    LxtCheckEqual(Stat.st_size, strlen(Target), "%ull");

ErrorExit:
    return Result;
}

int DrvFsTestUnsupportedCommon(int TestMode)

/*++

Description:

    This routine tests unsupported functionality on FAT and SMB.

Arguments:

    TestMode - Supplies the test mode.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    char Buffer[1024];
    int Fd;
    int Result;

    LxtCheckErrnoZeroSuccess(mkdir(DRVFS_UNSUPPORTED_TEST_DIR, 0777));
    LxtCheckErrno(Fd = creat(DRVFS_UNSUPPORTED_TEST_DIR "/target", 0666));
    LxtCheckClose(Fd);
    LxtCheckErrnoFailure(symlink(DRVFS_UNSUPPORTED_TEST_DIR "/target", DRVFS_UNSUPPORTED_TEST_DIR "/foo"), EPERM);

    LxtCheckErrnoFailure(mknod(DRVFS_UNSUPPORTED_TEST_DIR "/foo", S_IFIFO | 0666, 0), EPERM);

    if (TestMode == DRVFS_FAT_TEST_MODE)
    {
        LxtCheckErrnoFailure(link(DRVFS_UNSUPPORTED_TEST_DIR "/target", DRVFS_UNSUPPORTED_TEST_DIR "/foo"), EPERM);

        LxtCheckErrnoFailure(LxtGetxattr(DRVFS_UNSUPPORTED_TEST_DIR "/target", "user.test", Buffer, sizeof(Buffer)), ENOTSUP);

        LxtCheckErrnoFailure(LxtSetxattr(DRVFS_UNSUPPORTED_TEST_DIR "/target", "user.test", Buffer, sizeof(Buffer), 0), ENOTSUP);

        LxtCheckErrnoFailure(LxtListxattr(DRVFS_UNSUPPORTED_TEST_DIR "/target", Buffer, sizeof(Buffer)), ENOTSUP);
    }

ErrorExit:
    unlink(DRVFS_UNSUPPORTED_TEST_DIR "/target");
    rmdir(DRVFS_UNSUPPORTED_TEST_DIR);
    return Result;
}

int DrvFsTestUtimensat(PLXT_ARGS Args)

/*++

Description:

    This routine tests the utimensat system call on drvfs.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    return DrvFsTestUtimensatCommon(0);
}

int DrvFsTestUtimensatCommon(int Flags)

/*++

Description:

    This routine tests the utimensat system call on drvfs.

Arguments:

    Flags - Supplies the flags.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int Result;

    Flags |= FS_UTIME_NT_PRECISION;
    LxtCheckResult(LxtFsUtimeCreateTestFiles(DRVFS_UTIME_TEST_DIR, Flags));
    LxtCheckResult(LxtFsUtimeBasicCommon(DRVFS_UTIME_TEST_DIR, Flags));

ErrorExit:
    LxtFsUtimeCleanupTestFiles(DRVFS_UTIME_TEST_DIR);
    return Result;
}

int DrvFsTestWritev(PLXT_ARGS Args)

/*++
--*/

{

    int Result;

    LxtCheckErrnoZeroSuccess(mkdir(DRVFS_WRITEV_TEST_DIR, 0777));
    LxtCheckResult(LxtFsWritevCommon(DRVFS_WRITEV_TEST_DIR "/fs_writev_test.bin"));

ErrorExit:
    unlink(DRVFS_WRITEV_TEST_DIR "/fs_writev_test.bin");
    rmdir(DRVFS_WRITEV_TEST_DIR);
    return Result;
}
