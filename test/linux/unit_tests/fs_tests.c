/*
 * Copyright (c) Microsoft Corporation.
 * Licensed under the MIT License.
 */

/*++

Module:

    fs_tests.c

Abstract:

    Unit tests for filesystem operations including statfs, fstatat,
    renameat2, and linkat syscalls with various edge cases and error
    handling scenarios.

--*/

#include "lxtcommon.h"
#include "unittests.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/fs.h>

#define LXT_NAME "FsTests"

#define FS_TEST_DIR "/data/test/fs_tests"
#define FS_TEST_FILE_A "test_file_a"
#define FS_TEST_FILE_B "test_file_b"
#define FS_TEST_SUBDIR "test_subdir"
#define FS_TEST_LINK "test_link"
#define FS_TEST_SYMLINK "test_symlink"
#define FS_TEST_CONTENT "Hello, filesystem tests!\n"
#define FS_TEST_CONTENT_B "Second file content.\n"

/*
 * Forward declarations of test variations.
 */

int FsTestsStatfsVariation(PLXT_ARGS Args);
int FsTestsFstatatVariation(PLXT_ARGS Args);
int FsTestsRenameat2Variation(PLXT_ARGS Args);
int FsTestsLinkatVariation(PLXT_ARGS Args);
int FsTestsErrorHandlingVariation(PLXT_ARGS Args);
int FsTestsStatfsEdgeCasesVariation(PLXT_ARGS Args);
int FsTestsFstatatFlagsVariation(PLXT_ARGS Args);
int FsTestsRenameat2CrossDirVariation(PLXT_ARGS Args);

static const LXT_VARIATION g_LxtVariations[] = {
    {"Statfs basic", FsTestsStatfsVariation},
    {"Fstatat basic", FsTestsFstatatVariation},
    {"Renameat2 basic", FsTestsRenameat2Variation},
    {"Linkat basic", FsTestsLinkatVariation},
    {"Error handling", FsTestsErrorHandlingVariation},
    {"Statfs edge cases", FsTestsStatfsEdgeCasesVariation},
    {"Fstatat flags", FsTestsFstatatFlagsVariation},
    {"Renameat2 cross-dir", FsTestsRenameat2CrossDirVariation}
};

/*
 * Helper: Create a test file with specified content.
 */

static int
FsTestsCreateFile(
    int DirFd,
    const char *Name,
    const char *Content
    )
{
    int Fd;
    int Result;
    ssize_t Written;

    Fd = -1;
    LxtCheckErrno(Fd = openat(DirFd, Name, O_CREAT | O_WRONLY | O_TRUNC, 0644));

    if (Content != NULL) {
        Written = write(Fd, Content, strlen(Content));
        if (Written < 0) {
            Result = LXT_RESULT_FAILURE;
            LxtLogError("write to %s failed", Name);
            goto ErrorExit;
        }
    }

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    if (Fd >= 0) {
        close(Fd);
    }

    return Result;
}

/*
 * Helper: Set up the test directory structure.
 */

static int
FsTestsSetup(void)
{
    int Result;

    /* Create test directory hierarchy. */
    (void)mkdir(FS_TEST_DIR, 0755);
    (void)mkdir(FS_TEST_DIR "/" FS_TEST_SUBDIR, 0755);

    Result = LXT_RESULT_SUCCESS;
    return Result;
}

/*
 * Helper: Clean up test artifacts.
 */

static void
FsTestsCleanup(void)
{
    (void)unlink(FS_TEST_DIR "/" FS_TEST_FILE_A);
    (void)unlink(FS_TEST_DIR "/" FS_TEST_FILE_B);
    (void)unlink(FS_TEST_DIR "/" FS_TEST_LINK);
    (void)unlink(FS_TEST_DIR "/" FS_TEST_SYMLINK);
    (void)unlink(FS_TEST_DIR "/" FS_TEST_SUBDIR "/" FS_TEST_FILE_A);
    (void)rmdir(FS_TEST_DIR "/" FS_TEST_SUBDIR);
    (void)rmdir(FS_TEST_DIR);
}

