/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    fscommon.c

Abstract:

    This file contains common FS unit tests that are run on both LxFs and DrvFs.

--*/

#include "lxtcommon.h"
#include "unittests.h"
#include <poll.h>
#include <dirent.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/uio.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/syscall.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <linux/capability.h>
#include <sys/mount.h>
#include <sys/sysmacros.h>
#include <libmount/libmount.h>
#include "lxtmount.h"
#include "lxtfs.h"

#define LXT_NAME_LXFS "fscommon_lxfs"
#define LXT_NAME_DRVFS "fscommon_drvfs"

#define FS_TEST_DIR_PARENT "/data/fstest"
#define FS_CLEX_TEST_DIR_NAME FS_TEST_DIR_PARENT "/CLEX_test"
#define FS_READLINK_TEST_FILE FS_TEST_DIR_PARENT "/readlink_testfile"
#define FS_READLINK_TEST_LINK FS_TEST_DIR_PARENT "/readlink_testlink"
#define FS_RENAMEAT_TEST_DIR FS_TEST_DIR_PARENT "/rename_test"
#define FS_TRAILING_TEST_FILE FS_TEST_DIR_PARENT "/trailing_test_file"
#define FS_TRAILING_TEST_DIR FS_TEST_DIR_PARENT "/trailing_test_dir"
#define FS_TRAILING_TEST_LINK FS_TEST_DIR_PARENT "/trailing_test_link"
#define FS_MKNOD_TEST_FILE FS_TEST_DIR_PARENT "/myzero"
#define FS_MKNOD_TEST_FILE2 FS_TEST_DIR_PARENT "/myzero2"
#define FS_CHROOT_TEST_DIR FS_TEST_DIR_PARENT "/chroot_test"
#define FS_CHROOT_TEST_DIR_CHILD_FROM_ROOT "/child"
#define FS_CHROOT_TEST_DIR_CHILD FS_CHROOT_TEST_DIR FS_CHROOT_TEST_DIR_CHILD_FROM_ROOT
#define FS_CHROOT_TEST_DIR_PROC FS_CHROOT_TEST_DIR "/proc"
#define FS_FALLOCATE_TEST_FILE FS_TEST_DIR_PARENT "/fallocate_test_file"
#define FS_RMDIR_TEST_DIR FS_TEST_DIR_PARENT "/rmdir_test"
#define FS_POWERSHELL "/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe"
#define FS_POWERSHELL_MOUNT_COMMAND \
    FS_POWERSHELL " -Command \"& { (Mount-DiskImage " FS_DRVFS_CD_TEST_ISO_NT " -PassThru | Get-Volume).DriveLetter }\""
#define FS_POWERSHELL_UNMOUNT_COMMAND FS_POWERSHELL " -Command \"& { Dismount-DiskImage " FS_DRVFS_CD_TEST_ISO_NT " }\""
#define FS_GENISOIMAGE_COMMAND "genisoimage -JR -o " FS_DRVFS_CD_TEST_ISO " " FS_DRVFS_CD_CONTENTS_DIR
#define FS_LINKAT_TEST_DIR FS_TEST_DIR_PARENT "/linkat_test"
#define FS_LINKAT_TEST_DIR2 FS_TEST_DIR_PARENT "/linkat_test2"
#define FS_FCHOWNAT_TEST_DIR FS_TEST_DIR_PARENT "/fchownat_test"
#define FS_DELETELOOP_TEST_DIR FS_TEST_DIR_PARENT "/deleteloop"
#define FS_FSYNC_TEST_DIR FS_TEST_DIR_PARENT "/fsync_test"

LXT_VARIATION_HANDLER FsCommonTestCreateAndRename;
LXT_VARIATION_HANDLER FsCommonTestDeleteCurrentWorkingDirectory;
LXT_VARIATION_HANDLER FsCommonTestDeleteLoop;
LXT_VARIATION_HANDLER FsCommonTestDeleteOpenFile;
LXT_VARIATION_HANDLER FsCommonTestChroot;
LXT_VARIATION_HANDLER FsCommonTestClex;
LXT_VARIATION_HANDLER FsCommonTestCreateSymlinkTarget;
LXT_VARIATION_HANDLER FsCommonTestDeviceId;
LXT_VARIATION_HANDLER FsCommonTestFchownAt;
LXT_VARIATION_HANDLER FsCommonTestReadlinkat;
LXT_VARIATION_HANDLER FsCommonTestRemoveSelfOrParent;
LXT_VARIATION_HANDLER FsCommonTestRenameAt;
LXT_VARIATION_HANDLER FsCommonTestRenameDir;
LXT_VARIATION_HANDLER FsCommonTestSetEof;
LXT_VARIATION_HANDLER FsCommonTestTrailingSlash;
LXT_VARIATION_HANDLER FsCommonTestLinkAt;
LXT_VARIATION_HANDLER FsCommonTestMkdir;
LXT_VARIATION_HANDLER FsCommonTestMkDirAt;
LXT_VARIATION_HANDLER FsCommonTestMknod;
LXT_VARIATION_HANDLER FsCommonTestMknodSecurity;
LXT_VARIATION_HANDLER FsCommonTestOpen;
LXT_VARIATION_HANDLER FsCommonTestOpenAt;
LXT_VARIATION_HANDLER FsCommonTestOpenCreateSymlink;
LXT_VARIATION_HANDLER FsCommonTestOpenCreateSymlinkDir;
#ifdef __NR_getdents
LXT_VARIATION_HANDLER FsCommonTestGetDents;
#endif
LXT_VARIATION_HANDLER FsCommonTestGetDents64Alignment;
#ifdef __NR_getdents
LXT_VARIATION_HANDLER FsCommonTestGetDentsAlignment;
#endif
LXT_VARIATION_HANDLER FsCommonTestGetDentsTypes;
LXT_VARIATION_HANDLER FsCommonTestChdir;
LXT_VARIATION_HANDLER FsCommonTestUnlinkAt;
LXT_VARIATION_HANDLER FsCommonTestFstatAt64;
LXT_VARIATION_HANDLER FsCommonTestFchdir;
LXT_VARIATION_HANDLER FsCommonTestNoatimeFlag;
LXT_VARIATION_HANDLER FsCommonTestWritev;
LXT_VARIATION_HANDLER FsCommonTestFallocate;
LXT_VARIATION_HANDLER FsCommonTestDirSeek;
LXT_VARIATION_HANDLER FsCommonTestFsync;

//
// Global constants
//

static const LXT_VARIATION g_LxtVariations[] = {
    {"Test mkdir/rmdir", FsCommonTestMkdir},
    {"Test SetEof", FsCommonTestSetEof},
    {"Test Create, Rename and unlink", FsCommonTestCreateAndRename},
    {"Test Open", FsCommonTestOpen},
    {"Test OpenAt", FsCommonTestOpenAt},
    {"Test Open symlink with O_CREAT", FsCommonTestOpenCreateSymlink},
    {"Test creating a symlink to a directory", FsCommonTestOpenCreateSymlinkDir},
    {"Test Chdir", FsCommonTestChdir},
#ifdef __NR_getdents
    {"Test GetDents", FsCommonTestGetDents},
#endif
    {"Test UnlinkAt", FsCommonTestUnlinkAt},
    {"Test fstatat64", FsCommonTestFstatAt64},
    {"Test Fchdir", FsCommonTestFchdir},
    {"Test mkdirat", FsCommonTestMkDirAt},
    {"Test O_NOATIME flag", FsCommonTestNoatimeFlag},
    {"Test deleting an open file", FsCommonTestDeleteOpenFile},
    {"Test deleting the working directory", FsCommonTestDeleteCurrentWorkingDirectory},
    {"Test rename directory", FsCommonTestRenameDir},
    {"Test writev", FsCommonTestWritev},
    {"Test readlinkat", FsCommonTestReadlinkat},
    {"Test renameat", FsCommonTestRenameAt},
    {"Test DeviceId", FsCommonTestDeviceId},
    {"Test FIOCLEX/FIONCLEX", FsCommonTestClex},
    {"Test create symlink target", FsCommonTestCreateSymlinkTarget},
    {"Test trailing slash", FsCommonTestTrailingSlash},
    {"Test mknod", FsCommonTestMknod},
    {"Test mknod CAP_MKNOD", FsCommonTestMknodSecurity},
    {"Test chroot", FsCommonTestChroot},
    {"Test fallocate", FsCommonTestFallocate},
    {"Test remove self or parent", FsCommonTestRemoveSelfOrParent},
    {"Test linkat", FsCommonTestLinkAt},
    {"Test fchownat", FsCommonTestFchownAt},
    {"Test delete loop", FsCommonTestDeleteLoop},
#ifdef __NR_getdents
    {"Test getdents alignment", FsCommonTestGetDentsAlignment},
#endif
    {"Test getdents64 alignment", FsCommonTestGetDents64Alignment},
    {"Test lseek on directory", FsCommonTestDirSeek},
    {"Test getdents file types", FsCommonTestGetDentsTypes},
    {"Test fsync", FsCommonTestFsync}};

int FsCommonTestEntry(int Argc, char* Argv[])

/*++
--*/

{

    LXT_ARGS Args;
    int Index;
    const char* Name;
    int Result;
    bool UseDrvFs;

    //
    // Check if drvfs should be used.
    //

    Name = LXT_NAME_LXFS;
    UseDrvFs = false;
    for (Index = 1; Index < Argc; Index += 1)
    {
        if (strcmp(Argv[Index], "drvfs") == 0)
        {
            UseDrvFs = true;
            Name = LXT_NAME_DRVFS;
            break;
        }
    }

    LxtCheckResult(LxtInitialize(Argc, Argv, &Args, Name));
    LXT_SYNCHRONIZATION_POINT_INIT();
    LxtCheckResult(LxtFsTestSetup(&Args, FS_TEST_DIR_PARENT, "/fstest", UseDrvFs));

    LxtCheckResult(LxtRunVariations(&Args, g_LxtVariations, LXT_COUNT_OF(g_LxtVariations)));

ErrorExit:
    LxtFsTestCleanup(FS_TEST_DIR_PARENT, "/fstest", UseDrvFs);
    LXT_SYNCHRONIZATION_POINT_DESTROY();
    LxtUninitialize();
    return !LXT_SUCCESS(Result);
}

#if !defined(__amd64__) && !defined(__aarch64__)

struct dirent64
{
    __u64 d_ino;
    __s64 d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[1];
};

#endif

struct GetDentsPaths
{
    const char* Path;
    int MinElements;
    int MaxElements;
};

#define LXT_GET_DENTS_FOLDER FS_TEST_DIR_PARENT "/getdents"

int FsCommonTestChroot(PLXT_ARGS Args)

/*++

Description:

    This routine tests some chroot effects that are not covered by LTP.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    pid_t ChildPid;
    int Fd;
    int Fd2;
    char Path[PATH_MAX];
    int Result;

    Fd = -1;
    Fd2 = -2;

    //
    // This test is not really relevant to VM mode, and currently doesn't pass
    // because VM mode already runs in a chroot environment, changing some of
    // paths.
    //
    // TODO_LX: Re-enable this when init switches to using pivot_root.
    //

    if (g_LxtFsInfo.FsType == LxtFsTypePlan9)
    {
        LxtLogInfo("Skipping chroot test in VM mode.");
        Result = LXT_RESULT_SUCCESS;
        goto ErrorExit;
    }

    //
    // Set up the directories needed for the chroot environment.
    //

    LxtCheckResult(LxtSignalBlock(SIGUSR1));
    LxtCheckErrnoZeroSuccess(mkdir(FS_CHROOT_TEST_DIR, 0777));
    LxtCheckErrnoZeroSuccess(mkdir(FS_CHROOT_TEST_DIR_CHILD, 0777));
    LxtCheckErrnoZeroSuccess(mkdir(FS_CHROOT_TEST_DIR_PROC, 0777));
    LxtCheckErrnoZeroSuccess(mount("/proc", FS_CHROOT_TEST_DIR_PROC, NULL, MS_BIND, NULL));

    //
    // First test with the cwd inside the new root when chroot is called.
    //
    // N.B. The parent cwd is outside the new root.
    //

    LxtLogInfo("Cwd inside new root...");
    LxtCheckErrnoZeroSuccess(chdir(FS_TEST_DIR_PARENT));
    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        //
        // Change current directory and open a file both in and outside the
        // new root.
        //

        LxtCheckErrnoZeroSuccess(chdir(FS_CHROOT_TEST_DIR_CHILD));
        LxtCheckErrno(Fd = open("/etc/hosts", O_RDONLY));
        LxtCheckErrno(Fd2 = open(FS_CHROOT_TEST_DIR_CHILD, O_DIRECTORY));
        LxtCheckErrnoZeroSuccess(access("../../../../etc", F_OK));
        LxtCheckErrnoZeroSuccess(faccessat(Fd2, "../../../../etc", F_OK, 0));

        //
        // Change the root directory.
        //

        LxtCheckErrnoZeroSuccess(chroot(FS_CHROOT_TEST_DIR));

        //
        // The working directory path and fd inside the new root should
        // reflect the new root.
        //
        // N.B. The working directory is not changed by chroot; because it is
        //      inside the reported path is changed automatically.
        //

        LxtCheckErrno(LxtGetcwd(Path, sizeof(Path)));
        LxtCheckStringEqual(Path, FS_CHROOT_TEST_DIR_CHILD_FROM_ROOT);
        LxtCheckResult(LxtCheckLinkTarget("/proc/self/cwd", FS_CHROOT_TEST_DIR_CHILD_FROM_ROOT));

        LxtCheckResult(LxtCheckFdPath(Fd2, FS_CHROOT_TEST_DIR_CHILD_FROM_ROOT));

        //
        // The file descriptor outside the root still reports its old path.
        //

        LxtCheckResult(LxtCheckFdPath(Fd, "/etc/hosts"));

        //
        // Check that the root can't be escaped.
        //

        LxtCheckErrnoFailure(access("/etc", F_OK), ENOENT);
        LxtCheckErrnoFailure(access("../../../../etc", F_OK), ENOENT);
        LxtCheckErrnoFailure(faccessat(Fd2, "../../../../etc", F_OK, 0), ENOENT);

        //
        // The root symlink should say /, and refer to the new root.
        //

        LxtCheckResult(LxtCheckLinkTarget("/proc/self/root", "/"));
        LxtCheckErrnoFailure(access("/proc/self/root/etc", F_OK), ENOENT);

        //
        // The parent's root symlink also says /, even though it's not the same
        // path. It can be used to escape the chroot jail.
        //

        sprintf(Path, "/proc/%d/root", getppid());
        LxtCheckResult(LxtCheckLinkTarget(Path, "/"));
        sprintf(Path, "/proc/%d/root/etc", getppid());
        LxtCheckErrnoZeroSuccess(access(Path, F_OK));

        //
        // The parent's cwd is not inside the new root, so the link returns
        // its actual path. It can also be used to escape the chroot jail.
        //

        sprintf(Path, "/proc/%d/cwd", getppid());
        LxtCheckResult(LxtCheckLinkTarget(Path, FS_TEST_DIR_PARENT));
        sprintf(Path, "/proc/%d/cwd/chroot_test", getppid());
        LxtCheckErrnoZeroSuccess(access(Path, F_OK));

        //
        // Signal the parent.
        //

        LxtCheckErrnoZeroSuccess(kill(getppid(), SIGUSR1));
        LxtCheckResult(LxtSignalWaitBlocked(SIGUSR1, getppid(), 2));
        exit(0);
    }

    LxtCheckResult(LxtSignalWaitBlocked(SIGUSR1, ChildPid, 2));

    //
    // Check the root symlink for the child returns the new root path.
    //

    sprintf(Path, "/proc/%d/root", ChildPid);
    LxtCheckResult(LxtCheckLinkTarget(Path, FS_CHROOT_TEST_DIR));
    sprintf(Path, "/proc/%d/cwd", ChildPid);
    LxtCheckResult(LxtCheckLinkTarget(Path, FS_CHROOT_TEST_DIR_CHILD));
    LxtCheckErrnoZeroSuccess(kill(ChildPid, SIGUSR1));
    LxtCheckResult(LxtWaitPidPoll(ChildPid, 0));

    //
    // Now test with cwd outside the new root, in which case the path reported
    // by getcwd should indicate unreachable, but /proc/self/cwd should give
    // the normal path.
    //
    // N.B. The parent cwd is inside the new root for this test.
    //

    LxtLogInfo("Cwd outside new root...");
    LxtCheckErrnoZeroSuccess(chdir(FS_CHROOT_TEST_DIR_CHILD));
    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        LxtCheckErrnoZeroSuccess(chdir(FS_TEST_DIR_PARENT));
        LxtCheckErrnoZeroSuccess(chroot(FS_CHROOT_TEST_DIR));

        //
        // Glibc getcwd in newer versions returns NULL if the path doesn't
        // start with a /, which would be the case here, so call the syscall
        // directly.
        //

        LxtCheckErrno(LxtGetcwd(Path, sizeof(Path)));
        LxtCheckStringEqual(Path, "(unreachable)" FS_TEST_DIR_PARENT);
        LxtCheckResult(LxtCheckLinkTarget("/proc/self/cwd", FS_TEST_DIR_PARENT));

        //
        // The parent's cwd is reported using the new root.
        //

        sprintf(Path, "/proc/%d/cwd", getppid());
        LxtCheckResult(LxtCheckLinkTarget(Path, FS_CHROOT_TEST_DIR_CHILD_FROM_ROOT));

        exit(0);
    }

    LxtCheckResult(LxtWaitPidPoll(ChildPid, 0));

    //
    // Cwd matches the new root.
    //

    LxtLogInfo("Cwd exactly new root...");
    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        LxtCheckErrnoZeroSuccess(chdir(FS_CHROOT_TEST_DIR));
        LxtCheckErrnoZeroSuccess(chroot(FS_CHROOT_TEST_DIR));
        LxtCheckErrno(LxtGetcwd(Path, sizeof(Path)));
        LxtCheckStringEqual(Path, "/");
        LxtCheckResult(LxtCheckLinkTarget("/proc/self/cwd", "/"));
        exit(0);
    }

    LxtCheckResult(LxtWaitPidPoll(ChildPid, 0));

    //
    // Cwd is the old root.
    //

    LxtLogInfo("Cwd exactly old root...");
    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        LxtCheckErrnoZeroSuccess(chdir("/"));
        LxtCheckErrnoZeroSuccess(chroot(FS_CHROOT_TEST_DIR));

        //
        // Glibc getcwd in newer versions returns NULL if the path doesn't
        // start with a /, which would be the case here, so call the syscall
        // directly.
        //

        LxtCheckErrno(LxtGetcwd(Path, sizeof(Path)));
        LxtCheckStringEqual(Path, "(unreachable)/");
        memset(Path, 0, sizeof(Path));
        LxtCheckErrno(readlink("/proc/self/cwd", Path, sizeof(Path)));
        LxtCheckStringEqual(Path, "/");
        exit(0);
    }

    LxtCheckResult(LxtWaitPidPoll(ChildPid, 0));

ErrorExit:
    if (Fd >= 0)
    {
        close(Fd);
    }

    if (Fd2 >= 0)
    {
        close(Fd2);
    }

    umount(FS_CHROOT_TEST_DIR_PROC);
    rmdir(FS_CHROOT_TEST_DIR_PROC);
    rmdir(FS_CHROOT_TEST_DIR_CHILD);
    rmdir(FS_CHROOT_TEST_DIR);
    return Result;
}

int FsCommonTestClex(PLXT_ARGS Args)

/*++

Description:

    This routine tests the FIONCLEX / FIONCLEX file descriptor ioctls.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    pid_t ChildPid;
    int Fd = -1;
    int Result;

    //
    // Don't set close on exec on file descriptors in the main test process;
    // this would cause later tests to fail.
    //

    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {

        //
        // First set the ioctls on stdin.
        //

        LxtCheckErrno(ioctl(0, FIONCLEX, NULL));
        LxtCheckErrno(ioctl(0, FIOCLEX, NULL));

        //
        // Create a directory and open a file descriptor with O_PATH.
        //

        LxtCheckErrno(mkdir(FS_CLEX_TEST_DIR_NAME, 0777));
        LxtCheckErrno(Fd = open(FS_CLEX_TEST_DIR_NAME, O_PATH | O_DIRECTORY));

        //
        // Setting the CLOEXEC flag with fcntl should work even though the file
        // was opened with O_PATH.
        //

        LxtCheckErrno(fcntl(Fd, F_SETFD, FD_CLOEXEC));

        //
        // Setting FIONCLEX / FIOCLEX with the ioctl syscall should fail.
        //

        LxtCheckErrnoFailure(ioctl(Fd, FIONCLEX, NULL), EBADF);
        LxtCheckErrnoFailure(ioctl(Fd, FIOCLEX, NULL), EBADF);
        exit(0);
    }

    LxtCheckResult(LxtWaitPidPoll(ChildPid, 0));
    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    rmdir(FS_CLEX_TEST_DIR_NAME);
    if (Fd != -1)
    {
        LxtClose(Fd);
    }

    return Result;
}

typedef struct _LXSS_BYTE_ALIGNED_DIRENTS
{
    char Padding;
    char Buffer[sizeof(struct dirent64)];
} LXSS_BYTE_ALIGNED_DIRENTS;

#ifdef __NR_getdents
int FsCommonTestGetDents(PLXT_ARGS Args)

/*++
--*/

{

    LXSS_BYTE_ALIGNED_DIRENTS ByteAlignedDirents;
    char* Buffer;
    struct dirent64* BufferEntries[2000];
    int BufferEntriesCount;
    int BufferIndex;
    int BufferLoopIndex;
    int BufferSize = 2 * 1024 * 1024;
    int BytePos;
    int BytesRead;
    int DirFd;
    struct dirent64* Entry;
    int ExpectedLength;
    bool FoundDot;
    bool FoundDotDot;
    int NameLength;
    int Pass;
    size_t PathIndex;

    //
    // TODO: use cgroups path once it is mounted for 64 bit lxss.
    //

    struct GetDentsPaths Paths[] = {
        {"/proc/self/", 18, 64},
        {"/proc/", 10, 500},
        {"/dev/", 14, 1000},
        //{"/acct/", 7, 64},
        {"/", 4, 64},
        {LXT_GET_DENTS_FOLDER, 2, 2}};

    Buffer = NULL;
    DirFd = -1;
    int Result;
    char SingleEntry[100];
    int SingleEntrySize;
    rmdir(LXT_GET_DENTS_FOLDER);

    //
    // Check the expected getdents results for each directory;
    //

    Buffer = malloc(BufferSize);
    if (Buffer == NULL)
    {
        Result = LXT_RESULT_FAILURE;
        LxtLogError("malloc");
        goto ErrorExit;
    }

    LxtCheckErrno(mkdir(LXT_GET_DENTS_FOLDER, 0777));
    for (PathIndex = 0; PathIndex < LXT_COUNT_OF(Paths); PathIndex += 1)
    {

        //
        // First read all of the entries in a single call.
        //

        memset(Buffer, 1, BufferSize);
        LxtLogInfo("Opening %s...", Paths[PathIndex].Path);
        LxtCheckErrno(DirFd = open(Paths[PathIndex].Path, O_RDONLY | O_DIRECTORY));
        LxtCheckErrno(BytesRead = LxtGetdents64(DirFd, Buffer, BufferSize));
        if (BytesRead == 0)
        {
            Result = LXT_RESULT_FAILURE;
            LxtLogError("BytesRead == 0");
            goto ErrorExit;
        }

        LxtCheckErrno(LxtGetdents(DirFd, SingleEntry, sizeof(SingleEntry)));
        if (Result != 0)
        {
            Result = LXT_RESULT_FAILURE;
            LxtLogError("BytesRead Result ! 0");
            goto ErrorExit;
        }

        FoundDot = false;
        FoundDotDot = false;
        BufferIndex = 0;
        for (BytePos = 0; BytePos < BytesRead;)
        {
            Entry = (struct dirent64*)(Buffer + BytePos);
            BufferEntries[BufferIndex] = Entry;
            NameLength = strlen(Entry->d_name);

            if (strcmp(Entry->d_name, ".") == 0)
            {
                FoundDot = true;
            }

            if (strcmp(Entry->d_name, "..") == 0)
            {
                FoundDotDot = true;
            }

            BufferIndex += 1;
            BytePos += Entry->d_reclen;
        }

        if ((FoundDot == false) || (FoundDotDot == false))
        {
            Result = LXT_RESULT_FAILURE;
            LxtLogError("Missing entries for . or .. or both.");
            goto ErrorExit;
        }

        BufferEntriesCount = BufferIndex;
        if (BufferEntriesCount < Paths[PathIndex].MinElements)
        {
            Result = LXT_RESULT_FAILURE;
            LxtLogError("Unexpected number of elements %d < %d", BufferEntriesCount, Paths[PathIndex].MinElements);
            goto ErrorExit;
        }

        if (BufferEntriesCount > Paths[PathIndex].MaxElements)
        {
            Result = LXT_RESULT_FAILURE;
            LxtLogError("Unexpected number of elements %d > %d", BufferEntriesCount, Paths[PathIndex].MaxElements);
            goto ErrorExit;
        }

        LxtClose(DirFd);
        DirFd = -1;

        //
        // Then read each entry in a single call and make sure it matches the
        // previous data returned.
        //
        // In pass 0, just read sequentially. In pass 1, seek to each offset
        // in reverse order to ensure that seek works.
        //

        for (Pass = 0; Pass < 2; Pass++)
        {
            LxtLogInfo("Reopening %s...", Paths[PathIndex].Path);
            LxtCheckErrno(DirFd = open(Paths[PathIndex].Path, O_RDONLY | O_DIRECTORY));
            for (BufferLoopIndex = 0; BufferLoopIndex < BufferEntriesCount; BufferLoopIndex += 1)
            {
                if (Pass == 0)
                {
                    BufferIndex = BufferLoopIndex;
                }
                else
                {
                    BufferIndex = BufferEntriesCount - BufferLoopIndex - 1;

                    //
                    // Plan 9 client in Linux has a bug where seek does not
                    // take effect if not all entries were consumed. Reopen
                    // the FD to allow seek to work.
                    //
                    // TODO_LX: Remove this one the plan 9 bug is fixed.
                    //

                    if (g_LxtFsInfo.FsType == LxtFsTypePlan9)
                    {
                        LxtCheckClose(DirFd);
                        LxtCheckErrno(DirFd = open(Paths[PathIndex].Path, O_RDONLY | O_DIRECTORY));
                    }

                    LxtCheckErrno(lseek(DirFd, BufferIndex == 0 ? 0 : BufferEntries[BufferIndex - 1]->d_off, SEEK_SET));
                }

                SingleEntrySize = BufferEntries[BufferIndex]->d_reclen;
                BytesRead = LxtGetdents64(DirFd, SingleEntry, SingleEntrySize);
                if (BytesRead < 0)
                {
                    Result = LXT_RESULT_FAILURE;
                    LxtLogError("Failed on %s with %s", BufferEntries[BufferIndex]->d_name, strerror(errno));
                    goto ErrorExit;
                }

                if (BytesRead == 0)
                {
                    Result = LXT_RESULT_FAILURE;
                    LxtLogError("BytesRead == 0");
                    goto ErrorExit;
                }

                Entry = (struct dirent64*)SingleEntry;
                if (Entry->d_reclen != BufferEntries[BufferIndex]->d_reclen)
                {
                    Result = LXT_RESULT_FAILURE;
                    LxtLogInfo("Unexpected d_reclen %d != %d", Entry->d_reclen, BufferEntries[BufferIndex]->d_reclen);
                    goto ErrorExit;
                }

                if (strcmp(Entry->d_name, BufferEntries[BufferIndex]->d_name) != 0)
                {
                    Result = LXT_RESULT_FAILURE;
                    LxtLogError("Unexpected name %s != %s", Entry->d_name, BufferEntries[BufferIndex]->d_name);
                    goto ErrorExit;
                }

                if (Entry->d_type != BufferEntries[BufferIndex]->d_type)
                {
                    Result = LXT_RESULT_FAILURE;
                    LxtLogInfo("Unexpected d_type %d != %d", Entry->d_type, BufferEntries[BufferIndex]->d_type);
                    goto ErrorExit;
                }

                if (Entry->d_reclen != BufferEntries[BufferIndex]->d_reclen)
                {
                    Result = LXT_RESULT_FAILURE;
                    LxtLogInfo("Unexpected d_reclen %d != %d", Entry->d_reclen, BufferEntries[BufferIndex]->d_reclen);
                    goto ErrorExit;
                }

                if (Entry->d_off != BufferEntries[BufferIndex]->d_off)
                {
                    Result = LXT_RESULT_FAILURE;
                    LxtLogInfo("Unexpected d_off %d != %d", Entry->d_off, BufferEntries[BufferIndex]->d_off);
                    goto ErrorExit;
                }
            }

            if (Pass == 0)
            {
                LxtCheckErrno(LxtGetdents64(DirFd, SingleEntry, sizeof(SingleEntry)));
                if (Result != 0)
                {
                    Result = LXT_RESULT_FAILURE;
                    LxtLogError("BytesRead Result ! 0");
                    goto ErrorExit;
                }
            }

            LxtClose(DirFd);
            DirFd = -1;
        }
    }

    //
    // Test alignment of getdents syscall.
    //

    LxtCheckErrno(DirFd = open(".", O_RDONLY | O_DIRECTORY));
    LxtLogInfo("Calling getdents with input buffer %p", &ByteAlignedDirents.Buffer);
    LxtCheckErrno(LxtGetdents64(DirFd, &ByteAlignedDirents.Buffer, sizeof(ByteAlignedDirents.Buffer)));

    LxtLogInfo("getdents test successful!");

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    if (Buffer != NULL)
    {
        free(Buffer);
    }

    if (DirFd != -1)
    {
        LxtClose(DirFd);
    }

    rmdir(LXT_GET_DENTS_FOLDER);
    return Result;
}
#endif

int FsCommonTestGetDents64Alignment(PLXT_ARGS Args)

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

    LxtCheckResult(LxtFsGetDentsAlignmentCommon(LXT_GET_DENTS_FOLDER, FS_TEST_GETDENTS64));

ErrorExit:
    return Result;
}

#ifdef __NR_getdents
int FsCommonTestGetDentsAlignment(PLXT_ARGS Args)

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

    LxtCheckResult(LxtFsGetDentsAlignmentCommon(LXT_GET_DENTS_FOLDER, 0));

ErrorExit:
    return Result;
}
#endif

int FsCommonTestGetDentsTypes(PLXT_ARGS Args)