/*
 * Entry point for the filesystem tests module.
 */

int
FsTestsTestEntry(
    int Argc,
    char *Argv[]
    )
{
    LXT_ARGS Args;
    int Result;

    LxtCheckResult(LxtInitialize(Argc, Argv, &Args, LXT_NAME));
    LxtCheckResult(FsTestsSetup());
    LxtCheckResult(LxtRunVariations(&Args, g_LxtVariations, LXT_COUNT_OF(g_LxtVariations)));

ErrorExit:
    FsTestsCleanup();
    LxtUninitialize();
    return !LXT_SUCCESS(Result);
}

/*
 * Variation: Test statfs syscall on the test directory.
 */

int
FsTestsStatfsVariation(
    PLXT_ARGS Args
    )
{
    int Result;
    struct statvfs StatBuf;

    /* statfs on the test directory should succeed. */
    LxtCheckErrnoZeroSuccess(statvfs(FS_TEST_DIR, &StatBuf));

    /* Block size must be positive. */
    LxtCheckTrue(StatBuf.f_bsize > 0);

    /* Total blocks should be non-zero for a real filesystem. */
    LxtCheckTrue(StatBuf.f_blocks > 0);

    /* Free blocks should not exceed total blocks. */
    LxtCheckTrue(StatBuf.f_bfree <= StatBuf.f_blocks);

    /* Available blocks should not exceed free blocks. */
    LxtCheckTrue(StatBuf.f_bavail <= StatBuf.f_bfree);

    /* Fragment size should be positive. */
    LxtCheckTrue(StatBuf.f_frsize > 0);

    /* statfs on root should also succeed. */
    LxtCheckErrnoZeroSuccess(statvfs("/", &StatBuf));
    LxtCheckTrue(StatBuf.f_bsize > 0);

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    return Result;
}

/*
 * Variation: Test fstatat syscall with regular files and directories.
 */

int
FsTestsFstatatVariation(
    PLXT_ARGS Args
    )
{
    int DirFd;
    int Result;
    struct stat StatBuf;

    DirFd = -1;

    /* Create a test file. */
    LxtCheckErrno(DirFd = open(FS_TEST_DIR, O_RDONLY | O_DIRECTORY));
    LxtCheckResult(FsTestsCreateFile(DirFd, FS_TEST_FILE_A, FS_TEST_CONTENT));

    /* fstatat on the file should succeed. */
    LxtCheckErrnoZeroSuccess(fstatat(DirFd, FS_TEST_FILE_A, &StatBuf, 0));

    /* Verify it's a regular file. */
    LxtCheckTrue(S_ISREG(StatBuf.st_mode));

    /* Size should match content length. */
    LxtCheckEqual(StatBuf.st_size, (off_t)strlen(FS_TEST_CONTENT), "%ld");

    /* Link count for a regular file should be at least 1. */
    LxtCheckTrue(StatBuf.st_nlink >= 1);

    /* fstatat on the directory itself with AT_EMPTY_PATH. */
    LxtCheckErrnoZeroSuccess(fstatat(DirFd, "", &StatBuf, AT_EMPTY_PATH));
    LxtCheckTrue(S_ISDIR(StatBuf.st_mode));

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    if (DirFd >= 0) {
        close(DirFd);
    }

    (void)unlink(FS_TEST_DIR "/" FS_TEST_FILE_A);
    return Result;
}

/*
 * Variation: Test renameat2 syscall with RENAME_NOREPLACE.
 */