/*++

Description:

    This routine tests whether all files are reported as the correct types by
    getdents.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    LXT_CHILD_INFO Children[] = {
        {"regchild", DT_REG},
        {"dirchild", DT_DIR},
        {"linkchild1", DT_LNK},
        {"linkchild2", DT_LNK},
        {"linkchild3", DT_LNK},
        {"dirchild", DT_DIR},
        {"fifochild", DT_FIFO},
        {"sockchild", DT_SOCK},
        {"chrchild", DT_CHR},
        {"blkchild", DT_BLK}};

    int Result;

    LxtCheckErrnoZeroSuccess(mkdir(LXT_GET_DENTS_FOLDER, 0777));
    LxtCheckErrnoZeroSuccess(mkdir(LXT_GET_DENTS_FOLDER "/dirchild", 0777));
    LxtCheckErrnoZeroSuccess(mknod(LXT_GET_DENTS_FOLDER "/regchild", S_IFREG | 0666, 0));
    LxtCheckErrnoZeroSuccess(mknod(LXT_GET_DENTS_FOLDER "/fifochild", S_IFIFO | 0666, 0));
    LxtCheckErrnoZeroSuccess(mknod(LXT_GET_DENTS_FOLDER "/sockchild", S_IFSOCK | 0666, 0));
    LxtCheckErrnoZeroSuccess(mknod(LXT_GET_DENTS_FOLDER "/chrchild", S_IFCHR | 0666, makedev(1, 3)));
    LxtCheckErrnoZeroSuccess(mknod(LXT_GET_DENTS_FOLDER "/blkchild", S_IFBLK | 0666, makedev(1, 1)));
    LxtCheckErrnoZeroSuccess(symlink("regchild", LXT_GET_DENTS_FOLDER "/linkchild1"));

    //
    // Directory symlinks and absolute symlinks may have different representations on DrvFs, so test them too.
    //

    LxtCheckErrnoZeroSuccess(symlink("dirchild", LXT_GET_DENTS_FOLDER "/linkchild2"));
    LxtCheckErrnoZeroSuccess(symlink("/proc", LXT_GET_DENTS_FOLDER "/linkchild3"));
    LxtCheckResult(LxtCheckDirectoryContentsEx(LXT_GET_DENTS_FOLDER, Children, LXT_COUNT_OF(Children), 0));

ErrorExit:
    unlink(LXT_GET_DENTS_FOLDER "/linkchild1");
    unlink(LXT_GET_DENTS_FOLDER "/linkchild2");
    unlink(LXT_GET_DENTS_FOLDER "/linkchild3");
    unlink(LXT_GET_DENTS_FOLDER "/fifochild");
    unlink(LXT_GET_DENTS_FOLDER "/sockchild");
    unlink(LXT_GET_DENTS_FOLDER "/chrchild");
    unlink(LXT_GET_DENTS_FOLDER "/blkchild");
    unlink(LXT_GET_DENTS_FOLDER "/regchild");
    rmdir(LXT_GET_DENTS_FOLDER "/dirchild");
    rmdir(LXT_GET_DENTS_FOLDER);
    return Result;
}

int FsCommonTestLinkAt(PLXT_ARGS Args)

/*++

Description:

    This routine tests the linkat system call.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int Fd;
    struct stat FileStat;
    int Result;
    int SourceDirFd;
    struct stat Stat;
    int SymlinkFd;
    struct stat SymlinkStat;
    int TargetDirFd;

    SourceDirFd = -1;
    TargetDirFd = -1;
    Fd = -1;
    SymlinkFd = -1;

    //
    // Set up the test files.
    //

    LxtCheckErrnoZeroSuccess(mkdir(FS_LINKAT_TEST_DIR, 0777));
    LxtCheckErrnoZeroSuccess(mkdir(FS_LINKAT_TEST_DIR2, 0777));
    LxtCheckErrno(Fd = creat(FS_LINKAT_TEST_DIR "/testfile", 0666));
    LxtCheckClose(Fd);
    LxtCheckErrnoZeroSuccess(symlink(FS_LINKAT_TEST_DIR "/testfile", FS_LINKAT_TEST_DIR "/testsymlink"));

    LxtCheckErrnoZeroSuccess(symlink(FS_LINKAT_TEST_DIR, FS_LINKAT_TEST_DIR "/testdirsymlink"));

    LxtCheckErrnoZeroSuccess(lstat(FS_LINKAT_TEST_DIR "/testfile", &FileStat));
    LxtCheckErrnoZeroSuccess(lstat(FS_LINKAT_TEST_DIR "/testsymlink", &SymlinkStat));

    //
    // Create a regular hard link.
    //

    LxtCheckErrno(SourceDirFd = open(FS_LINKAT_TEST_DIR, O_RDONLY | O_DIRECTORY));
    LxtCheckErrno(TargetDirFd = open(FS_LINKAT_TEST_DIR2, O_RDONLY | O_DIRECTORY));
    LxtCheckErrnoZeroSuccess(linkat(SourceDirFd, "testfile", TargetDirFd, "testlink", 0));

    LxtCheckErrnoZeroSuccess(lstat(FS_LINKAT_TEST_DIR2 "/testlink", &Stat));
    LxtCheckEqual(FileStat.st_ino, Stat.st_ino, "%lld");
    LxtCheckTrue(S_ISREG(Stat.st_mode));
    LxtCheckErrnoZeroSuccess(unlink(FS_LINKAT_TEST_DIR2 "/testlink"));

    //
    // Using AT_FDCWD.
    //

    LxtCheckErrnoZeroSuccess(chdir(FS_LINKAT_TEST_DIR));
    LxtCheckErrnoZeroSuccess(linkat(AT_FDCWD, "testfile", TargetDirFd, "testlink", 0));

    LxtCheckErrnoZeroSuccess(lstat(FS_LINKAT_TEST_DIR2 "/testlink", &Stat));
    LxtCheckEqual(FileStat.st_ino, Stat.st_ino, "%lld");
    LxtCheckTrue(S_ISREG(Stat.st_mode));
    LxtCheckErrnoZeroSuccess(unlink(FS_LINKAT_TEST_DIR2 "/testlink"));
    LxtCheckErrnoZeroSuccess(chdir(FS_LINKAT_TEST_DIR2));
    LxtCheckErrnoZeroSuccess(linkat(SourceDirFd, "testfile", AT_FDCWD, "testlink", 0));

    LxtCheckErrnoZeroSuccess(lstat(FS_LINKAT_TEST_DIR2 "/testlink", &Stat));
    LxtCheckEqual(FileStat.st_ino, Stat.st_ino, "%lld");
    LxtCheckTrue(S_ISREG(Stat.st_mode));
    LxtCheckErrnoZeroSuccess(unlink(FS_LINKAT_TEST_DIR2 "/testlink"));

    //
    // Symlinks are not followed by default.
    //

    LxtCheckErrnoZeroSuccess(linkat(SourceDirFd, "testsymlink", TargetDirFd, "testlink", 0));

    LxtCheckErrnoZeroSuccess(lstat(FS_LINKAT_TEST_DIR2 "/testlink", &Stat));
    LxtCheckEqual(SymlinkStat.st_ino, Stat.st_ino, "%lld");
    LxtCheckTrue(S_ISLNK(Stat.st_mode));
    LxtCheckErrnoZeroSuccess(unlink(FS_LINKAT_TEST_DIR2 "/testlink"));

    //
    // Use AT_SYMLINK_FOLLOW to follow the link.
    //

    LxtCheckErrnoZeroSuccess(linkat(SourceDirFd, "testsymlink", TargetDirFd, "testlink", AT_SYMLINK_FOLLOW));

    LxtCheckErrnoZeroSuccess(lstat(FS_LINKAT_TEST_DIR2 "/testlink", &Stat));
    LxtCheckEqual(FileStat.st_ino, Stat.st_ino, "%lld");
    LxtCheckTrue(S_ISREG(Stat.st_mode));
    LxtCheckErrnoZeroSuccess(unlink(FS_LINKAT_TEST_DIR2 "/testlink"));

    //
    // Fd must a directory, not a symlink to a directory.
    //

    LxtCheckErrno(SymlinkFd = open(FS_LINKAT_TEST_DIR "/testdirsymlink", O_NOFOLLOW | O_PATH));

    LxtCheckErrnoFailure(linkat(SymlinkFd, "testfile", TargetDirFd, "testlink", 0), ENOTDIR);

    LxtCheckErrnoFailure(linkat(SymlinkFd, "testfile", TargetDirFd, "testlink", AT_SYMLINK_FOLLOW), ENOTDIR);

    LxtCheckErrnoFailure(linkat(SourceDirFd, "testfile", SymlinkFd, "testlink", AT_SYMLINK_FOLLOW), ENOTDIR);

    LxtCheckClose(SymlinkFd);

    //
    // AT_EMPTY_PATH creates a link to the specified item.
    //

    LxtCheckErrno(Fd = open(FS_LINKAT_TEST_DIR "/testfile", O_RDONLY));
    LxtCheckErrnoZeroSuccess(linkat(Fd, "", TargetDirFd, "testlink", AT_EMPTY_PATH));

    LxtCheckErrnoZeroSuccess(lstat(FS_LINKAT_TEST_DIR2 "/testlink", &Stat));
    LxtCheckEqual(FileStat.st_ino, Stat.st_ino, "%lld");
    LxtCheckTrue(S_ISREG(Stat.st_mode));
    LxtCheckErrnoZeroSuccess(unlink(FS_LINKAT_TEST_DIR2 "/testlink"));
    LxtCheckClose(Fd);

    if (g_LxtFsInfo.FsType == LxtFsTypeVirtioFs)
    {
        LxtLogInfo("TODO: debug this test on virtiofs");
        Result = 0;
        goto ErrorExit;
    }

    //
    // If the fd is a symlink, it's not followed regardless of flags.
    //

    LxtCheckErrno(SymlinkFd = open(FS_LINKAT_TEST_DIR "/testsymlink", O_NOFOLLOW | O_PATH));

    LxtCheckErrnoZeroSuccess(linkat(SymlinkFd, "", TargetDirFd, "testlink", AT_EMPTY_PATH));

    LxtCheckErrnoZeroSuccess(lstat(FS_LINKAT_TEST_DIR2 "/testlink", &Stat));
    LxtCheckEqual(SymlinkStat.st_ino, Stat.st_ino, "%lld");
    LxtCheckTrue(S_ISLNK(Stat.st_mode));
    LxtCheckErrnoZeroSuccess(unlink(FS_LINKAT_TEST_DIR2 "/testlink"));
    LxtCheckErrnoZeroSuccess(linkat(SymlinkFd, "", TargetDirFd, "testlink", AT_EMPTY_PATH | AT_SYMLINK_FOLLOW));

    LxtCheckErrnoZeroSuccess(lstat(FS_LINKAT_TEST_DIR2 "/testlink", &Stat));
    LxtCheckEqual(SymlinkStat.st_ino, Stat.st_ino, "%lld");
    LxtCheckTrue(S_ISLNK(Stat.st_mode));
    LxtCheckErrnoZeroSuccess(unlink(FS_LINKAT_TEST_DIR2 "/testlink"));

    //
    // Directory FD should not work.
    //

    LxtCheckErrnoFailure(linkat(SourceDirFd, "", TargetDirFd, "testlink", AT_EMPTY_PATH), EPERM);

    //
    // AT_EMPTY_PATH only affects the source FD.
    //

    LxtCheckErrnoFailure(linkat(SourceDirFd, "testfile", Fd, "", AT_EMPTY_PATH), ENOENT);

    //
    // Create a link when the original link is removed.
    //
    // N.B. In WSL 1, the inode keeps a handle to the original link even after
    //      it has been removed, which is why this test is interesting.
    //

    LxtCheckErrnoZeroSuccess(linkat(SourceDirFd, "testfile", TargetDirFd, "testlink", 0));
    LxtCheckErrnoZeroSuccess(unlink(FS_LINKAT_TEST_DIR "/testfile"));
    LxtCheckErrnoZeroSuccess(linkat(TargetDirFd, "testlink", TargetDirFd, "testlink2", 0));
    LxtCheckErrnoZeroSuccess(unlink(FS_LINKAT_TEST_DIR2 "/testlink"));
    LxtCheckErrnoZeroSuccess(unlink(FS_LINKAT_TEST_DIR2 "/testlink2"));

    //
    // Same using the original fd.
    //
    // N.B. This test does not pass on WSL 2 DrvFs, because the 9p server stores
    //      a path in the fid and that path is no longer valid after the unlink.
    //

    if (g_LxtFsInfo.FsType != LxtFsTypePlan9)
    {
        LxtCheckErrno(Fd = creat(FS_LINKAT_TEST_DIR "/testfile", 0666));
        LxtCheckErrnoZeroSuccess(linkat(Fd, "", TargetDirFd, "testlink", AT_EMPTY_PATH));
        LxtCheckErrnoZeroSuccess(unlink(FS_LINKAT_TEST_DIR "/testfile"));
        LxtCheckErrnoZeroSuccess(linkat(Fd, "", TargetDirFd, "testlink2", AT_EMPTY_PATH));
        LxtCheckClose(Fd);
        LxtCheckErrnoZeroSuccess(unlink(FS_LINKAT_TEST_DIR2 "/testlink"));
        LxtCheckErrnoZeroSuccess(unlink(FS_LINKAT_TEST_DIR2 "/testlink2"));
    }

    //
    // You cannot resurrect a file with link count 0.
    //
    // N.B. On real Linux, the error code this produces is ENOENT, but since
    //      NTFS returns STATUS_ACCESS_DENIED for this, WSL gives EACCES
    //      instead. On WSL 2, 9p gives the Linux error code.
    //

    LxtCheckErrno(Fd = creat(FS_LINKAT_TEST_DIR "/testfile", 0666));
    LxtCheckErrnoZeroSuccess(unlink(FS_LINKAT_TEST_DIR "/testfile"));
    if (g_LxtFsInfo.FsType == LxtFsTypePlan9)
    {
        LxtCheckErrnoFailure(linkat(Fd, "", TargetDirFd, "testlink", AT_EMPTY_PATH), ENOENT);
    }
    else
    {
        LxtCheckErrnoFailure(linkat(Fd, "", TargetDirFd, "testlink", AT_EMPTY_PATH), EACCES);
    }

    LxtCheckClose(Fd);

ErrorExit:
    if (Fd >= 0)
    {
        close(Fd);
    }

    if (SourceDirFd >= 0)
    {
        close(SourceDirFd);
    }

    if (TargetDirFd >= 0)
    {
        close(TargetDirFd);
    }

    if (SymlinkFd >= 0)
    {
        close(SymlinkFd);
    }

    unlink(FS_LINKAT_TEST_DIR "/testdirsymlink");
    unlink(FS_LINKAT_TEST_DIR "/testsymlink");
    unlink(FS_LINKAT_TEST_DIR "/testfile");
    unlink(FS_LINKAT_TEST_DIR2 "/testlink");
    unlink(FS_LINKAT_TEST_DIR2 "/testlink2");
    rmdir(FS_LINKAT_TEST_DIR);
    rmdir(FS_LINKAT_TEST_DIR2);
    return Result;
}

int FsCommonTestOpen(PLXT_ARGS Args)

/*++

Description:

    This routine tests the open syscall.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int Result;

    //
    // Test that opening a directory with O_CREAT always fails with EISDIR.
    //

    LxtCheckErrnoFailure(open(FS_TEST_DIR_PARENT, O_RDONLY | O_CREAT), EISDIR);
    LxtCheckErrnoFailure(open(FS_TEST_DIR_PARENT, O_RDONLY | O_CREAT | O_EXCL), EEXIST);

ErrorExit:
    return Result;
}

int FsCommonTestOpenAt(PLXT_ARGS Args)

/*++
--*/

{

    int Result;
    int DirFd;
    int ChildFd1;
    int ChildFd2;
    const char DirPath[] = FS_TEST_DIR_PARENT "/test_openat";
    const char Child1[] = "newfile";
    const char Child1FullPath[] = FS_TEST_DIR_PARENT "/test_openat/newfile";
    const char* UnlinkName = NULL;
    const char* RmdirPath = NULL;

    //
    // Initialize locals.
    //

    DirFd = -1;
    ChildFd1 = -1;
    ChildFd2 = -1;
    UnlinkName = NULL;
    RmdirPath = NULL;

    //
    // Make a directory.
    //

    LxtLogInfo("Creating test directory folder %s", DirPath);

    Result = mkdir(DirPath, 0777);
    if (Result < 0)
    {
        LxtLogError("Could not create test directory:  %d", Result);
        goto cleanup;
    }

    LxtLogInfo("Created test directory folder!");
    RmdirPath = DirPath;

    //
    // Open the directory.
    //

    LxtLogInfo("Opening test directory folder %s", DirPath);

    Result = open(DirPath, O_RDONLY);
    if (Result < 0)
    {
        LxtLogError("Could not open test directory: %d", Result);
        goto cleanup;
    }

    DirFd = Result;
    LxtLogInfo("Opened test directory folder, fd = %d", DirFd);

    //
    // Open a child relative to the directory. This should fail.
    //

    LxtLogInfo("Opening child %s without create flag", Child1);

    Result = openat(DirFd, Child1, O_RDONLY);
    if (Result >= 0)
    {
        LxtLogError("Unexpectedly opened child: %d", Result);
        ChildFd1 = Result;
        Result = -1;
        goto cleanup;
    }

    //
    // Create child directory. This should succeed.
    //

    LxtLogInfo("Opening child %s with create flag", Child1);

    Result = openat(DirFd, Child1, O_RDONLY | O_CREAT, S_IRWXU);
    if (Result < 0)
    {
        LxtLogError("Failed to create child %d", Result);
        Result = -1;
        goto cleanup;
    }

    ChildFd1 = Result;
    LxtLogInfo("Created child, fd = %d", ChildFd1);

    UnlinkName = Child1FullPath;

    //
    // Open child directory using a full path. This should succeed.
    //

    LxtLogInfo("Opening child with full path %s", Child1FullPath);

    Result = open(Child1FullPath, O_RDONLY);
    if (Result < 0)
    {
        LxtLogError("Failed to open child full path %s: %d", Child1FullPath, Result);
        Result = -1;
        goto cleanup;
    }

    ChildFd2 = Result;
    Result = 0;

    LxtLogInfo("Opened child with full path, fd = %d", ChildFd2);

    LxtLogInfo("FsCommonTestOpenAt succeeded! Party in the USA!");

cleanup:

    if (ChildFd1 != -1)
    {
        close(ChildFd1);
    }

    if (ChildFd2 != -1)
    {
        close(ChildFd2);
    }

    if (DirFd != -1)
    {
        close(DirFd);
    }

    if (UnlinkName != NULL)
    {
        unlink(UnlinkName);
    }

    if (RmdirPath != NULL)
    {
        rmdir(RmdirPath);
    }

    return Result;
}

int FsCommonTestOpenCreateSymlink(PLXT_ARGS Args)

/*++

Description:

    This routine tests opening files through existing symlinks with O_CREAT.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    ssize_t BytesWritten;
    int Fd;
    const char* LinkPath = FS_TEST_DIR_PARENT "/test_opencreatelink";
    const char* Path = FS_TEST_DIR_PARENT "/test_opencreate";
    int Result;
    struct stat Stat;

    //
    // Create a test file and link.
    //

    LxtCheckErrno(Fd = creat(Path, 0777));
    LxtCheckErrnoZeroSuccess(close(Fd));
    Fd = -1;
    LxtCheckErrnoZeroSuccess(symlink(Path, LinkPath));

    //
    // Try to open the file through the link with O_CREAT and write some data.
    //

    LxtCheckErrno(Fd = open(LinkPath, O_RDWR | O_CREAT));
    LxtCheckErrno(BytesWritten = write(Fd, "test", 4));
    LxtCheckEqual(BytesWritten, 4, "%ld");
    LxtCheckErrnoZeroSuccess(close(Fd));
    Fd = -1;

    //
    // Check the file was written to.
    //

    LxtCheckErrnoZeroSuccess(stat(Path, &Stat));
    LxtCheckEqual(Stat.st_size, 4, "%ld");
    LxtCheckErrnoZeroSuccess(stat(LinkPath, &Stat));
    LxtCheckEqual(Stat.st_size, 4, "%ld");

    //
    // Point the link at /dev/null and try again.
    //

    LxtCheckErrnoZeroSuccess(unlink(LinkPath));
    LxtCheckErrnoZeroSuccess(symlink("/dev/null", LinkPath));
    LxtCheckErrno(Fd = open(LinkPath, O_RDWR | O_CREAT));
    LxtCheckErrno(BytesWritten = write(Fd, "test", 4));
    LxtCheckEqual(BytesWritten, 4, "%ld");
    LxtCheckErrnoZeroSuccess(close(Fd));
    Fd = -1;
    LxtCheckErrnoZeroSuccess(stat("/dev/null", &Stat));
    LxtCheckEqual(Stat.st_size, 0, "%ld");
    LxtCheckErrnoZeroSuccess(stat(LinkPath, &Stat));
    LxtCheckEqual(Stat.st_size, 0, "%ld");

ErrorExit:
    if (Fd >= 0)
    {
        close(Fd);
    }

    unlink(LinkPath);
    unlink(Path);

    return Result;
}

int FsCommonTestOpenCreateSymlinkDir(PLXT_ARGS Args)

/*++

Description:

    This routine tests creating a symlink to a directory.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    ssize_t BytesWritten;
    int Fd;
    const char* LinkDir = FS_TEST_DIR_PARENT "/test_dir_link";
    const char* Dir = FS_TEST_DIR_PARENT "/test_dir/";
    const char* LinkFile = FS_TEST_DIR_PARENT "/test_dir_link/test.txt";
    const char* File = FS_TEST_DIR_PARENT "/test_dir/test.txt";
    int Result;
    struct stat Stat;

    //
    // Create a new directory and a link to the directory.
    // Note that the link's target directory contains a trailing slash.
    //

    LxtCheckErrnoZeroSuccess(mkdir(Dir, 0777));
    LxtCheckErrnoZeroSuccess(symlink(Dir, LinkDir));

    //
    // Create a new file in the new directory (without using the
    // directory symlink), and write 4 bytes to the file.
    //

    LxtCheckErrno(Fd = creat(File, 0777));
    LxtCheckErrno(BytesWritten = write(Fd, "test", 4));
    LxtCheckEqual(BytesWritten, 4, "%ld");
    LxtCheckErrnoZeroSuccess(close(Fd));

    //
    // Check that the file was written to.
    //

    LxtCheckErrnoZeroSuccess(stat(File, &Stat));
    LxtCheckEqual(Stat.st_size, 4, "%ld");

    //
    // Check that accessing the file through the directory
    // symlink works properly.
    //

    LxtCheckErrnoZeroSuccess(stat(LinkFile, &Stat));
    LxtCheckEqual(Stat.st_size, 4, "%ld");

ErrorExit:
    if (Fd >= 0)
    {
        close(Fd);
    }

    unlink(LinkFile);
    unlink(File);
    unlink(LinkDir);
    rmdir(Dir);

    return Result;
}

int FsCommonTestCreateAndRename(PLXT_ARGS Args)

/*++
--*/

{

    int ChildPid;
    int ExpectedError;
    int FileDescriptor;
    int Result;
    const char Source[] = FS_TEST_DIR_PARENT "/fs_test.bin";
    const char SourceLink[] = FS_TEST_DIR_PARENT "/fs_test.bin.link";
    const char Target1[] = FS_TEST_DIR_PARENT "/test/fs_test.bin";
    const char Target2[] = FS_TEST_DIR_PARENT "/test/fs_test.bin.bak";
    const char TestPath[] = FS_TEST_DIR_PARENT "/test";
    const char TestPathError[] = FS_TEST_DIR_PARENT "/test/test";
    const char* UnlinkName = NULL;
    const char* RmdirName = NULL;

    //
    // Open the test file; this should fail.
    //

    FileDescriptor = open(Source, O_RDWR);
    if (FileDescriptor != -1)
    {
        Result = errno;
        LxtLogError("Found '%s' at the start; it should not exist!", Source);
        goto ErrorExit;
    }

    if (mkdir(TestPath, 0777) == 0)
    {
        RmdirName = TestPath;
    }

    //
    // Create the test file; this should succeed.
    //

    FileDescriptor = open(Source, O_RDWR | O_CREAT, S_IRWXU);
    if (FileDescriptor == -1)
    {
        Result = errno;
        LxtLogError("Could not create '%s', %d", Source, Result);
        goto ErrorExit;
    }

    UnlinkName = Source;
    close(FileDescriptor);
    FileDescriptor = -1;

    //
    // Create the test link.
    //

    LxtCheckErrno(symlink(Source, SourceLink));

    //
    // Rename the file and directory to itself.
    //

    LxtCheckErrno(rename(TestPath, TestPath));
    LxtCheckErrno(rename(Source, Source));
    LxtCheckErrno(rename(SourceLink, SourceLink));
    LxtCheckErrnoFailure(rename(TestPath, TestPathError), EINVAL);

    //
    // Various invalid renames (requires chroot).
    //

    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        const char* RelativePath = "foo";

        LxtCheckErrno(chdir(TestPath));
        LxtCheckErrno(chroot("."));
        mkdir(RelativePath, 0777);
        LxtCheckErrnoFailure(rename(RelativePath, "."), EBUSY);
        LxtCheckErrnoFailure(rename(RelativePath, ".."), EBUSY);
        LxtCheckErrnoFailure(rename(RelativePath, "/"), EBUSY);
        LxtCheckErrnoFailure(rename(RelativePath, "./"), EBUSY);
        LxtCheckErrnoFailure(rename(RelativePath, "../"), EBUSY);
        LxtCheckErrnoFailure(rename(RelativePath, "//"), EBUSY);
        LxtCheckErrnoFailure(rename(".", RelativePath), EBUSY);
        LxtCheckErrnoFailure(rename("..", RelativePath), EBUSY);
        LxtCheckErrnoFailure(rename("/", RelativePath), EBUSY);
        LxtCheckErrno(rmdir(RelativePath));
        exit(0);
    }

    LxtCheckResult(LxtWaitPidPoll(ChildPid, 0));

    //
    // Rename across directories.
    //

    Result = rename(Source, Target1);
    if (Result < 0)
    {
        Result = errno;
        LxtLogError("Could not rename '%s' to '%s', %d", Source, Target1, Result);
        goto ErrorExit;
    }

    UnlinkName = Target1;

    //
    // Rename within the same directory.
    //

    Result = rename(Target1, Target2);
    if (Result < 0)
    {
        Result = errno;
        LxtLogError("Could not rename '%s' to '%s', %d", Target1, Target2, Result);
        goto ErrorExit;
    }

    UnlinkName = Target2;

    //
    // Rename with an open file in the directory.
    //

    LxtCheckErrno(FileDescriptor = open(Target2, O_RDONLY));
    LxtCheckErrnoFailure(rename(TestPath, FS_TEST_DIR_PARENT "/test_fail"), EACCES);

    LxtCheckClose(FileDescriptor);

    //
    // The previous failed rename may have flushed directory entries, so try
    // another rename inside of the directory.
    //

    Result = rename(Target2, Target1);
    if (Result < 0)
    {
        Result = errno;
        LxtLogError("Could not rename '%s' to '%s', %d", Source, Target1, Result);
        goto ErrorExit;
    }

    UnlinkName = Target1;

    //
    // Unlink the final file.
    //

    Result = unlink(UnlinkName);
    if (Result < 0)
    {
        Result = errno;
        LxtLogError("Could not unlink '%s', %d", Target2, Result);
        goto ErrorExit;
    }

    UnlinkName = NULL;

    Result = LXT_RESULT_SUCCESS;

ErrorExit:

    if (FileDescriptor != -1)
    {
        close(FileDescriptor);
    }

    if (NULL != UnlinkName)
    {
        (void)unlink(UnlinkName);
    }

    if (NULL != RmdirName)
    {
        (void)rmdir(RmdirName);
    }

    remove(SourceLink);

    return Result;
}

int FsCommonTestCreateSymlinkTarget(PLXT_ARGS Args)

/*++

Description:

    This routine tests creating the target of a symlink through the symlink.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int Fd;
    int Result;
    const char* SymlinkPath = FS_TEST_DIR_PARENT "/fs_createsymlink";
    const char* SymlinkTarget = "fs_createsymlinktarget";
    const char* SymlinkTargetAbsolute = FS_TEST_DIR_PARENT "/fs_createsymlinktarget";

    Fd = -1;

    //
    // Create the symlink, and verify the target does not exist.
    //

    LxtCheckErrnoZeroSuccess(symlink(SymlinkTarget, SymlinkPath));
    LxtCheckErrnoFailure(open(SymlinkPath, O_RDONLY), ENOENT);
    LxtCheckErrnoFailure(open(SymlinkTargetAbsolute, O_RDONLY), ENOENT);
    LxtCheckErrno(Fd = open(SymlinkPath, O_PATH | O_NOFOLLOW));
    LxtCheckErrnoZeroSuccess(close(Fd));
    Fd = -1;

    //
    // Using O_EXCL will fail even if the target does not exist.
    //

    LxtCheckErrnoFailure(Fd = open(SymlinkPath, O_CREAT | O_EXCL), EEXIST);

    //
    // Using O_NOFOLLOW will fail as usual.
    //

    LxtCheckErrnoFailure(Fd = open(SymlinkPath, O_CREAT | O_NOFOLLOW), ELOOP);
    LxtCheckErrnoFailure(open(SymlinkPath, O_RDONLY), ENOENT);
    LxtCheckErrnoFailure(open(SymlinkTargetAbsolute, O_RDONLY), ENOENT);

    //
    // Create the target through the symlink, and check it got created.
    //

    LxtCheckErrno(Fd = open(SymlinkPath, O_CREAT));
    LxtCheckErrnoZeroSuccess(close(Fd));
    Fd = -1;
    LxtCheckErrnoZeroSuccess(access(SymlinkTargetAbsolute, F_OK));

ErrorExit:
    if (Fd >= 0)
    {
        close(Fd);
    }

    unlink(SymlinkTargetAbsolute);
    unlink(SymlinkPath);
    return Result;
}

int FsCommonTestReadlinkat(PLXT_ARGS Args)

/*++

Description:

    This routine tests the readlinkat function.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    char Buffer[PATH_MAX];
    ssize_t BytesRead;
    int Fd;
    int Result;

    //
    // Create a symlink to test.
    //

    LxtCheckErrno(Fd = creat(FS_READLINK_TEST_FILE, 0666));
    LxtCheckErrnoZeroSuccess(close(Fd));
    Fd = -1;
    LxtCheckErrnoZeroSuccess(symlink(FS_READLINK_TEST_FILE, FS_READLINK_TEST_LINK));

    //
    // Test the ability to operate directly on a symlink.
    //

    LxtCheckErrno(Fd = open(FS_READLINK_TEST_LINK, O_PATH | O_NOFOLLOW));
    memset(Buffer, 0, sizeof(Buffer));
    LxtCheckErrno(BytesRead = readlinkat(Fd, "", Buffer, sizeof(Buffer)));
    LxtCheckEqual(BytesRead, strlen(FS_READLINK_TEST_FILE), "%ld");
    LxtCheckStringEqual(Buffer, FS_READLINK_TEST_FILE);

    //
    // Path specified with symlink file descriptor.
    //

    LxtCheckErrnoFailure(readlinkat(Fd, "foo", Buffer, sizeof(Buffer)), ENOTDIR);
    LxtCheckErrnoZeroSuccess(close(Fd));
    Fd = -1;

    //
    // Empty path with non-symlink file descriptor.
    //

    LxtCheckErrno(Fd = open(FS_READLINK_TEST_FILE, O_RDONLY));
    LxtCheckErrnoFailure(readlinkat(Fd, "", Buffer, sizeof(Buffer)), ENOENT);

ErrorExit:
    if (Fd >= 0)
    {
        close(Fd);
    }

    unlink(FS_READLINK_TEST_LINK);
    unlink(FS_READLINK_TEST_FILE);
    return Result;
}

int FsCommonTestRemoveSelfOrParent(PLXT_ARGS Args)

/*++

Description:

    This routine tests some corner cases of the rmdir function.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    pid_t ChildPid;
    int Fd;
    int Result;

    ChildPid = -1;
    Fd = -1;
    LxtCheckErrnoZeroSuccess(mkdir(FS_RMDIR_TEST_DIR, 0777));
    LxtCheckErrnoZeroSuccess(mkdir(FS_RMDIR_TEST_DIR "/test", 0777));

    //
    // Test relative to current working directory.
    //

    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        LxtCheckErrnoZeroSuccess(chdir(FS_RMDIR_TEST_DIR "/test"));

        //
        // Can't remove . or ..
        //

        LxtCheckErrnoFailure(rmdir(".."), ENOTEMPTY);
        LxtCheckErrnoFailure(unlink(".."), EISDIR);
        LxtCheckErrnoFailure(rmdir("."), EINVAL);
        LxtCheckErrnoFailure(unlink("."), EISDIR);

        //
        // Even when the directory is empty, rmdir("..") says ENOTEMPTY.
        //
        // N.B. On Plan9, using the current working directory after deleting it
        //      does not work, even if just to say "..".
        //

        LxtCheckErrnoZeroSuccess(rmdir(FS_RMDIR_TEST_DIR "/test"));
        if (g_LxtFsInfo.FsType != LxtFsTypePlan9)
        {
            LxtCheckErrnoFailure(rmdir(".."), ENOTEMPTY);
        }

        LxtCheckErrnoZeroSuccess(rmdir(FS_RMDIR_TEST_DIR));

        //
        // Root path.
        //
        // N.B. Cannot chroot to a deleted working directory on plan 9.
        //

        if (g_LxtFsInfo.FsType != LxtFsTypePlan9)
        {
            LxtCheckErrno(chroot("."));
        }

        LxtCheckErrnoFailure(rmdir("/"), EBUSY);
        LxtCheckErrnoFailure(unlink("/"), EISDIR);
        LxtCheckErrnoFailure(rmdir("//"), EBUSY);
        LxtCheckErrnoFailure(unlink("//"), EISDIR);
        LxtCheckErrnoFailure(rmdir("/."), EINVAL);
        LxtCheckErrnoFailure(unlink("/."), EISDIR);
        LxtCheckErrnoFailure(rmdir("/.."), ENOTEMPTY);
        LxtCheckErrnoFailure(unlink("/.."), EISDIR);

        exit(0);
    }

    LxtCheckResult(LxtWaitPidPoll(ChildPid, 0));
    LxtCheckErrnoZeroSuccess(mkdir(FS_RMDIR_TEST_DIR, 0777));
    LxtCheckErrnoZeroSuccess(mkdir(FS_RMDIR_TEST_DIR "/test", 0777));

    //
    // Same tests, with unlinkat.
    //

    LxtCheckErrno(Fd = open(FS_RMDIR_TEST_DIR "/test", O_DIRECTORY));
    LxtCheckErrnoFailure(unlinkat(Fd, "..", AT_REMOVEDIR), ENOTEMPTY);
    LxtCheckErrnoFailure(unlinkat(Fd, "..", 0), EISDIR);
    LxtCheckErrnoFailure(unlinkat(Fd, ".", AT_REMOVEDIR), EINVAL);
    LxtCheckErrnoFailure(unlinkat(Fd, ".", 0), EISDIR);
    LxtCheckClose(Fd);

    //
    // Full paths ending in . or ..
    //

    LxtCheckErrnoFailure(rmdir(FS_RMDIR_TEST_DIR "/test/.."), ENOTEMPTY);
    LxtCheckErrnoFailure(unlink(FS_RMDIR_TEST_DIR "/test/.."), EISDIR);
    LxtCheckErrnoFailure(rmdir(FS_RMDIR_TEST_DIR "/test/."), EINVAL);
    LxtCheckErrnoFailure(unlink(FS_RMDIR_TEST_DIR "/test/."), EISDIR);

    //
    // Nonexistent paths.
    //

    LxtCheckErrnoFailure(rmdir(FS_RMDIR_TEST_DIR "/test2/.."), ENOENT);
    LxtCheckErrnoFailure(rmdir(FS_RMDIR_TEST_DIR "/test2/."), ENOENT);
    LxtCheckErrnoFailure(unlink(FS_RMDIR_TEST_DIR "/test2/.."), ENOENT);
    LxtCheckErrnoFailure(unlink(FS_RMDIR_TEST_DIR "/test2/."), ENOENT);

    //
    // Having a . anywhere but the last component does work.
    //

    LxtCheckErrnoZeroSuccess(rmdir(FS_RMDIR_TEST_DIR "/./test"));
    LxtCheckErrnoFailure(access(FS_RMDIR_TEST_DIR "/test", F_OK), ENOENT);

ErrorExit:
    if (Fd >= 0)
    {
        close(Fd);
    }

    if (ChildPid == 0)
    {
        exit(LXT_RESULT_FAILURE);
    }

    rmdir(FS_RMDIR_TEST_DIR "/test");
    rmdir(FS_RMDIR_TEST_DIR);
    return Result;
}

int FsCommonTestRenameAt(PLXT_ARGS Args)

/*++

Description:

    This routine tests the renameat system call on volfs.

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

    LxtCheckResult(LxtFsCreateTestDir((FS_RENAMEAT_TEST_DIR)));
    LxtCheckResult(LxtFsCreateTestDir((FS_RENAMEAT_TEST_DIR "/a")));
    LxtCheckResult(LxtFsCreateTestDir((FS_RENAMEAT_TEST_DIR "/a/b")));
    LxtCheckResult(LxtFsCreateTestDir((FS_RENAMEAT_TEST_DIR "/a/b/c")));
    LxtCheckResult(LxtFsCreateTestDir((FS_RENAMEAT_TEST_DIR "/a/b/c/d")));
    LxtCheckResult(LxtFsCreateTestDir((FS_RENAMEAT_TEST_DIR "/a/b/c/d/e")));
    LxtCheckResult(LxtFsCreateTestDir((FS_RENAMEAT_TEST_DIR "/a/b/c/d/e/f")));

    LxtCheckErrno(DirFd1 = open(FS_RENAMEAT_TEST_DIR "/a", O_DIRECTORY));
    LxtCheckErrno(DirFd2 = open(FS_RENAMEAT_TEST_DIR "/a/b/c", O_DIRECTORY));

    LxtCheckErrnoZeroSuccess(chdir(FS_RENAMEAT_TEST_DIR));

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

    rmdir(FS_RENAMEAT_TEST_DIR "/a/b/c/d/e/f");
    rmdir(FS_RENAMEAT_TEST_DIR "/a/b/c/d/e");
    rmdir(FS_RENAMEAT_TEST_DIR "/a/b/c/d");
    rmdir(FS_RENAMEAT_TEST_DIR "/a/b/c");
    rmdir(FS_RENAMEAT_TEST_DIR "/a/b");
    rmdir(FS_RENAMEAT_TEST_DIR "/a");
    rmdir(FS_RENAMEAT_TEST_DIR);

    return Result;
}

int FsCommonTestRenameDir(PLXT_ARGS Args)

/*++

Description:

    This routine tests the rename system call for LxFs directories.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int Result;

    LxtCheckErrno(LxtFsRenameDirCommon(FS_TEST_DIR_PARENT));

ErrorExit:
    return Result;
}

int FsCommonTestSetEofCheckTimeLessThan(struct timespec* X, struct timespec* Y)

{

    int Result;

    Result = LXT_RESULT_FAILURE;
    if (X->tv_sec > Y->tv_sec)
    {
        LxtLogError("Unexpected seconds");
        goto ErrorExit;
    }

    if ((X->tv_sec == Y->tv_sec) && (X->tv_nsec >= Y->tv_nsec))
    {
        LxtLogError("Unexpected nano seconds");
        goto ErrorExit;
    }

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    return Result;
}

int FsCommonTestSetEof(PLXT_ARGS Args)

/*++
--*/