int
FsTestsRenameat2Variation(
    PLXT_ARGS Args
    )
{
    int DirFd;
    int Result;
    struct stat StatBuf;

    DirFd = -1;

    LxtCheckErrno(DirFd = open(FS_TEST_DIR, O_RDONLY | O_DIRECTORY));

    /* Create two test files. */
    LxtCheckResult(FsTestsCreateFile(DirFd, FS_TEST_FILE_A, FS_TEST_CONTENT));
    LxtCheckResult(FsTestsCreateFile(DirFd, FS_TEST_FILE_B, FS_TEST_CONTENT_B));

    /* RENAME_NOREPLACE: should fail because target exists. */
    LxtCheckErrnoFailure(
        renameat2(DirFd, FS_TEST_FILE_A, DirFd, FS_TEST_FILE_B, RENAME_NOREPLACE),
        EEXIST);

    /* Remove target, then RENAME_NOREPLACE should succeed. */
    LxtCheckErrnoZeroSuccess(unlinkat(DirFd, FS_TEST_FILE_B, 0));
    LxtCheckErrnoZeroSuccess(
        renameat2(DirFd, FS_TEST_FILE_A, DirFd, FS_TEST_FILE_B, RENAME_NOREPLACE));

    /* Original should not exist. */
    LxtCheckErrnoFailure(fstatat(DirFd, FS_TEST_FILE_A, &StatBuf, 0), ENOENT);

    /* Target should exist. */
    LxtCheckErrnoZeroSuccess(fstatat(DirFd, FS_TEST_FILE_B, &StatBuf, 0));

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    if (DirFd >= 0) {
        close(DirFd);
    }

    (void)unlink(FS_TEST_DIR "/" FS_TEST_FILE_A);
    (void)unlink(FS_TEST_DIR "/" FS_TEST_FILE_B);
    return Result;
}

/*
 * Variation: Test linkat syscall with various flags.
 */

int
FsTestsLinkatVariation(
    PLXT_ARGS Args
    )
{
    int DirFd;
    int Result;
    struct stat StatBufOrig;
    struct stat StatBufLink;

    DirFd = -1;

    LxtCheckErrno(DirFd = open(FS_TEST_DIR, O_RDONLY | O_DIRECTORY));
    LxtCheckResult(FsTestsCreateFile(DirFd, FS_TEST_FILE_A, FS_TEST_CONTENT));

    /* Create a hard link. */
    LxtCheckErrnoZeroSuccess(
        linkat(DirFd, FS_TEST_FILE_A, DirFd, FS_TEST_LINK, 0));

    /* Both should refer to the same inode. */
    LxtCheckErrnoZeroSuccess(fstatat(DirFd, FS_TEST_FILE_A, &StatBufOrig, 0));
    LxtCheckErrnoZeroSuccess(fstatat(DirFd, FS_TEST_LINK, &StatBufLink, 0));
    LxtCheckEqual(StatBufOrig.st_ino, StatBufLink.st_ino, "%lu");

    /* Link count should be 2. */
    LxtCheckEqual(StatBufOrig.st_nlink, 2, "%lu");

    /* Create a symlink, then linkat with AT_SYMLINK_FOLLOW. */
    LxtCheckErrnoZeroSuccess(
        symlinkat(FS_TEST_FILE_A, DirFd, FS_TEST_SYMLINK));

    /* Remove old hard link to get clean state. */
    LxtCheckErrnoZeroSuccess(unlinkat(DirFd, FS_TEST_LINK, 0));

    /* linkat with AT_SYMLINK_FOLLOW should follow the symlink. */
    LxtCheckErrnoZeroSuccess(
        linkat(DirFd, FS_TEST_SYMLINK, DirFd, FS_TEST_LINK, AT_SYMLINK_FOLLOW));

    LxtCheckErrnoZeroSuccess(fstatat(DirFd, FS_TEST_LINK, &StatBufLink, 0));
    LxtCheckTrue(S_ISREG(StatBufLink.st_mode));
    LxtCheckEqual(StatBufOrig.st_ino, StatBufLink.st_ino, "%lu");

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    if (DirFd >= 0) {
        close(DirFd);
    }

    (void)unlink(FS_TEST_DIR "/" FS_TEST_FILE_A);
    (void)unlink(FS_TEST_DIR "/" FS_TEST_LINK);
    (void)unlink(FS_TEST_DIR "/" FS_TEST_SYMLINK);
    return Result;
}