{

    int Result = LXT_RESULT_FAILURE;
    int fd;
    const char TestFileName[] = FS_TEST_DIR_PARENT "/fs_test.bin";
    struct stat statbuf;
    struct stat statbuf2;

    //
    // Create the test file.
    //

    LxtCheckErrno(fd = open(TestFileName, O_RDWR | O_CREAT, S_IRWXU));
    LxtCheckErrno(ftruncate(fd, 54321));
    LxtCheckErrno(stat(TestFileName, &statbuf));
    LxtCheckErrnoFailure(stat(FS_TEST_DIR_PARENT "/*", &statbuf), ENOENT);
    LxtCheckErrnoFailure(stat(FS_TEST_DIR_PARENT "/*.bin", &statbuf), ENOENT);
    LxtCheckErrno(fstat(fd, &statbuf));
    if (54321 != statbuf.st_size)
    {
        Result = LXT_RESULT_FAILURE;
        LxtLogError("size mismatch after ftruncate64.");
        goto ErrorExit;
    }

    LxtCheckErrno(ftruncate(fd, 12345));
    if (Result < 0)
    {
        Result = errno;
        LxtLogError("ftruncate('%fs') failed, %d", TestFileName, Result);
        goto ErrorExit;
    }

    LxtCheckErrno(fstat(fd, &statbuf));
    if (12345 != statbuf.st_size)
    {
        Result = LXT_RESULT_FAILURE;
        LxtLogError("size mismatch after ftruncate.");
        goto ErrorExit;
    }

    //
    // Check that setting the eof does change the file times even if there
    // was no change.
    //

    LxtCheckErrno(ftruncate(fd, 0));
    if (Result < 0)
    {
        Result = errno;
        LxtLogError("ftruncate('%fs') failed, %d", TestFileName, Result);
        goto ErrorExit;
    }

    LxtCheckErrno(fstat(fd, &statbuf));
    if (0 != statbuf.st_size)
    {
        Result = LXT_RESULT_FAILURE;
        LxtLogError("size mismatch after ftruncate.");
        goto ErrorExit;
    }

    usleep(500000);
    LxtCheckErrno(ftruncate(fd, 0));
    if (Result < 0)
    {
        Result = errno;
        LxtLogError("ftruncate('%fs') failed, %d", TestFileName, Result);
        goto ErrorExit;
    }

    LxtCheckErrno(fstat(fd, &statbuf2));
    if (0 != statbuf.st_size)
    {
        Result = LXT_RESULT_FAILURE;
        LxtLogError("size mismatch after ftruncate.");
        goto ErrorExit;
    }

    //
    // NTFS updates the atime when other timestamps are updated, even when
    // access time is otherwise disabled.
    //

    if (g_LxtFsInfo.Flags.DrvFsBehavior == 0)
    {
        LxtCheckEqual(statbuf.st_atim.tv_sec, statbuf2.st_atim.tv_sec, "%d");
        LxtCheckEqual(statbuf.st_atim.tv_nsec, statbuf2.st_atim.tv_nsec, "%d");
    }

    if (FS_IS_PLAN9_CACHED() == FALSE)
    {
        LxtCheckResult(FsCommonTestSetEofCheckTimeLessThan(&statbuf.st_mtim, &statbuf2.st_mtim));
        LxtCheckResult(FsCommonTestSetEofCheckTimeLessThan(&statbuf.st_ctim, &statbuf2.st_ctim));
    }

    close(fd);
    fd = -1;

    usleep(500000);
    LxtCheckErrno(fd = open(TestFileName, O_RDWR | O_TRUNC, S_IRWXU));
    LxtCheckErrno(fstat(fd, &statbuf));
    if (0 != statbuf.st_size)
    {
        Result = LXT_RESULT_FAILURE;
        LxtLogError("size mismatch after ftruncate.");
        goto ErrorExit;
    }

    if (g_LxtFsInfo.Flags.DrvFsBehavior == 0)
    {
        LxtCheckEqual(statbuf.st_atim.tv_sec, statbuf2.st_atim.tv_sec, "%d");
        LxtCheckEqual(statbuf.st_atim.tv_nsec, statbuf2.st_atim.tv_nsec, "%d");
    }

    if (FS_IS_PLAN9_CACHED() == FALSE)
    {
        LxtCheckResult(FsCommonTestSetEofCheckTimeLessThan(&statbuf2.st_mtim, &statbuf.st_mtim));
        LxtCheckResult(FsCommonTestSetEofCheckTimeLessThan(&statbuf2.st_ctim, &statbuf.st_ctim));
    }

    close(fd);
    fd = -1;

    LxtCheckErrno(unlink(TestFileName));

    Result = LXT_RESULT_SUCCESS;

ErrorExit:

    if (-1 != fd)
    {
        close(fd);
        (void)unlink(TestFileName);
    }

    return Result;
}

int FsCommonTestTrailingSlash(PLXT_ARGS Args)

/*++

Description:

    This routine tests the behavior of open with trailing slashes.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int Fd;
    int Result;
    struct stat Stat;

    Fd = -1;

    //
    // Nonexistent file tests.
    //

    LxtCheckErrnoFailure(creat(FS_TRAILING_TEST_FILE "/", 0666), EISDIR);
    LxtCheckErrnoFailure(creat(FS_TRAILING_TEST_FILE "/foo/", 0666), ENOENT);
    LxtCheckErrnoFailure(open(FS_TRAILING_TEST_FILE "/", O_RDONLY), ENOENT);
    LxtCheckErrnoFailure(stat(FS_TRAILING_TEST_FILE "/", &Stat), ENOENT);

    //
    // Create a directory.
    //

    LxtCheckErrnoZeroSuccess(mkdir(FS_TRAILING_TEST_DIR "/", 0777));
    LxtCheckErrno(Fd = open(FS_TRAILING_TEST_DIR "/", O_RDONLY | O_DIRECTORY));
    LxtCheckErrnoZeroSuccess(close(Fd));
    Fd = -1;
    LxtCheckErrno(Fd = open(FS_TRAILING_TEST_DIR "//", O_RDONLY | O_DIRECTORY));
    LxtCheckErrnoZeroSuccess(close(Fd));
    Fd = -1;
    LxtCheckErrnoFailure(open(FS_TRAILING_TEST_DIR "/", O_RDONLY | O_CREAT), EISDIR);

    //
    // Create a symlink to a directory.
    //

    LxtCheckErrnoFailure(symlink(FS_TRAILING_TEST_DIR, FS_TRAILING_TEST_LINK "/"), ENOENT);

    LxtCheckErrnoZeroSuccess(symlink(FS_TRAILING_TEST_DIR, FS_TRAILING_TEST_LINK));

    LxtCheckErrnoFailure(symlink(FS_TRAILING_TEST_DIR, FS_TRAILING_TEST_LINK "/"), EEXIST);

    //
    // Test the symlink with and without trailing slash.
    //

    LxtCheckErrnoZeroSuccess(lstat(FS_TRAILING_TEST_LINK "/", &Stat));
    LxtCheckTrue(S_ISDIR(Stat.st_mode));
    LxtCheckErrnoZeroSuccess(lstat(FS_TRAILING_TEST_LINK "//", &Stat));
    LxtCheckTrue(S_ISDIR(Stat.st_mode));
    LxtCheckErrnoZeroSuccess(lstat(FS_TRAILING_TEST_LINK, &Stat));
    LxtCheckTrue(S_ISLNK(Stat.st_mode));
    LxtCheckErrnoFailure(open(FS_TRAILING_TEST_LINK, O_RDONLY | O_NOFOLLOW), ELOOP);

    LxtCheckErrno(Fd = open(FS_TRAILING_TEST_LINK "/", O_RDONLY | O_NOFOLLOW | O_DIRECTORY));

    LxtCheckErrnoZeroSuccess(close(Fd));
    Fd = -1;

    //
    // Create a file and test using it with a trailing slash.
    //

    LxtCheckErrno(Fd = creat(FS_TRAILING_TEST_FILE, 0666));
    LxtCheckErrnoZeroSuccess(close(Fd));
    Fd = -1;
    LxtCheckErrnoFailure(open(FS_TRAILING_TEST_FILE "/", O_RDONLY), ENOTDIR);
    LxtCheckErrnoFailure(open(FS_TRAILING_TEST_FILE "//", O_RDONLY), ENOTDIR);
    LxtCheckErrnoFailure(stat(FS_TRAILING_TEST_FILE "/", &Stat), ENOTDIR);
    LxtCheckErrnoFailure(stat(FS_TRAILING_TEST_FILE "//", &Stat), ENOTDIR);
    LxtCheckErrnoFailure(open(FS_TRAILING_TEST_FILE "/", O_RDONLY | O_CREAT), EISDIR);

    //
    // Create a symlink to a file and test using with a trailing slash.
    //

    LxtCheckErrnoZeroSuccess(unlink(FS_TRAILING_TEST_LINK));
    LxtCheckErrnoZeroSuccess(symlink(FS_TRAILING_TEST_FILE, FS_TRAILING_TEST_LINK));

    LxtCheckErrnoFailure(open(FS_TRAILING_TEST_LINK "/", O_RDONLY), ENOTDIR);
    LxtCheckErrnoFailure(open(FS_TRAILING_TEST_LINK "/", O_RDONLY | O_NOFOLLOW), ENOTDIR);

    LxtCheckErrnoFailure(stat(FS_TRAILING_TEST_LINK "/", &Stat), ENOTDIR);
    LxtCheckErrnoFailure(lstat(FS_TRAILING_TEST_LINK "/", &Stat), ENOTDIR);

    //
    // Create a symlink where the target has a trailing slash.
    //

    LxtCheckErrnoZeroSuccess(unlink(FS_TRAILING_TEST_LINK));
    LxtCheckErrnoZeroSuccess(symlink(FS_TRAILING_TEST_FILE "/", FS_TRAILING_TEST_LINK));

    LxtCheckErrnoFailure(open(FS_TRAILING_TEST_LINK, O_RDONLY), ENOTDIR);
    LxtCheckErrnoFailure(open(FS_TRAILING_TEST_LINK, O_RDONLY | O_CREAT), EISDIR);

    LxtCheckErrnoFailure(stat(FS_TRAILING_TEST_LINK, &Stat), ENOTDIR);

    //
    // Mkdir over an existing file.
    //

    LxtCheckErrnoFailure(mkdir(FS_TRAILING_TEST_FILE "/", 0777), EEXIST);

    //
    // Unlink/rmdir
    //

    LxtCheckErrnoFailure(unlink(FS_TRAILING_TEST_FILE "/"), ENOTDIR);
    LxtCheckErrnoFailure(unlink(FS_TRAILING_TEST_DIR "/"), EISDIR);
    LxtCheckErrnoZeroSuccess(rmdir(FS_TRAILING_TEST_DIR "/"));
    LxtCheckErrnoFailure(rmdir(FS_TRAILING_TEST_FILE "/"), ENOTDIR);

    //
    // Test a symlink to a nonexistent target.
    //

    LxtCheckErrnoZeroSuccess(unlink(FS_TRAILING_TEST_LINK));
    LxtCheckErrnoZeroSuccess(symlink(FS_TRAILING_TEST_FILE, FS_TRAILING_TEST_LINK));

    LxtCheckErrnoZeroSuccess(unlink(FS_TRAILING_TEST_FILE));
    LxtCheckErrnoFailure(open(FS_TRAILING_TEST_LINK "/", O_RDONLY), ENOENT);
    LxtCheckErrnoFailure(open(FS_TRAILING_TEST_LINK "/", O_RDONLY | O_NOFOLLOW), ENOENT);

    LxtCheckErrnoFailure(open(FS_TRAILING_TEST_LINK "/", O_RDONLY | O_CREAT), EISDIR);

    LxtCheckErrnoFailure(stat(FS_TRAILING_TEST_LINK "/", &Stat), ENOENT);
    LxtCheckErrnoFailure(lstat(FS_TRAILING_TEST_LINK "/", &Stat), ENOENT);

    //
    // Symlink to a nonexistent target with trailing slash.
    //

    LxtCheckErrnoZeroSuccess(unlink(FS_TRAILING_TEST_LINK));
    LxtCheckErrnoZeroSuccess(symlink(FS_TRAILING_TEST_FILE "/", FS_TRAILING_TEST_LINK));

    LxtCheckErrnoFailure(open(FS_TRAILING_TEST_LINK, O_RDONLY), ENOENT);
    LxtCheckErrnoFailure(open(FS_TRAILING_TEST_LINK, O_RDONLY | O_CREAT), EISDIR);

    LxtCheckErrnoFailure(stat(FS_TRAILING_TEST_LINK, &Stat), ENOENT);

    //
    // Other creation functions.
    //

    LxtCheckErrnoFailure(link(FS_TRAILING_TEST_LINK, FS_TRAILING_TEST_FILE "/"), ENOENT);

    LxtCheckErrnoFailure(mknod(FS_TRAILING_TEST_FILE "/", S_IFIFO | 0666, 0), ENOENT);

    LxtCheckErrnoZeroSuccess(mkdir(FS_TRAILING_TEST_DIR, 0777));
    LxtCheckErrnoFailure(link(FS_TRAILING_TEST_LINK, FS_TRAILING_TEST_DIR "/"), EEXIST);

    LxtCheckErrnoFailure(mknod(FS_TRAILING_TEST_DIR "/", S_IFIFO | 0666, 0), EEXIST);

ErrorExit:
    if (Fd >= 0)
    {
        close(Fd);
    }

    unlink(FS_TRAILING_TEST_FILE);
    unlink(FS_TRAILING_TEST_LINK);
    rmdir(FS_TRAILING_TEST_DIR);
    return Result;
}

int FsCommonTestMkdir(PLXT_ARGS Args)

/*++
--*/

{

    int Fd;
    const char* ParentDirName = FS_TEST_DIR_PARENT;
    const char* TestDirName = FS_TEST_DIR_PARENT "/test_dir";
    const char* TestSubDirName = FS_TEST_DIR_PARENT "/test_dir/foo";
    const char* RelativeDirName = "test_dir";
    const char* RelativeDotSlashDirName = "./test_dir/";
    const char* RelativeSubDirName = "test_dir/foo";
    int Result;
    struct stat Stat;

    //
    // Ensure the dir doesn't exist.
    //

    unlink(TestDirName);
    rmdir(TestSubDirName);
    rmdir(TestDirName);

    //
    // Create the subdir while the parent doesn't exist.
    //

    LxtCheckErrnoFailure(mkdir(TestSubDirName, 0777), ENOENT);

    //
    // Create the test dir as a file.
    //

    LxtCheckErrno(Fd = open(TestDirName, O_RDWR | O_CREAT, S_IRWXU));

    //
    // Verify the file size is 0.
    //

    LxtCheckErrno(fstat(Fd, &Stat));
    LxtCheckEqual(Stat.st_size, 0, "%ld");
    LxtCheckErrnoZeroSuccess(close(Fd));

    //
    // Try to create a dir with this name, and a dir under that name.
    //

    LxtCheckErrnoFailure(mkdir(TestDirName, 0777), EEXIST);
    LxtCheckErrnoFailure(mkdir(TestSubDirName, 0777), ENOTDIR);
    LxtCheckErrnoZeroSuccess(unlink(TestDirName));

    //
    // Create a dir with no collisions expected.
    //

    LxtCheckErrnoZeroSuccess(mkdir(TestDirName, 0777));

    //
    // Verify the directory file size is equal to the file-system block-size.
    //
    // N.B. Plan 9 in cached mode doesn't return the block size reported by the
    //      server.
    //

    LxtCheckErrno(stat(TestDirName, &Stat));
    if (FS_IS_PLAN9_CACHED() == FALSE)
    {
        LxtCheckEqual(Stat.st_size, Stat.st_blksize, "%ld");
    }

    //
    // Test a directory name collision.
    //

    LxtCheckErrnoFailure(mkdir(TestDirName, 0777), EEXIST);

    //
    // Test the rmdir.
    //

    LxtCheckErrnoZeroSuccess(rmdir(TestDirName));
    LxtCheckErrnoFailure(rmdir(TestDirName), ENOENT);

    //
    // Test mkdir with a relative path. Change the working directory first
    // since it's normally / for tests which is not interesting.
    //

    LxtCheckErrnoZeroSuccess(chdir(ParentDirName));
    LxtCheckErrnoFailure(mkdir(RelativeSubDirName, 0777), ENOENT);
    LxtCheckErrnoZeroSuccess(mkdir(RelativeDirName, 0777));
    LxtCheckErrnoFailure(mkdir(RelativeDirName, 0777), EEXIST);
    LxtCheckErrnoFailure(mkdir(TestDirName, 0777), EEXIST);
    LxtCheckErrnoZeroSuccess(mkdir(RelativeSubDirName, 0777));
    LxtCheckErrnoFailure(mkdir(RelativeSubDirName, 0777), EEXIST);
    LxtCheckErrnoFailure(mkdir(TestSubDirName, 0777), EEXIST);
    LxtCheckErrnoZeroSuccess(rmdir(TestSubDirName));
    LxtCheckErrnoZeroSuccess(rmdir(TestDirName));

    //
    // Relative path starting with "./" and ending in "/".
    //

    LxtCheckErrnoZeroSuccess(mkdir(RelativeDotSlashDirName, 0777));
    LxtCheckErrnoFailure(mkdir(RelativeDotSlashDirName, 0777), EEXIST);
    LxtCheckErrnoFailure(mkdir(TestDirName, 0777), EEXIST);

    //
    // Empty path should return ENOENT
    //

    LxtCheckErrnoFailure(mkdir("", 0777), ENOENT);

    //
    // Special path edge cases.
    //

    LxtCheckErrnoFailure(mkdir(".", 0777), EEXIST);
    LxtCheckErrnoFailure(mkdir("..", 0777), EEXIST);
    LxtCheckErrnoFailure(mkdir("/", 0777), EEXIST);
    LxtCheckErrnoFailure(mkdir("/.", 0777), EEXIST);
    LxtCheckErrnoFailure(mkdir("/..", 0777), EEXIST);
    LxtCheckErrnoFailure(mkdir("/data/", 0777), EEXIST);
    LxtCheckErrnoFailure(mkdir("/data/.", 0777), EEXIST);
    LxtCheckErrnoFailure(mkdir("/data/..", 0777), EEXIST);

    //
    // Variation succeeded.
    //

ErrorExit:
    unlink(TestDirName);
    rmdir(TestSubDirName);
    rmdir(TestDirName);
    rmdir(RelativeDirName);

    //
    // Restore working directory (other tests depend on it)
    //

    chdir("/");
    return Result;
}

int FsCommonTestMkDirAt(PLXT_ARGS Args)

/*++

Routine Description:

This routine tests the mkdirat system call.

Arguments:

Args - Supplies the command line arguments.

Return Value:

0 on success, -1 on failure.

--*/

{

    int Fd;
    const char* ParentDirName = FS_TEST_DIR_PARENT;
    int ParentFd;
    const char* TestDirName = FS_TEST_DIR_PARENT "/test_dir";
    const char* TestSubDirName = FS_TEST_DIR_PARENT "/test_dir/foo";
    const char* RelativeDirName = "test_dir";
    const char* RelativeDotSlashDirName = "./test_dir/";
    const char* RelativeSubDirName = "test_dir/foo";
    int Result;

    Fd = -1;

    //
    // Ensure the dir doesn't exist.
    //

    unlink(TestDirName);
    rmdir(TestSubDirName);
    rmdir(TestDirName);

    //
    // Open the parent.
    //

    LxtCheckErrno(ParentFd = open(ParentDirName, O_DIRECTORY));

    //
    // Create the subdir while the parent doesn't exist.
    //

    LxtCheckErrnoFailure(mkdirat(ParentFd, RelativeSubDirName, 0777), ENOENT);

    //
    // Create the test dir as a file.
    //

    LxtCheckErrno(Fd = open(TestDirName, O_RDWR | O_CREAT, S_IRWXU));

    //
    // Try using the file fd as the parent.
    //

    LxtCheckErrnoFailure(mkdirat(Fd, RelativeDirName, 0777), ENOTDIR);
    LxtCheckErrnoZeroSuccess(close(Fd));

    //
    // Try to create a dir with this name, and a dir under that name.
    //

    LxtCheckErrnoFailure(mkdirat(ParentFd, RelativeDirName, 0777), EEXIST);
    LxtCheckErrnoFailure(mkdirat(ParentFd, RelativeSubDirName, 0777), ENOTDIR);
    LxtCheckErrnoZeroSuccess(unlink(TestDirName));

    //
    // Create a dir with no collisions expected.
    //

    LxtCheckErrnoZeroSuccess(mkdirat(ParentFd, RelativeDirName, 0777));

    //
    // Test a directory name collision.
    //

    LxtCheckErrnoFailure(mkdirat(ParentFd, RelativeDirName, 0777), EEXIST);
    LxtCheckErrnoFailure(mkdir(TestDirName, 0777), EEXIST);

    //
    // Test the rmdir.
    //

    LxtCheckErrnoZeroSuccess(rmdir(TestDirName));
    LxtCheckErrnoFailure(rmdir(TestDirName), ENOENT);

    //
    // Relative path starting with "./" and ending in "/".
    //

    LxtCheckErrnoZeroSuccess(mkdirat(ParentFd, RelativeDotSlashDirName, 0777));
    LxtCheckErrnoFailure(mkdirat(ParentFd, RelativeDotSlashDirName, 0777), EEXIST);
    LxtCheckErrnoFailure(mkdir(TestDirName, 0777), EEXIST);
    LxtCheckErrnoZeroSuccess(rmdir(TestDirName));

    //
    // Test mkdirat with a AT_FDCWD. Change the working directory first
    // since it's normally / for tests which is not interesting.
    //

    LxtCheckErrnoZeroSuccess(chdir(FS_TEST_DIR_PARENT));
    LxtCheckErrnoFailure(mkdirat(AT_FDCWD, RelativeSubDirName, 0777), ENOENT);
    LxtCheckErrnoZeroSuccess(mkdir(RelativeDirName, 0777));
    LxtCheckErrnoFailure(mkdirat(AT_FDCWD, RelativeDirName, 0777), EEXIST);
    LxtCheckErrnoFailure(mkdir(TestDirName, 0777), EEXIST);
    LxtCheckErrnoZeroSuccess(mkdir(RelativeSubDirName, 0777));
    LxtCheckErrnoFailure(mkdirat(AT_FDCWD, RelativeSubDirName, 0777), EEXIST);
    LxtCheckErrnoFailure(mkdir(TestSubDirName, 0777), EEXIST);
    LxtCheckErrnoZeroSuccess(rmdir(TestSubDirName));
    LxtCheckErrnoZeroSuccess(rmdir(TestDirName));

    //
    // Empty path should return ENOENT, even with invalid fd.
    //

    LxtCheckErrnoFailure(mkdirat(AT_FDCWD, "", 0777), ENOENT);
    LxtCheckErrnoFailure(mkdirat(-1, "", 0777), ENOENT);

    //
    // Invalid fd.
    //

    LxtCheckErrnoFailure(mkdirat(-1, RelativeDirName, 0777), EBADF);

    //
    // Special path edge cases.
    //

    LxtCheckErrnoFailure(mkdirat(ParentFd, ".", 0777), EEXIST);
    LxtCheckErrnoFailure(mkdirat(ParentFd, "..", 0777), EEXIST);
    LxtCheckErrnoFailure(mkdirat(ParentFd, "/", 0777), EEXIST);
    LxtCheckErrnoFailure(mkdirat(ParentFd, "/.", 0777), EEXIST);
    LxtCheckErrnoFailure(mkdirat(ParentFd, "/..", 0777), EEXIST);
    LxtCheckErrnoFailure(mkdirat(ParentFd, "/data/", 0777), EEXIST);
    LxtCheckErrnoFailure(mkdirat(ParentFd, "/data/.", 0777), EEXIST);
    LxtCheckErrnoFailure(mkdirat(ParentFd, "/data/..", 0777), EEXIST);

    //
    // Variation succeeded.
    //

ErrorExit:
    if (Fd >= 0)
    {
        close(Fd);
    }

    if (ParentFd >= 0)
    {
        close(ParentFd);
    }

    unlink(TestDirName);
    rmdir(TestSubDirName);
    rmdir(TestDirName);
    rmdir(RelativeDirName);

    //
    // Restore working directory (other tests depend on it)
    //

    chdir("/");
    return Result;
}

int FsCommonTestChdir(PLXT_ARGS Args)

/*++
--*/

{
    int Result;
    int FileDescriptor;
    const char TestFilePath1[] = "data/fstest/test_chdir.txt";
    const char TestFilePath2[] = "test_chdir.txt";
    const char DataTestDirPath[] = FS_TEST_DIR_PARENT;
    const char DataTestDirPath2[] = FS_TEST_DIR_PARENT "/";
    const char FailureDirPath[] = "/system12314/";

    LxtCheckErrno(Result = chdir("/"));

    //
    // Since the working directory is "/", create a file under /data/fstest
    //

    LxtLogInfo("Creating file using path %s", TestFilePath1);
    LxtCheckErrno(FileDescriptor = open(TestFilePath1, O_RDWR | O_CREAT, S_IRWXU));
    LxtLogInfo("Opened file using path %s, closing now", TestFilePath1);
    LxtCheckClose(FileDescriptor);

    //
    // Change working directory to /system
    //

    LxtLogInfo("Changing working dir to %s", DataTestDirPath);
    LxtCheckErrno(Result = chdir(DataTestDirPath));

    //
    // Open the same file now that the working directory is different.
    //

    LxtLogInfo("Opening file using path %s", TestFilePath2);
    LxtCheckErrno(FileDescriptor = open(TestFilePath2, O_RDWR));
    LxtCheckClose(FileDescriptor);
    LxtLogInfo("Opened file using path %s successfully!", TestFilePath2);

    //
    // Change working directory to /system/
    //

    LxtLogInfo("Changing working dir to %s", DataTestDirPath2);
    LxtCheckErrno(Result = chdir(DataTestDirPath2));

    //
    // Open the same file now that the working directory is different.
    //

    LxtLogInfo("Opening file using path %s", TestFilePath2);
    LxtCheckErrno(FileDescriptor = open(TestFilePath2, O_RDWR));
    LxtCheckClose(FileDescriptor);
    LxtLogInfo("Opened file using path %s successfully!", TestFilePath2);

    //
    // Change working directory to a bogus path; this should fail.
    //

    LxtLogInfo("Changing working dir to %s", FailureDirPath);
    Result = chdir(FailureDirPath);
    if (Result != -1)
    {
        LxtLogError("Chdir to directory ('%s') succeeded unexpectedly, %d", FailureDirPath, errno);
        Result = -1;
        goto ErrorExit;
    }

    LxtLogInfo("Changing working dir to %s failed as expected", FailureDirPath);

    Result = 0;

    LxtLogInfo("TEST SUCCESSFUL!");

ErrorExit:

    if (FileDescriptor != -1)
    {
        close(FileDescriptor);
    }

    unlink(FS_TEST_DIR_PARENT "/test_chdir.txt");
    return Result;
}

typedef struct _UNLINK_AT_VARIATION
{
    const char* Description;
    const int* Fd;
    const char* Path;
    const unsigned int Flags;
    const int DesiredResult;
    const int DesiredError;
} UNLINK_AT_VARIATION, *PUNLINK_AT_VARIATION;

int FsCommonTestUnlinkAt(PLXT_ARGS Args)

/*++

Routine Description:

This routine runs tests associated with the unlinkat syscall.

Arguments:

Args - Supplies the command line arguments.

Return Value:

Returns 0 on success, -1 on failure.

--*/

{

    const char Child1[] = "newfile";
    const char Child1FullPath[] = FS_TEST_DIR_PARENT "/test_unlinkat/newfile";
    int DirFd;
    const char DirPath[] = FS_TEST_DIR_PARENT "/test_unlinkat";
    int Result;
    const char* RmdirPath = NULL;
    PUNLINK_AT_VARIATION ThisVariation;
    UNLINK_AT_VARIATION UnlinkAtVariations[] = {
        {"unlinkat with invalid flags", &DirFd, Child1, 0x80000000, -1, EINVAL},
        {"unlink via unlinkat with full path", &DirFd, Child1FullPath, 0, 0, 0},
        {"unlink via unlinkat with relative path", &DirFd, Child1, 0, 0, 0},
        {"rmdir via unlinkat with full path", &DirFd, Child1FullPath, AT_REMOVEDIR, 0, 0},
        {"rmdir via unlinkat with relative path", &DirFd, Child1, AT_REMOVEDIR, 0, 0}};
    const char* UnlinkName = NULL;
    unsigned int Variation;

    //
    // Initialize locals.
    //

    DirFd = -1;
    RmdirPath = NULL;
    UnlinkName = NULL;
    Variation = 0;

    //
    // Make a directory.
    //

    LxtLogInfo("Creating test directory folder %s", DirPath);
    LxtCheckErrnoZeroSuccess(mkdir(DirPath, 0777));
    RmdirPath = DirPath;

    //
    // Open the directory.
    //

    LxtCheckErrno(open(DirPath, O_RDONLY));
    DirFd = Result;
    LxtLogInfo("Opened test directory folder, fd = %d", DirFd);

    //
    // Unlink a child that we haven't created yet. This should fail.
    // umount

    LxtLogInfo("Unlinking child %s without creating it", Child1);
    LxtCheckErrnoFailure(unlinkat(DirFd, Child1, 0), ENOENT);

    //
    // Test various things that should succeed.
    //

    for (Variation = 0; Variation < sizeof(UnlinkAtVariations) / sizeof(UnlinkAtVariations[0]); Variation += 1)
    {

        ThisVariation = &UnlinkAtVariations[Variation];

        //
        // Create child file. This should succeed.
        //

        LxtLogInfo("Attempting %s", ThisVariation->Description);

        if ((ThisVariation->Flags & AT_REMOVEDIR) == 0)
        {
            LxtLogInfo("Creating child file %s", Child1);

            LxtCheckErrno(openat(DirFd, Child1, O_RDWR | O_CREAT, S_IRWXU));
            close(Result);
        }
        else
        {
            LxtLogInfo("Creating child directory %s", Child1);

            LxtCheckErrnoZeroSuccess(mkdir(Child1FullPath, S_IRWXU));
        }

        UnlinkName = Child1FullPath;

        //
        // Execute the desired test variation.
        //

        Result = unlinkat(*ThisVariation->Fd, ThisVariation->Path, ThisVariation->Flags);

        if (Result != ThisVariation->DesiredResult)
        {
            LxtLogError("unlinkat returned unexpected result; returned %d, expected %d", Result, ThisVariation->DesiredResult);
            Result = -1;
            goto ErrorExit;
        }

        if (Result != 0 && errno != ThisVariation->DesiredError)
        {
            LxtLogError("unlinkat failed with unexpected error; errno %d, expected %d", errno, ThisVariation->DesiredError);
            Result = -1;
            goto ErrorExit;
        }

        //
        // If the variation expected success, we've already deleted the object.
        // If not, we need to delete it below.
        //

        if (Result == 0)
        {
            UnlinkName = NULL;
        }

        if (UnlinkName != NULL)
        {
            unlink(UnlinkName);
            UnlinkName = NULL;
        }
    }

    LxtLogInfo("FsCommonTestUnlinkAt succeeded!");

ErrorExit:
    if (DirFd >= 0)
    {
        close(DirFd);
    }

    if (UnlinkName != NULL)
    {
        unlink(UnlinkName);
    }

    if (RmdirPath != NULL)
    {
        rmdir(RmdirPath);
    }

    return Result;
}

int FsCommonTestFchownAt(PLXT_ARGS Args)