/*
 * Variation: Test error handling for various syscalls.
 */

int
FsTestsErrorHandlingVariation(
    PLXT_ARGS Args
    )
{
    int DirFd;
    int Result;
    struct stat StatBuf;
    struct statvfs VfsBuf;

    DirFd = -1;

    LxtCheckErrno(DirFd = open(FS_TEST_DIR, O_RDONLY | O_DIRECTORY));

    /* fstatat on nonexistent file should return ENOENT. */
    LxtCheckErrnoFailure(
        fstatat(DirFd, "nonexistent_file_xyz", &StatBuf, 0),
        ENOENT);

    /* statvfs on nonexistent path should return ENOENT. */
    LxtCheckErrnoFailure(
        statvfs("/nonexistent_path_xyz/does_not_exist", &VfsBuf),
        ENOENT);

    /* linkat with nonexistent source should return ENOENT. */
    LxtCheckErrnoFailure(
        linkat(DirFd, "nonexistent_src", DirFd, "link_target", 0),
        ENOENT);

    /* renameat2 with nonexistent source should return ENOENT. */
    LxtCheckErrnoFailure(
        renameat2(DirFd, "nonexistent_src", DirFd, "rename_target", 0),
        ENOENT);

    /* unlinkat on nonexistent file should return ENOENT. */
    LxtCheckErrnoFailure(
        unlinkat(DirFd, "nonexistent_file", 0),
        ENOENT);

    /* fstatat with invalid fd should return EBADF. */
    LxtCheckErrnoFailure(
        fstatat(-1, "anything", &StatBuf, 0),
        EBADF);

    /* linkat to existing target should fail with EEXIST. */
    LxtCheckResult(FsTestsCreateFile(DirFd, FS_TEST_FILE_A, FS_TEST_CONTENT));
    LxtCheckResult(FsTestsCreateFile(DirFd, FS_TEST_FILE_B, FS_TEST_CONTENT_B));
    LxtCheckErrnoFailure(
        linkat(DirFd, FS_TEST_FILE_A, DirFd, FS_TEST_FILE_B, 0),
        EEXIST);

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    if (DirFd >= 0) {
        close(DirFd);
    }

    (void)unlink(FS_TEST_DIR "/" FS_TEST_FILE_A);
    (void)unlink(FS_TEST_DIR "/" FS_TEST_FILE_B);
    return Result;
}

/*
 * Variation: Edge cases for statfs - test on proc, sys, devpts.
 */

int
FsTestsStatfsEdgeCasesVariation(
    PLXT_ARGS Args
    )
{
    int Result;
    struct statvfs StatBuf;

    /* /proc is a pseudo-filesystem. */
    LxtCheckErrnoZeroSuccess(statvfs("/proc", &StatBuf));
    LxtCheckTrue(StatBuf.f_bsize > 0);

    /* /sys is sysfs. */
    LxtCheckErrnoZeroSuccess(statvfs("/sys", &StatBuf));
    LxtCheckTrue(StatBuf.f_bsize > 0);

    /* /dev is devtmpfs. */
    LxtCheckErrnoZeroSuccess(statvfs("/dev", &StatBuf));
    LxtCheckTrue(StatBuf.f_bsize > 0);

    /* /tmp should be accessible. */
    LxtCheckErrnoZeroSuccess(statvfs("/tmp", &StatBuf));
    LxtCheckTrue(StatBuf.f_blocks > 0);

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    return Result;
}

/*
 * Variation: Test fstatat with AT_SYMLINK_NOFOLLOW and symlinks.
 */