/*++

Description:

    This routine tests the fchownat system call.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int DirFd;
    int Fd;
    struct stat Original;
    int Result;
    struct stat Stat;

    DirFd = -1;
    Fd = -1;

    //
    // Set up the test environment.
    //

    LxtCheckErrnoZeroSuccess(mkdir(FS_FCHOWNAT_TEST_DIR, 0777));
    LxtCheckErrno(Fd = creat(FS_FCHOWNAT_TEST_DIR "/testfile", 0666));
    LxtCheckClose(Fd);
    LxtCheckErrnoZeroSuccess(symlink(FS_FCHOWNAT_TEST_DIR "/testfile", FS_FCHOWNAT_TEST_DIR "/testlink"));

    LxtCheckErrnoZeroSuccess(symlink(FS_FCHOWNAT_TEST_DIR, FS_FCHOWNAT_TEST_DIR "/dirlink"));

    LxtCheckErrno(DirFd = open(FS_FCHOWNAT_TEST_DIR, O_RDONLY | O_DIRECTORY));

    //
    // Change owner.
    //

    LxtCheckErrnoZeroSuccess(fchownat(DirFd, "testfile", 2000, 3000, 0));
    LxtCheckErrnoZeroSuccess(lstat(FS_FCHOWNAT_TEST_DIR "/testfile", &Stat));
    LxtCheckEqual(Stat.st_uid, 2000, "%d");
    LxtCheckEqual(Stat.st_gid, 3000, "%d");

    //
    // Using AT_FDCWD.
    //

    LxtCheckErrnoZeroSuccess(chdir(FS_FCHOWNAT_TEST_DIR));
    LxtCheckErrnoZeroSuccess(fchownat(AT_FDCWD, "testfile", 2001, 3001, 0));
    LxtCheckErrnoZeroSuccess(lstat(FS_FCHOWNAT_TEST_DIR "/testfile", &Stat));
    LxtCheckEqual(Stat.st_uid, 2001, "%d");
    LxtCheckEqual(Stat.st_gid, 3001, "%d");

    //
    // Symlinks should be followed.
    //

    LxtCheckErrnoZeroSuccess(lstat(FS_FCHOWNAT_TEST_DIR "/testlink", &Original));
    LxtCheckErrnoZeroSuccess(fchownat(DirFd, "testlink", 2002, 3002, 0));
    LxtCheckErrnoZeroSuccess(lstat(FS_FCHOWNAT_TEST_DIR "/testfile", &Stat));
    LxtCheckEqual(Stat.st_uid, 2002, "%d");
    LxtCheckEqual(Stat.st_gid, 3002, "%d");
    LxtCheckErrnoZeroSuccess(lstat(FS_FCHOWNAT_TEST_DIR "/testlink", &Stat));
    LxtCheckEqual(Stat.st_uid, Original.st_uid, "%d");
    LxtCheckEqual(Stat.st_gid, Original.st_gid, "%d");

    //
    // Not followed with AT_SYMLINK_NOFOLLOW.
    //

    LxtCheckErrnoZeroSuccess(fchownat(DirFd, "testlink", 2003, 3003, AT_SYMLINK_NOFOLLOW));

    LxtCheckErrnoZeroSuccess(lstat(FS_FCHOWNAT_TEST_DIR "/testfile", &Stat));
    LxtCheckEqual(Stat.st_uid, 2002, "%d");
    LxtCheckEqual(Stat.st_gid, 3002, "%d");
    LxtCheckErrnoZeroSuccess(lstat(FS_FCHOWNAT_TEST_DIR "/testlink", &Stat));
    LxtCheckEqual(Stat.st_uid, 2003, "%d");
    LxtCheckEqual(Stat.st_gid, 3003, "%d");

    //
    // Fd must be a directory, not a symlink to a directory.
    //

    LxtCheckErrno(Fd = open(FS_FCHOWNAT_TEST_DIR "/dirlink", O_NOFOLLOW | O_PATH));
    LxtCheckErrnoFailure(fchownat(Fd, "testlink", 2004, 3004, 0), ENOTDIR);
    LxtCheckErrnoFailure(fchownat(Fd, "testlink", 2004, 3004, AT_SYMLINK_NOFOLLOW), ENOTDIR);

    LxtCheckClose(Fd);

    //
    // AT_EMPTY_PATH changes the file itself.
    //

    LxtCheckErrno(Fd = open(FS_FCHOWNAT_TEST_DIR "/testfile", O_RDONLY));
    LxtCheckErrnoZeroSuccess(fchownat(Fd, "", 2005, 3005, AT_EMPTY_PATH));

    LxtCheckErrnoZeroSuccess(lstat(FS_FCHOWNAT_TEST_DIR "/testfile", &Stat));
    LxtCheckEqual(Stat.st_uid, 2005, "%d");
    LxtCheckEqual(Stat.st_gid, 3005, "%d");
    LxtCheckClose(Fd);

    //
    // If the symlink is an FD, it's not followed regardless of flags.
    //

    LxtCheckErrno(Fd = open(FS_FCHOWNAT_TEST_DIR "/testlink", O_NOFOLLOW | O_PATH));
    LxtCheckErrnoZeroSuccess(fchownat(Fd, "", 2006, 3006, AT_EMPTY_PATH));

    LxtCheckErrnoZeroSuccess(lstat(FS_FCHOWNAT_TEST_DIR "/testlink", &Stat));
    LxtCheckEqual(Stat.st_uid, 2006, "%d");
    LxtCheckEqual(Stat.st_gid, 3006, "%d");
    LxtCheckErrnoZeroSuccess(lstat(FS_FCHOWNAT_TEST_DIR "/testfile", &Stat));
    LxtCheckEqual(Stat.st_uid, 2005, "%d");
    LxtCheckEqual(Stat.st_gid, 3005, "%d");
    LxtCheckErrnoZeroSuccess(fchownat(Fd, "", 2007, 3007, AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW));

    LxtCheckErrnoZeroSuccess(lstat(FS_FCHOWNAT_TEST_DIR "/testlink", &Stat));
    LxtCheckEqual(Stat.st_uid, 2007, "%d");
    LxtCheckEqual(Stat.st_gid, 3007, "%d");
    LxtCheckErrnoZeroSuccess(lstat(FS_FCHOWNAT_TEST_DIR "/testfile", &Stat));
    LxtCheckEqual(Stat.st_uid, 2005, "%d");
    LxtCheckEqual(Stat.st_gid, 3005, "%d");

ErrorExit:
    if (DirFd >= 0)
    {
        close(DirFd);
    }

    if (Fd >= 0)
    {
        close(Fd);
    }

    unlink(FS_FCHOWNAT_TEST_DIR "/dirlink");
    unlink(FS_FCHOWNAT_TEST_DIR "/testlink");
    unlink(FS_FCHOWNAT_TEST_DIR "/testfile");
    rmdir(FS_FCHOWNAT_TEST_DIR);
    return Result;
}

int FsCommonTestFstatAt64(PLXT_ARGS Args)

/*++

Routine Description:

    This routine runs tests associated with the fstatat64 syscall.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

#if !defined(__amd64__)
    struct stat Buffer;
#else
    struct stat64 Buffer;
#endif

    const char Child1[] = "newfile";
    const char Child1FullPath[] = FS_TEST_DIR_PARENT "/test_fstatat64/newfile";
    int Child1Fd;
    int DirFd;
    const char DirPath[] = FS_TEST_DIR_PARENT "/test_fstatat64";
    int Result;
    const char Symlink[] = "symlink1";
    int SymlinkFd;
    const char SymlinkFullPath[] = FS_TEST_DIR_PARENT "/test_fstatat64/symlink1";
    const char DirSymlinkPath[] = FS_TEST_DIR_PARENT "/test_fstatat64/symlink2";

    //
    // Initialize locals.
    //

    Child1Fd = -1;
    DirFd = -1;
    SymlinkFd = -1;

    //
    // Make a directory.
    //

    LxtLogInfo("Creating test directory folder %s", DirPath);
    LxtCheckErrnoZeroSuccess(mkdir(DirPath, 0777));

    //
    // Open the directory.
    //

    LxtCheckErrno(open(DirPath, O_RDONLY));
    DirFd = Result;
    LxtLogInfo("Opened test directory folder, fd = %d", DirFd);

    //
    // Create a file.
    //

    LxtCheckErrno(Child1Fd = creat(Child1FullPath, 0777 | S_IRWXU | S_ISGID | S_ISUID));

    //
    // Create the symlinks.
    //

    LxtCheckErrnoZeroSuccess(symlink(Child1FullPath, SymlinkFullPath));
    LxtCheckErrnoZeroSuccess(symlink(DirPath, DirSymlinkPath));

    //
    // Call fstatat64 with an absolute path.
    //

    LxtCheckErrno(LxtFStatAt64(DirFd, Child1FullPath, &Buffer, 0));

    //
    // Call fstatat64 with a relative path.
    //

    LxtCheckErrno(LxtFStatAt64(DirFd, Child1, &Buffer, 0));

    //
    // Call fstatat64 on the symlink.
    //

    LxtCheckErrno(LxtFStatAt64(DirFd, Symlink, &Buffer, 0));
    LxtLogInfo("symlink mode: %o", Buffer.st_mode);
    if ((Buffer.st_mode & S_IFMT) != S_IFREG)
    {
        LxtLogError("Expected regular file, got: %x", Buffer.st_mode & S_IFMT);
    }

    //
    // Call fstatat64 on the symlink with the AT_SYMLINK_NOFOLLOW flag.
    //

    LxtCheckErrno(LxtFStatAt64(DirFd, Symlink, &Buffer, AT_SYMLINK_NOFOLLOW));
    LxtLogInfo("symlink mode with AT_SYMLINK_NOFOLLOW: %o", Buffer.st_mode);
    if ((Buffer.st_mode & S_IFMT) != S_IFLNK)
    {
        LxtLogError("Expected symlink, got: %x", Buffer.st_mode & S_IFMT);
    }

    //
    // Ensure that fstatat fails if the file descriptor is not a directory.
    //

    LxtCheckErrnoFailure(LxtFStatAt64(Child1Fd, "foo", &Buffer, 0), ENOTDIR);

    //
    // Use AT_EMPTY_PATH to directly stat the file descriptor.
    //

    LxtCheckErrnoZeroSuccess(LxtFStatAt64(DirFd, "", &Buffer, AT_EMPTY_PATH));
    LxtLogInfo("dir mode with AT_EMPTY_PATH: %o", Buffer.st_mode);
    LxtCheckTrue(S_ISDIR(Buffer.st_mode));

    //
    // AT_EMPTY_PATH does nothing if the path is not empty.
    //

    LxtCheckErrnoZeroSuccess(LxtFStatAt64(DirFd, Child1, &Buffer, AT_EMPTY_PATH));
    LxtLogInfo("child mode with AT_EMPTY_PATH: %o", Buffer.st_mode);
    LxtCheckTrue(S_ISREG(Buffer.st_mode));

    //
    // AT_EMPTY_PATH on a symlink does not follow the link regardless of
    // AT_SYMLINK_NOFOLLOW.
    //

    LxtCheckErrno(SymlinkFd = open(SymlinkFullPath, O_NOFOLLOW | O_PATH));
    LxtCheckErrnoZeroSuccess(LxtFStatAt64(SymlinkFd, "", &Buffer, AT_EMPTY_PATH));
    LxtLogInfo("symlink mode with AT_EMPTY_PATH: %o", Buffer.st_mode);
    LxtCheckTrue(S_ISLNK(Buffer.st_mode));
    LxtCheckErrnoZeroSuccess(LxtFStatAt64(SymlinkFd, "", &Buffer, (AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW)));

    LxtLogInfo("symlink mode with AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW: %o", Buffer.st_mode);

    LxtCheckTrue(S_ISLNK(Buffer.st_mode));
    LxtCheckClose(SymlinkFd);

    //
    // If the path is not empty, the FD must be a directory; a symlink
    // to a directory does not work.
    //

    LxtCheckErrno(SymlinkFd = open(DirSymlinkPath, O_NOFOLLOW | O_PATH));
    LxtCheckErrnoFailure(LxtFStatAt64(SymlinkFd, Child1, &Buffer, 0), ENOTDIR);

ErrorExit:
    if (Child1Fd != -1)
    {
        LxtClose(Child1Fd);
    }

    if (DirFd != -1)
    {
        LxtClose(DirFd);
    }

    if (SymlinkFd != -1)
    {
        close(SymlinkFd);
    }

    unlink(DirSymlinkPath);
    remove(Child1FullPath);
    remove(SymlinkFullPath);
    rmdir(DirPath);

    return Result;
}

int FsCommonTestDeleteCurrentWorkingDirectory(PLXT_ARGS Args)

/*++

Description:

    This routine tests the behavior if the current working directory is
    unlinked for LxFs.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int Result;

    LxtCheckErrno(LxtFsDeleteCurrentWorkingDirectoryCommon(FS_TEST_DIR_PARENT, 0));

ErrorExit:
    return Result;
}

int FsCommonTestDeleteLoop(PLXT_ARGS Args)

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

    if (g_LxtFsInfo.FsType == LxtFsTypeVirtioFs)
    {
        LxtLogInfo("TODO: debug this test on virtiofs");
        Result = 0;
        goto ErrorExit;
    }

    LxtCheckResult(LxtFsDeleteLoopCommon(FS_DELETELOOP_TEST_DIR));

ErrorExit:
    return Result;
}

int FsCommonTestDeleteOpenFile(PLXT_ARGS Args)

/*++

Description:

    This routine tests using unlink and rmdir on a LxFs file/directory that's open.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int Result;

    LxtCheckErrno(LxtFsDeleteOpenFileCommon(FS_TEST_DIR_PARENT, 0));

ErrorExit:
    return Result;
}

int FsCommonTestFchdir(PLXT_ARGS Args)

/*++
--*/

{
    int Result;
    int FileDescriptor;
    const char TestFilePath1[] = "data/fstest/test_chdir.txt";
    const char TestFilePath2[] = "test_chdir.txt";
    const char DataTestDirPath[] = FS_TEST_DIR_PARENT;
    const char DataTestDirPath2[] = FS_TEST_DIR_PARENT "/";
    const char FailureDirPath[] = "/system12314/";

    FileDescriptor = -1;
    LxtCheckErrno(chdir("/"));

    //
    // Since the working directory is "/", create a file under /data/fstest
    //

    LxtLogInfo("Creating file using path %s", TestFilePath1);
    LxtCheckErrno(FileDescriptor = open(TestFilePath1, O_RDWR | O_CREAT, S_IRWXU));
    LxtCheckClose(FileDescriptor);

    //
    // Change working directory to /data/fstest
    //

    LxtLogInfo("Changing working dir to %s", DataTestDirPath);
    LxtCheckErrno(FileDescriptor = open(DataTestDirPath, O_RDONLY | O_DIRECTORY, 0));
    LxtCheckErrno(fchdir(FileDescriptor));
    LxtCheckClose(FileDescriptor);

    //
    // Open the same file now that the working directory is different.
    //

    LxtLogInfo("Opening file using path %s", TestFilePath2);
    LxtCheckErrno(FileDescriptor = open(TestFilePath2, O_RDWR));
    LxtLogInfo("Opened file using path %s successfully!", TestFilePath2);
    LxtCheckClose(FileDescriptor);

    //
    // Change working directory to /system/
    //

    LxtLogInfo("Changing working dir to %s", DataTestDirPath2);
    LxtCheckErrno(FileDescriptor = open(DataTestDirPath2, O_RDONLY | O_DIRECTORY, 0));
    LxtCheckErrno(fchdir(FileDescriptor));
    LxtCheckClose(FileDescriptor);

    //
    // Open the same file now that the working directory is different.
    //

    LxtLogInfo("Opening file using path %s", TestFilePath2);
    LxtCheckErrno(FileDescriptor = open(TestFilePath2, O_RDWR));
    LxtCheckClose(FileDescriptor);
    LxtLogInfo("Opened file using path %s successfully!", TestFilePath2);

    //
    // Change working directory to a bogus fd; this should fail.
    //

    LxtLogInfo("Changing working dir to %s", FailureDirPath);
    LxtCheckErrnoFailure(fchdir(-1), EBADF);
    LxtLogInfo("Changing working dir to %s failed as expected", FailureDirPath);

    Result = 0;

    LxtLogInfo("TEST SUCCESSFUL!");

ErrorExit:
    if (FileDescriptor != -1)
    {
        close(FileDescriptor);
    }

    unlink(FS_TEST_DIR_PARENT "/test_chdir.txt");
    return Result;
}

int FsCommonTestMknod(PLXT_ARGS Args)

/*++

Description:

    This routine tests creation of device nodes using mknod.

    N.B. Creation of fifos is covered by the pipe unit tests, and other types
         of files are sufficiently covered by the LTP tests.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    char Buffer[10];
    ssize_t BytesRead;
    int Fd;
    struct stat FileStat;
    int Result;
    struct stat Stat;
    char Zero[10];

    Fd = -1;

    //
    // Test basic device node creation.
    //

    umask(0);
    LxtCheckErrnoZeroSuccess(mknod(FS_MKNOD_TEST_FILE, S_IFCHR | 0666, makedev(1, 5)));

    LxtCheckErrnoZeroSuccess(stat(FS_MKNOD_TEST_FILE, &Stat));
    LxtCheckEqual(Stat.st_mode, S_IFCHR | 0666, "0%o");
    LxtCheckNotEqual(Stat.st_ino, 0, "%llu");
    LxtCheckEqual(Stat.st_rdev, makedev(1, 5), "0x%x");
    LxtCheckNotEqual(Stat.st_rdev, Stat.st_dev, "0x%x");

    //
    // Test using the device node.
    //

    LxtCheckErrno(Fd = open(FS_MKNOD_TEST_FILE, O_RDONLY));
    memset(Zero, 0, sizeof(Buffer));
    memset(Buffer, 1, sizeof(Buffer));
    LxtCheckErrno(BytesRead = read(Fd, Buffer, sizeof(Buffer)));
    LxtCheckEqual(BytesRead, sizeof(Buffer), "%d");
    LxtCheckMemoryEqual(Buffer, Zero, sizeof(Buffer));

    //
    // Check the fd's inode matches the stat results.
    //

    LxtCheckErrnoZeroSuccess(fstat(Fd, &FileStat));
    LxtCheckEqual(FileStat.st_ino, Stat.st_ino, "%llu");
    LxtCheckEqual(FileStat.st_dev, Stat.st_dev, "0x%x");
    LxtCheckEqual(FileStat.st_rdev, Stat.st_rdev, "0x%x");
    LxtCheckEqual(FileStat.st_mode, Stat.st_mode, "0%o");

    //
    // Check the fd's path follows renames.
    //

    LxtCheckResult(LxtCheckFdPath(Fd, FS_MKNOD_TEST_FILE));
    LxtCheckErrnoZeroSuccess(rename(FS_MKNOD_TEST_FILE, FS_MKNOD_TEST_FILE2));
    LxtCheckResult(LxtCheckFdPath(Fd, FS_MKNOD_TEST_FILE2));
    LxtCheckErrnoZeroSuccess(close(Fd));
    Fd = -1;

    //
    // Check opening with O_PATH.
    //

    LxtCheckErrno(Fd = open(FS_MKNOD_TEST_FILE2, O_PATH));
    LxtCheckErrnoFailure(read(Fd, Buffer, sizeof(Buffer)), EBADF);
    LxtCheckErrnoZeroSuccess(fstat(Fd, &FileStat));
    LxtCheckEqual(FileStat.st_ino, Stat.st_ino, "%llu");
    LxtCheckEqual(FileStat.st_dev, Stat.st_dev, "0x%x");
    LxtCheckEqual(FileStat.st_rdev, Stat.st_rdev, "0x%x");
    LxtCheckEqual(FileStat.st_mode, Stat.st_mode, "0%o");
    LxtCheckErrnoZeroSuccess(close(Fd));
    Fd = -1;
    LxtCheckErrnoZeroSuccess(unlink(FS_MKNOD_TEST_FILE2));

    //
    // Check mknod applies the umask.
    //

    umask(022);
    LxtCheckErrnoZeroSuccess(mknod(FS_MKNOD_TEST_FILE, S_IFCHR | 0666, makedev(1, 5)));

    LxtCheckErrnoZeroSuccess(stat(FS_MKNOD_TEST_FILE, &Stat));
    LxtCheckEqual(Stat.st_mode, S_IFCHR | 0644, "0%o");
    LxtCheckErrnoZeroSuccess(unlink(FS_MKNOD_TEST_FILE));

    //
    // Create a device with a non-existing major number.
    //
    // N.B. This test could fail on real Linux if at any point a device is
    //      added with this number.
    //

    LxtCheckErrnoZeroSuccess(mknod(FS_MKNOD_TEST_FILE, S_IFCHR | 0666, makedev(200, 0)));

    LxtCheckErrnoFailure(open(FS_MKNOD_TEST_FILE, O_RDONLY), ENXIO);
    LxtCheckErrnoZeroSuccess(unlink(FS_MKNOD_TEST_FILE));

    //
    // Existing major number, non-existing minor number.
    //

    LxtCheckErrnoZeroSuccess(mknod(FS_MKNOD_TEST_FILE, S_IFCHR | 0666, makedev(1, 200)));

    LxtCheckErrnoFailure(open(FS_MKNOD_TEST_FILE, O_RDONLY), ENXIO);
    LxtCheckErrnoZeroSuccess(unlink(FS_MKNOD_TEST_FILE));

    //
    // Major number 10 returns different error code for unknown devices.
    //

    LxtCheckErrnoZeroSuccess(mknod(FS_MKNOD_TEST_FILE, S_IFCHR | 0666, makedev(10, 100)));

    LxtCheckErrnoFailure(open(FS_MKNOD_TEST_FILE, O_RDONLY), ENODEV);
    LxtCheckErrnoZeroSuccess(unlink(FS_MKNOD_TEST_FILE));

    //
    // Nonexistent block device.
    //
    // N.B. Currently, no block devices exist in WSL.
    //

    LxtCheckErrnoZeroSuccess(mknod(FS_MKNOD_TEST_FILE, S_IFBLK | 0666, makedev(200, 0)));

    LxtCheckErrnoFailure(open(FS_MKNOD_TEST_FILE, O_RDONLY), ENXIO);
    LxtCheckErrnoZeroSuccess(unlink(FS_MKNOD_TEST_FILE));

ErrorExit:
    if (Fd >= 0)
    {
        close(Fd);
    }

    unlink(FS_MKNOD_TEST_FILE);
    unlink(FS_MKNOD_TEST_FILE2);
    return Result;
}

int FsCommonTestMknodSecurity(PLXT_ARGS Args)

/*++

Description:

    This routine tests whether mknod correctly checks capabilities.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    struct __user_cap_data_struct CapData[2];
    struct __user_cap_header_struct CapHeader;
    pid_t ChildPid;
    int Result;

    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {

        //
        // Drop the CAP_MKNOD capability.
        //

        memset(&CapData, 0, sizeof(CapData));
        memset(&CapHeader, 0, sizeof(CapHeader));
        CapHeader.version = _LINUX_CAPABILITY_VERSION_3;
        LxtCheckErrno(LxtCapGet(&CapHeader, CapData)) CapData[CAP_TO_INDEX(CAP_MKNOD)].effective &= ~CAP_TO_MASK(CAP_MKNOD);
        LxtCheckErrno(LxtCapSet(&CapHeader, CapData));

        //
        // Creating devices should fail.
        //

        LxtCheckErrnoFailure(mknod(FS_MKNOD_TEST_FILE, S_IFCHR | 0666, makedev(1, 5)), EPERM);

        LxtCheckErrnoFailure(mknod(FS_MKNOD_TEST_FILE, S_IFBLK | 0666, makedev(1, 5)), EPERM);

        //
        // Other file types should still succeed.
        //

        LxtCheckErrnoZeroSuccess(mknod(FS_MKNOD_TEST_FILE, S_IFREG | 0666, 0));
        LxtCheckErrnoZeroSuccess(unlink(FS_MKNOD_TEST_FILE));
        LxtCheckErrnoZeroSuccess(mknod(FS_MKNOD_TEST_FILE, S_IFIFO | 0666, 0));
        LxtCheckErrnoZeroSuccess(unlink(FS_MKNOD_TEST_FILE));
        LxtCheckErrnoZeroSuccess(mknod(FS_MKNOD_TEST_FILE, S_IFSOCK | 0666, 0));
        LxtCheckErrnoZeroSuccess(unlink(FS_MKNOD_TEST_FILE));
        exit(0);
    }

    LxtCheckResult(LxtWaitPidPoll(ChildPid, 0));

ErrorExit:
    unlink(FS_MKNOD_TEST_FILE);
    return Result;
}

int FsCommonTestNoatimeFlag(PLXT_ARGS Args)

/*++
--*/

{

    char Buffer[100];
    int Bytes;
    char* DentsBuffer = NULL;
    int DentsBufferSize = 2 * 1024 * 1024;
    int FileDescriptor = -1;
    int Result;
    const char SourceGetdents[] = FS_TEST_DIR_PARENT "/";
    const char SourceOpen[] = FS_TEST_DIR_PARENT "/fs_access_time_test.bin";
    const char Content[] = "I am your father! Noooo!";
    struct iovec Iov;
    char SingleEntry[100];
    int SingleEntrySize;
    struct stat StatA;
    struct stat StatB;

    //
    // Plan 9 and virtiofs do not forward O_NOATIME to the server.
    //

    if (g_LxtFsInfo.FsType == LxtFsTypePlan9)
    {
        LxtLogInfo("Test not supported on Plan 9.");
        Result = 0;
        goto ErrorExit;
    }

    if (g_LxtFsInfo.FsType == LxtFsTypeVirtioFs)
    {
        LxtLogInfo("Test not supported on virtiofs.");
        Result = 0;
        goto ErrorExit;
    }

    //
    // Create the test file; this should succeed.
    //

    LxtCheckErrno(FileDescriptor = open(SourceOpen, O_RDWR | O_CREAT, S_IRWXU));

    //
    // Test O_NOATIME for read no access time changes.
    //

    LxtCheckErrno(fstat(FileDescriptor, &StatA));
    LxtCheckErrno(close(FileDescriptor));
    LxtCheckErrno(FileDescriptor = open(SourceOpen, O_RDWR));
    LxtCheckErrno(fstat(FileDescriptor, &StatB));
    LxtCheckMemoryEqual(&StatA.st_atim, &StatB.st_atim, sizeof(StatA.st_atim));

    usleep(10 * 1000);
    LxtCheckErrno(Bytes = write(FileDescriptor, Content, sizeof(Content)));
    LxtCheckEqual(Bytes, sizeof(Content), "%d");
    LxtCheckErrno(fstat(FileDescriptor, &StatB));

    //
    // NTFS updates the atime when other timestamps are updated, even when
    // O_NOATIME is specified.
    //

    if (g_LxtFsInfo.Flags.DrvFsBehavior == 0)
    {
        LxtCheckMemoryEqual(&StatA.st_atim, &StatB.st_atim, sizeof(StatA.st_atim));
    }

    LxtCheckErrno(close(FileDescriptor));
    LxtCheckErrno(FileDescriptor = open(SourceOpen, O_RDWR));
    memset(&Buffer, 0, sizeof(Buffer));
    usleep(10 * 1000);
    LxtCheckErrno(Bytes = read(FileDescriptor, Buffer, sizeof(Buffer)));
    LxtCheckEqual(Bytes, sizeof(Content), "%d");

    //
    // Close first; with DrvFs, in case NTFS has atime updates enabled it
    // won't do it until the handle is closed.
    //
    // TODO_LX: Once NTFS timestamp updating is fixed, change this back.
    //

    LxtCheckErrno(close(FileDescriptor));
    LxtCheckErrno(stat(SourceOpen, &StatA));

    //
    // TODO_LX: Uncomment when the file system is mounted without noatime.
    //
    // if (memcmp(&StatA.st_atim, &StatB.st_atim, sizeof(StatA.st_atim)) == 0) {
    //     LxtLogError("Access time was supposed to be updated.");
    //     Result = -1;
    //     goto ErrorExit;
    // }
    //

    LxtCheckErrno(FileDescriptor = open(SourceOpen, O_RDWR | O_NOATIME));
    memset(&Buffer, 0, sizeof(Buffer));
    usleep(10 * 1000);
    LxtCheckErrno(Bytes = read(FileDescriptor, Buffer, sizeof(Buffer)));
    LxtCheckEqual(Bytes, sizeof(Content), "%d");
    LxtCheckErrno(fstat(FileDescriptor, &StatB));
    LxtCheckMemoryEqual(&StatA.st_atim, &StatB.st_atim, sizeof(StatA.st_atim));

    LxtCheckErrno(close(FileDescriptor));
    LxtCheckErrno(stat(SourceOpen, &StatB));
    LxtCheckMemoryEqual(&StatA.st_atim, &StatB.st_atim, sizeof(StatA.st_atim));

    FileDescriptor = -1;

    //
    // Test O_NOATIME for readv no access time changes.
    //

    LxtCheckErrno(FileDescriptor = open(SourceOpen, O_RDWR | O_NOATIME));
    Iov.iov_base = Buffer;
    Iov.iov_len = sizeof(Buffer);
    usleep(10 * 1000);
    LxtCheckErrno(Bytes = readv(FileDescriptor, &Iov, 1));
    LxtCheckEqual(Bytes, sizeof(Content), "%d");
    LxtCheckErrno(fstat(FileDescriptor, &StatB));
    LxtCheckMemoryEqual(&StatA.st_atim, &StatB.st_atim, sizeof(StatA.st_atim));

    LxtCheckErrno(close(FileDescriptor));
    LxtCheckErrno(stat(SourceOpen, &StatB));
    LxtCheckMemoryEqual(&StatA.st_atim, &StatB.st_atim, sizeof(StatA.st_atim));

    FileDescriptor = -1;

    //
    // Test O_NOATIME for getdents no access time changes.
    //

    rmdir(LXT_GET_DENTS_FOLDER);

    //
    // Check the expected getdents results for each directory;
    //

    DentsBuffer = malloc(DentsBufferSize);
    if (DentsBuffer == NULL)
    {
        Result = LXT_RESULT_FAILURE;
        LxtLogError("malloc");
        goto ErrorExit;
    }

    LxtCheckErrno(mkdir(LXT_GET_DENTS_FOLDER, 0777));
    memset(DentsBuffer, 1, DentsBufferSize);
    LxtCheckErrno(FileDescriptor = open(SourceGetdents, O_RDONLY | O_DIRECTORY));

    LxtCheckErrno(fstat(FileDescriptor, &StatA));
    usleep(10 * 1000);
    LxtCheckErrno(Bytes = LxtGetdents64(FileDescriptor, DentsBuffer, DentsBufferSize));

    if (Bytes == 0)
    {
        Result = LXT_RESULT_FAILURE;
        LxtLogError("BytesRead == 0");
        goto ErrorExit;
    }

    LxtCheckErrno(fstat(FileDescriptor, &StatB));

    //
    // TODO_LX: Uncomment when the file system is mounted without noatime.
    //
    // if (memcmp(&StatA.st_atim, &StatB.st_atim, sizeof(StatA.st_atim)) == 0) {
    //     LxtLogError("Access time was supposed to be updated.");
    //     Result = -1;
    //     goto ErrorExit;
    // }
    //

    LxtCheckErrno(close(FileDescriptor));
    FileDescriptor = -1;
    LxtCheckErrno(FileDescriptor = open(SourceGetdents, O_RDONLY | O_DIRECTORY | O_NOATIME));

    usleep(100 * 1000);
    LxtCheckErrno(Bytes = LxtGetdents64(FileDescriptor, DentsBuffer, DentsBufferSize));

    if (Bytes == 0)
    {
        Result = LXT_RESULT_FAILURE;
        LxtLogError("BytesRead == 0");
        goto ErrorExit;
    }

    LxtCheckErrno(fstat(FileDescriptor, &StatA));
    LxtCheckMemoryEqual(&StatA.st_atim, &StatB.st_atim, sizeof(StatA.st_atim));

    LxtCheckClose(FileDescriptor);
    LxtCheckErrno(stat(SourceGetdents, &StatA));
    LxtCheckMemoryEqual(&StatA.st_atim, &StatB.st_atim, sizeof(StatA.st_atim));

    Result = LXT_RESULT_SUCCESS;

ErrorExit:

    if (FileDescriptor != -1)
    {
        close(FileDescriptor);
    }

    if (DentsBuffer != NULL)
    {
        free(DentsBuffer);
    }

    unlink(SourceOpen);
    rmdir(LXT_GET_DENTS_FOLDER);
    return Result;
}

int FsCommonTestWritev(PLXT_ARGS Args)

/*++
--*/

{

    //
    // This test doesn't pass on real Linux, so it's skipped for VM mode.
    //

    if (g_LxtFsInfo.FsType == LxtFsTypePlan9)
    {
        LxtLogInfo("Skipping writev test in VM mode.");
        return LXT_RESULT_SUCCESS;
    }

    return LxtFsWritevCommon(FS_TEST_DIR_PARENT "/fs_writev_test.bin");
}

int FsCommonTestDeviceId(PLXT_ARGS Args)

/*++

Description:

    This routine tests that mounts have unique device id's.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    dev_t DevDeviceId;
    dev_t ProcDeviceId;
    int Result;
    dev_t RootDeviceId;
    struct stat Stat;

    //
    // Test that various directories device id's are reported correctly.
    //

    LxtCheckErrnoZeroSuccess(stat("/", &Stat));
    RootDeviceId = Stat.st_dev;

    LxtCheckErrnoZeroSuccess(stat("/proc", &Stat));
    ProcDeviceId = Stat.st_dev;

    LxtCheckErrnoZeroSuccess(stat("/dev", &Stat));
    DevDeviceId = Stat.st_dev;

    LxtLogInfo("DeviceId's: / = %lld /proc = %lld /dev = %lld", RootDeviceId, ProcDeviceId, DevDeviceId);

    if ((RootDeviceId == ProcDeviceId) || (RootDeviceId == DevDeviceId) || (ProcDeviceId == DevDeviceId))
    {

        LxtLogError("Detected non-unique device id's");
    }

ErrorExit:
    return Result;
}

int FsCommonTestFallocate(PLXT_ARGS Args)

/*++

Description:

    This routine tests the fallocate system call.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int Fd;
    dev_t ProcDeviceId;
    int Result;
    dev_t RootDeviceId;
    struct stat Stat;

    unlink(FS_FALLOCATE_TEST_FILE);
    LxtCheckErrno(Fd = creat(FS_FALLOCATE_TEST_FILE, 0666));

    //
    // Plan 9 and virtiofs do not support fallocate.
    //

    if (g_LxtFsInfo.FsType == LxtFsTypePlan9 || g_LxtFsInfo.FsType == LxtFsTypeVirtioFs)
    {
        LxtCheckErrnoFailure(fallocate(Fd, 0, 0, 1024), ENOTSUP);
        LxtLogInfo("Fallocate is not supported on Plan 9.");
        goto ErrorExit;
    }

    //
    // Allocate some space.
    //

    LxtCheckErrnoZeroSuccess(fallocate(Fd, 0, 0, 1024));
    LxtCheckErrnoZeroSuccess(fstat(Fd, &Stat));
    LxtCheckEqual(Stat.st_size, 1024, "%lld");
    LxtCheckGreaterOrEqual(Stat.st_blocks, 2, "%ld");

    //
    // Don't change the length.
    //

    LxtCheckErrnoZeroSuccess(fallocate(Fd, FALLOC_FL_KEEP_SIZE, 0, 16384));
    LxtCheckErrnoZeroSuccess(fstat(Fd, &Stat));
    LxtCheckEqual(Stat.st_size, 1024, "%lld");
    LxtCheckGreaterOrEqual(Stat.st_blocks, 32, "%ld");

    //
    // Fallocate won't shrink the file.
    //

    LxtCheckErrnoZeroSuccess(fallocate(Fd, 0, 0, 512));
    LxtCheckErrnoZeroSuccess(fstat(Fd, &Stat));
    LxtCheckEqual(Stat.st_size, 1024, "%lld");
    LxtCheckGreaterOrEqual(Stat.st_blocks, 32, "%ld");

    //
    // Attempt to make the file very very large.
    //
    // N.B. On some machines with very large hard drives (larger than 1TB) this
    //      can succeed.
    //

    Result = fallocate(Fd, 0, 0, 0xffffffffff);
    if (Result < 0)
    {
        LxtCheckErrnoFailure(Result, ENOSPC);
    }

ErrorExit:
    unlink(FS_FALLOCATE_TEST_FILE);
    if (Fd != -1)
    {
        LxtClose(Fd);
    }

    return Result;
}

int FsCommonTestDirSeek(PLXT_ARGS Args)

/*++

Description:

    This routine tests the seek operation on directory.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int Result;
    LxtCheckResult(LxtFsDirSeekCommon(LXT_GET_DENTS_FOLDER));

ErrorExit:
    return Result;
}

int FsCommonTestFsync(PLXT_ARGS Args)

/*++

Description:

    This routine tests the fsync system call.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int Fd;
    int Result;
    struct stat St;

    Fd = -1;

    LxtCheckErrnoZeroSuccess(mkdir(FS_FSYNC_TEST_DIR, 0777));
    LxtCheckErrno(Fd = creat(FS_FSYNC_TEST_DIR "/testfile", 0666));
    LxtCheckErrnoZeroSuccess(fsync(Fd));
    LxtCheckClose(Fd);

    //
    // Open the file as read-only and attempt to call fsync on it.
    //

    LxtCheckErrno(Fd = open(FS_FSYNC_TEST_DIR "/testfile", O_RDONLY));
    LxtCheckErrnoZeroSuccess(fsync(Fd));
    LxtCheckClose(Fd);

    //
    // Create a file with no write access and call fsync on it.

    LxtCheckErrno(Fd = creat(FS_FSYNC_TEST_DIR "/testfile2", 0444));
    LxtCheckErrnoZeroSuccess(fstat(Fd, &St));
    LxtCheckEqual(St.st_mode, S_IFREG | 0444, "0%o");
    LxtCheckErrnoZeroSuccess(fsync(Fd));
    LxtCheckClose(Fd);

    //
    // Open that file as read-only and attempt to call fsync on it.
    //

    LxtCheckErrno(Fd = open(FS_FSYNC_TEST_DIR "/testfile2", O_RDONLY));
    LxtCheckErrnoZeroSuccess(fsync(Fd));
    LxtCheckClose(Fd);

ErrorExit:
    if (Fd >= 0)
    {
        close(Fd);
    }

    unlink(FS_FSYNC_TEST_DIR "/testfile2");
    unlink(FS_FSYNC_TEST_DIR "/testfile");
    rmdir(FS_FSYNC_TEST_DIR);
    return Result;
}