int
FsTestsFstatatFlagsVariation(
    PLXT_ARGS Args
    )
{
    int DirFd;
    int Result;
    struct stat StatBuf;

    DirFd = -1;

    LxtCheckErrno(DirFd = open(FS_TEST_DIR, O_RDONLY | O_DIRECTORY));
    LxtCheckResult(FsTestsCreateFile(DirFd, FS_TEST_FILE_A, FS_TEST_CONTENT));

    /* Create a symlink. */
    LxtCheckErrnoZeroSuccess(
        symlinkat(FS_TEST_FILE_A, DirFd, FS_TEST_SYMLINK));

    /* fstatat without NOFOLLOW should follow symlink → regular file. */
    LxtCheckErrnoZeroSuccess(fstatat(DirFd, FS_TEST_SYMLINK, &StatBuf, 0));
    LxtCheckTrue(S_ISREG(StatBuf.st_mode));

    /* fstatat with AT_SYMLINK_NOFOLLOW should see the symlink itself. */
    LxtCheckErrnoZeroSuccess(
        fstatat(DirFd, FS_TEST_SYMLINK, &StatBuf, AT_SYMLINK_NOFOLLOW));
    LxtCheckTrue(S_ISLNK(StatBuf.st_mode));

    /* Verify sizes differ between symlink and target. */
    {
        struct stat TargetStat;
        LxtCheckErrnoZeroSuccess(fstatat(DirFd, FS_TEST_FILE_A, &TargetStat, 0));
        /* Symlink's st_size is the length of the target path string. */
        LxtCheckEqual(StatBuf.st_size, (off_t)strlen(FS_TEST_FILE_A), "%ld");
    }

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    if (DirFd >= 0) {
        close(DirFd);
    }

    (void)unlink(FS_TEST_DIR "/" FS_TEST_FILE_A);
    (void)unlink(FS_TEST_DIR "/" FS_TEST_SYMLINK);
    return Result;
}

/*
 * Variation: Test renameat2 across directories.
 */

int
FsTestsRenameat2CrossDirVariation(
    PLXT_ARGS Args
    )
{
    int DirFd;
    int SubDirFd;
    int Result;
    struct stat StatBuf;

    DirFd = -1;
    SubDirFd = -1;

    LxtCheckErrno(DirFd = open(FS_TEST_DIR, O_RDONLY | O_DIRECTORY));
    LxtCheckErrno(SubDirFd = open(FS_TEST_DIR "/" FS_TEST_SUBDIR, O_RDONLY | O_DIRECTORY));

    /* Create a file in the parent directory. */
    LxtCheckResult(FsTestsCreateFile(DirFd, FS_TEST_FILE_A, FS_TEST_CONTENT));

    /* Rename across directories: parent → subdir. */
    LxtCheckErrnoZeroSuccess(
        renameat2(DirFd, FS_TEST_FILE_A, SubDirFd, FS_TEST_FILE_A, 0));

    /* File should no longer exist in parent. */
    LxtCheckErrnoFailure(fstatat(DirFd, FS_TEST_FILE_A, &StatBuf, 0), ENOENT);

    /* File should exist in subdir. */
    LxtCheckErrnoZeroSuccess(fstatat(SubDirFd, FS_TEST_FILE_A, &StatBuf, 0));
    LxtCheckTrue(S_ISREG(StatBuf.st_mode));

    /* Move it back. */
    LxtCheckErrnoZeroSuccess(
        renameat2(SubDirFd, FS_TEST_FILE_A, DirFd, FS_TEST_FILE_A, 0));

    /* Verify it's back in parent. */
    LxtCheckErrnoZeroSuccess(fstatat(DirFd, FS_TEST_FILE_A, &StatBuf, 0));

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    if (SubDirFd >= 0) {
        close(SubDirFd);
    }

    if (DirFd >= 0) {
        close(DirFd);
    }

    (void)unlink(FS_TEST_DIR "/" FS_TEST_FILE_A);
    (void)unlink(FS_TEST_DIR "/" FS_TEST_SUBDIR "/" FS_TEST_FILE_A);
    return Result;
}
