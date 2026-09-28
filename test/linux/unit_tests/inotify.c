/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    inotify.c

Abstract:

    This file contains extensive inotify unit tests.

--*/

#include "lxtcommon.h"
#include "unittests.h"
#include "lxtfs.h"
#include <poll.h>
#include <dirent.h>
#include <sys/epoll.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/inotify.h>
#include <sys/mount.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <errno.h>

#define LXT_NAME "INOTIFY"

#define INOTIFY_TEST_BASE_DIR_LXFS "/data/inotify_test/"
#define INOTIFY_TEST_PROCFS_MAX_QUEUED_EVENTS_FILE "/proc/sys/fs/inotify/max_queued_events"

int TestInotifyComprehensive1Common(char* BaseDir);

int TestInotifyComprehensive2Common(char* BaseDir);

LXT_VARIATION_HANDLER TestInotifyNonBlockRead;
LXT_VARIATION_HANDLER TestInotifyEventQueueOverflow;
LXT_VARIATION_HANDLER TestInotifyEpollLxfs;
LXT_VARIATION_HANDLER TestInotifyBasicLxfs;
LXT_VARIATION_HANDLER TestInotifyComprehensive1Lxfs;
LXT_VARIATION_HANDLER TestInotifyComprehensive2Lxfs;
LXT_VARIATION_HANDLER TestInotifyPosixUnlinkRenameLxfs;
LXT_VARIATION_HANDLER TestInotifyUnmountBindLxfs;
LXT_VARIATION_HANDLER TestInotifyFtruncateLxfs;
LXT_VARIATION_HANDLER TestInotifyPseudoPlugin;

//
// Global constants
//

static const LXT_VARIATION g_LxtVariations[] = {
    {"Test non-blocking read of inotify descriptor", TestInotifyNonBlockRead},
    {"Test overflow of inotify event queue", TestInotifyEventQueueOverflow},
    {"Test inotify with epoll - lxfs", TestInotifyEpollLxfs},
    {"Test inotify watching basic paths - lxfs", TestInotifyBasicLxfs},
    {"Comprehensive inotify tests 1 - lxfs", TestInotifyComprehensive1Lxfs},
    {"Comprehensive inotify tests 2 - lxfs", TestInotifyComprehensive2Lxfs},
    {"Test inotify with POSIX unlink/rename - lxfs", TestInotifyPosixUnlinkRenameLxfs},
    {"Test unmounting of a bind mount - lxfs", TestInotifyUnmountBindLxfs},
    {"Test ftruncate - lxfs", TestInotifyFtruncateLxfs},
    {"Test inotify pseudo plugin", TestInotifyPseudoPlugin}};

int InotifyTestEntry(int Argc, char* Argv[])

/*++
--*/

{

    LXT_ARGS Args;
    int Result;

    LxtCheckResult(LxtInitialize(Argc, Argv, &Args, LXT_NAME));
    LxtCheckResult(LxtRunVariations(&Args, g_LxtVariations, LXT_COUNT_OF(g_LxtVariations)));

ErrorExit:
    LxtUninitialize();
    return !LXT_SUCCESS(Result);
}

int TestInotifyNonBlockRead(PLXT_ARGS Args)

{

    int Id;
    char Buf[10];
    int Result;

    //
    // There is nothing to read here, but should not block.
    //

    LxtCheckErrno(Id = inotify_init1(IN_NONBLOCK));
    LxtCheckErrnoFailure(Result = read(Id, Buf, 1), EAGAIN);
    LxtCheckErrnoZeroSuccess(close(Id));

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    return Result;
}

int TestInotifyEventQueueOverflow(PLXT_ARGS Args)

{

    int Id;
    int Fd;
    int ProcFd;
    char Buf[11];
    int Result;
    int OriginalMaxQueuedEvents;
    char InotifyBuf[500];
    struct inotify_event* Events[INOTIFY_TEST_EVENTS_BUF_SIZE];
    int NumEvents;
    char TestFile1[PATH_MAX];
    char TestFile2[PATH_MAX];
    char TestFile1Hlink[PATH_MAX];
    char TestFile1Slink[PATH_MAX];

    //
    // Initialize and also do cleanup if the files have not been removed.
    //

    sprintf(TestFile1, "%s%s", INOTIFY_TEST_BASE_DIR_LXFS, INOTIFY_TEST_FILE1_NAME_ONLY);

    sprintf(TestFile2, "%s%s", INOTIFY_TEST_BASE_DIR_LXFS, INOTIFY_TEST_FILE2_NAME_ONLY);

    sprintf(TestFile1Hlink, "%s%s", INOTIFY_TEST_BASE_DIR_LXFS, INOTIFY_TEST_FILE1_HLINK_NAME_ONLY);

    sprintf(TestFile1Slink, "%s%s", INOTIFY_TEST_BASE_DIR_LXFS, INOTIFY_TEST_FILE1_SLINK_NAME_ONLY);

    unlink(TestFile1);
    unlink(TestFile2);
    unlink(TestFile1Hlink);
    unlink(TestFile1Slink);
    rmdir(INOTIFY_TEST_BASE_DIR_LXFS);
    LxtCheckErrnoZeroSuccess(mkdir(INOTIFY_TEST_BASE_DIR_LXFS, 0777));
    LxtCheckErrno(Fd = creat(TestFile1, 0777));
    LxtCheckErrnoZeroSuccess(close(Fd));

    //
    // Read the procFS value /proc/sys/fs/inotify/max_queued_events.
    //

    LxtCheckErrno(ProcFd = open(INOTIFY_TEST_PROCFS_MAX_QUEUED_EVENTS_FILE, O_RDWR));

    LxtCheckErrno(Result = read(ProcFd, Buf, sizeof(Buf)));
    OriginalMaxQueuedEvents = atoi(Buf);
    LxtCheckNotEqual(OriginalMaxQueuedEvents, 0, "%d");

    //
    // Change the value to -1, verify failed.
    //

    sprintf(Buf, "-1");
    LxtCheckErrnoFailure(Result = write(ProcFd, Buf, strlen(Buf)), EINVAL);

    //
    // Change the value to INT_MAX + 1 (2^31), verify failed.
    //

    sprintf(Buf, "2147483648");
    LxtCheckErrnoFailure(Result = write(ProcFd, Buf, strlen(Buf)), EINVAL);

    //
    // Change the value to INT_MAX (2^31 - 1), verify succeeded.
    //

    sprintf(Buf, "2147483647");
    LxtCheckErrno(Result = write(ProcFd, Buf, strlen(Buf)));
    LxtCheckEqual(atoi(Buf), INT_MAX, "%d");

    //
    // Change the value to 2, and then read it back to verify.
    //

    sprintf(Buf, "2");
    LxtCheckErrno(Result = write(ProcFd, Buf, strlen(Buf)));
    LxtCheckErrno(Result = read(ProcFd, Buf, sizeof(Buf)));
    LxtCheckEqual(atoi(Buf), 2, "%d");

    //
    // Generate 2 inotify events, verify that there is no overflow.
    //

    LxtCheckErrno(Id = inotify_init());
    LxtCheckErrno(Result = inotify_add_watch(Id, TestFile1, IN_ALL_EVENTS));
    LxtCheckErrno(Fd = open(TestFile1, O_RDWR));
    LxtCheckErrnoZeroSuccess(close(Fd));
    LxtCheckErrno(LxtFsInotifyReadAndProcess(Id, InotifyBuf, sizeof(InotifyBuf), Events, INOTIFY_TEST_EVENTS_BUF_SIZE, &NumEvents, FALSE));

    LxtCheckEqual(NumEvents, 2, "%d");
    LxtCheckEqual(Events[0]->mask, IN_OPEN, "%d");
    LxtCheckEqual(Events[1]->mask, IN_CLOSE_WRITE, "%d");

    //
    // Generate 3 inotify events, verify that there is an overflow.
    //

    LxtCheckErrno(Fd = open(TestFile1, O_RDWR));
    LxtCheckErrnoZeroSuccess(fchmod(Fd, 0666));
    LxtCheckErrnoZeroSuccess(fchmod(Fd, 0777));
    LxtCheckErrnoZeroSuccess(close(Fd));
    LxtCheckErrno(LxtFsInotifyReadAndProcess(Id, InotifyBuf, sizeof(InotifyBuf), Events, INOTIFY_TEST_EVENTS_BUF_SIZE, &NumEvents, FALSE));

    LxtCheckEqual(NumEvents, 3, "%d");
    LxtCheckEqual(Events[0]->mask, IN_OPEN, "%d");
    LxtCheckEqual(Events[1]->mask, IN_ATTRIB, "%d");
    LxtCheckEqual(Events[2]->mask, IN_Q_OVERFLOW, "%d");
    LxtCheckEqual(Events[2]->wd, -1, "%d");
    LxtCheckEqual(Events[2]->cookie, 0, "%d");
    LxtCheckEqual(Events[1]->len, 0, "%d");

    //
    // Restore the max_queued_events value to the original value read
    // in the beginning, and verify.
    //

    sprintf(Buf, "%d", OriginalMaxQueuedEvents);
    LxtCheckErrno(Result = write(ProcFd, Buf, strlen(Buf)));
    LxtCheckErrno(Result = read(ProcFd, Buf, sizeof(Buf)));
    LxtCheckEqual(atoi(Buf), OriginalMaxQueuedEvents, "%d");

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    close(Id);
    close(Fd);
    close(ProcFd);
    unlink(TestFile1);
    unlink(TestFile2);
    unlink(TestFile1Hlink);
    unlink(TestFile1Slink);
    rmdir(INOTIFY_TEST_BASE_DIR_LXFS);
    return Result;
}

int TestInotifyEpollLxfs(PLXT_ARGS Args)

{

    return LxtFsInotifyEpollCommon(INOTIFY_TEST_BASE_DIR_LXFS);
}

int TestInotifyBasicLxfs(PLXT_ARGS Args)

{

    int Id;
    int Result;
    int Wd[10];

    //
    // Test watching basic Lxfs paths.
    //

    LxtCheckErrno(Id = inotify_init());
    LxtCheckErrno(Wd[0] = inotify_add_watch(Id, "/", IN_ALL_EVENTS));
    LxtCheckErrno(Wd[1] = inotify_add_watch(Id, "/mnt", IN_ALL_EVENTS));
    LxtCheckErrno(Wd[2] = inotify_add_watch(Id, "/mnt/", IN_ALL_EVENTS));
    LxtCheckErrno(Wd[3] = inotify_add_watch(Id, "/proc", IN_ALL_EVENTS));
    LxtCheckErrno(Wd[4] = inotify_add_watch(Id, "/sys", IN_ALL_EVENTS));
    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    close(Id);
    return Result;
}

int TestInotifyComprehensive1Lxfs(PLXT_ARGS Args)

{

    return TestInotifyComprehensive1Common(INOTIFY_TEST_BASE_DIR_LXFS);
}

int TestInotifyComprehensive1Common(char* BaseDir)

/*++
--*/

{

    int Fd;
    int Id1;
    int Id2;
    int Wd[10];
    char Buf[10];
    char InotifyBuf[500];
    struct inotify_event* Events[INOTIFY_TEST_EVENTS_BUF_SIZE];
    int NumEvents;
    int Bytes;
    int Result;
    int Index;
    int AttribEvent;
    int CreateEvent;
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

    //
    // Setup inotify.
    //

    LxtCheckErrno(Id1 = inotify_init());
    LxtCheckErrno(Id2 = inotify_init());
    LxtCheckErrno(Wd[0] = inotify_add_watch(Id1, TestFile1, IN_ALL_EVENTS));

    //
    // Check that "output params" can also be specified as input.
    //

    LxtCheckErrno(Wd[1] = inotify_add_watch(Id1, BaseDir, IN_ALL_EVENTS | IN_IGNORED | IN_ISDIR | IN_Q_OVERFLOW | IN_UNMOUNT));

    LxtCheckEqual(Wd[0], 1, "%d");
    LxtCheckEqual(Wd[1], 2, "%d");

    //
    // Test IN_OPEN, IN_ATTRIB, IN_MODIFY, IN_ACCESS, IN_CLOSE_WRITE.
    //

    LxtCheckErrno(Fd = open(TestFile1, O_RDWR));
    LxtCheckErrnoZeroSuccess(fchmod(Fd, 0666));
    LxtCheckErrno(Bytes = write(Fd, Buf, 10));
    LxtCheckEqual(Bytes, 10, "%d");
    LxtCheckErrno(Bytes = write(Fd, Buf, 10));
    LxtCheckEqual(Bytes, 10, "%d");
    LxtCheckErrnoZeroSuccess(lseek(Fd, 0, SEEK_SET));
    LxtCheckErrno(Bytes = read(Fd, Buf, 10));
    LxtCheckEqual(Bytes, 10, "%d");
    LxtCheckErrno(Bytes = read(Fd, Buf, 10));
    LxtCheckEqual(Bytes, 10, "%d");
    LxtCheckErrnoZeroSuccess(fchmod(Fd, 0777));
    LxtCheckErrnoZeroSuccess(close(Fd));

    //
    // Verify.
    //

    LxtCheckErrno(LxtFsInotifyReadAndProcess(Id1, InotifyBuf, sizeof(InotifyBuf), Events, INOTIFY_TEST_EVENTS_BUF_SIZE, &NumEvents, FALSE));

    LxtCheckEqual(NumEvents, 16, "%d");
    for (Index = 0; Index < 2; Index++)
    {
        LxtCheckEqual(Events[0 + Index]->mask, IN_OPEN, "%d");
        LxtCheckEqual(Events[2 + Index]->mask, IN_ATTRIB, "%d");
        LxtCheckEqual(Events[4 + Index]->mask, IN_MODIFY, "%d");
        LxtCheckEqual(Events[6 + Index]->mask, IN_MODIFY, "%d");
        LxtCheckEqual(Events[8 + Index]->mask, IN_ACCESS, "%d");
        LxtCheckEqual(Events[10 + Index]->mask, IN_ACCESS, "%d");
        LxtCheckEqual(Events[12 + Index]->mask, IN_ATTRIB, "%d");
        LxtCheckEqual(Events[14 + Index]->mask, IN_CLOSE_WRITE, "%d");
    }

    for (Index = 0; Index < NumEvents; Index++)
    {
        LxtCheckEqual(Events[Index]->cookie, 0, "%d");
        if ((Index % 2) == 0)
        {

            //
            // The parent directory.
            //

            LxtCheckEqual(Events[Index]->wd, 2, "%d");
            LxtCheckTrue(strcmp(Events[Index]->name, INOTIFY_TEST_FILE1_NAME_ONLY) == 0);
            LxtCheckNotEqual(Events[Index]->len, 0, "%d");
        }
        else
        {

            //
            // The file (child).
            //

            LxtCheckEqual(Events[Index]->wd, 1, "%d");
            LxtCheckEqual(Events[Index]->len, 0, "%d");
        }
    }

    //
    // Test IN_CLOSE_NOWRITE.
    //

    LxtCheckErrno(Fd = open(TestFile1, O_RDONLY));
    LxtCheckErrno(Bytes = read(Fd, Buf, 10));
    LxtCheckEqual(Bytes, 10, "%d");
    LxtCheckErrno(Bytes = read(Fd, Buf, 10));
    LxtCheckEqual(Bytes, 10, "%d");
    LxtCheckErrnoZeroSuccess(close(Fd));

    //
    // Verify.
    //

    LxtCheckErrno(LxtFsInotifyReadAndProcess(Id1, InotifyBuf, sizeof(InotifyBuf), Events, INOTIFY_TEST_EVENTS_BUF_SIZE, &NumEvents, FALSE));

    LxtCheckEqual(NumEvents, 8, "%d");

    //
    // Test that opening an existing file with O_TRUNC generates IN_MODIFY,
    // even if the open is for read-only access.
    //

    LxtCheckErrno(Fd = open(TestFile1, O_RDONLY | O_TRUNC));
    LxtCheckErrnoZeroSuccess(close(Fd));

    //
    // Verify.
    //

    LxtCheckErrno(LxtFsInotifyReadAndProcess(Id1, InotifyBuf, sizeof(InotifyBuf), Events, INOTIFY_TEST_EVENTS_BUF_SIZE, &NumEvents, FALSE));

    LxtCheckEqual(NumEvents, 6, "%d");
    LxtCheckEqual(Events[0]->mask, IN_MODIFY, "%d");
    LxtCheckEqual(Events[1]->mask, IN_MODIFY, "%d");
    LxtCheckEqual(Events[2]->mask, IN_OPEN, "%d");
    LxtCheckEqual(Events[3]->mask, IN_OPEN, "%d");
    LxtCheckEqual(Events[4]->mask, IN_CLOSE_NOWRITE, "%d");
    LxtCheckEqual(Events[5]->mask, IN_CLOSE_NOWRITE, "%d");

    //
    // Test that opening an existing file with only O_PATH generates IN_OPEN.
    //

    LxtCheckErrno(Fd = open(TestFile1, O_PATH));
    LxtCheckErrnoZeroSuccess(close(Fd));

    //
    // Verify.
    //

    LxtCheckErrno(LxtFsInotifyReadAndProcess(Id1, InotifyBuf, sizeof(InotifyBuf), Events, INOTIFY_TEST_EVENTS_BUF_SIZE, &NumEvents, FALSE));

    LxtCheckEqual(NumEvents, 4, "%d");
    LxtCheckEqual(Events[0]->mask, IN_OPEN, "%d");
    LxtCheckEqual(Events[1]->mask, IN_OPEN, "%d");
    LxtCheckEqual(Events[2]->mask, IN_CLOSE_NOWRITE, "%d");
    LxtCheckEqual(Events[3]->mask, IN_CLOSE_NOWRITE, "%d");

    //
    // Test IN_MOVED_FROM, IN_MOVED_TO, IN_MOVE_SELF
    // (rename with no overwrite).
    //

    LxtCheckErrnoZeroSuccess(rename(TestFile1, TestFile2));
    LxtCheckErrnoZeroSuccess(rename(TestFile2, TestFile1));

    //
    // Verify.
    //

    LxtCheckErrno(LxtFsInotifyReadAndProcess(Id1, InotifyBuf, sizeof(InotifyBuf), Events, INOTIFY_TEST_EVENTS_BUF_SIZE, &NumEvents, FALSE));

    LxtCheckEqual(NumEvents, 6, "%d");
    LxtCheckEqual(Events[0]->mask, IN_MOVED_FROM, "%d");
    LxtCheckEqual(Events[1]->mask, IN_MOVED_TO, "%d");
    LxtCheckEqual(Events[2]->mask, IN_MOVE_SELF, "%d");
    LxtCheckEqual(Events[0]->cookie, Events[1]->cookie, "%d");
    LxtCheckTrue(strcmp(Events[0]->name, INOTIFY_TEST_FILE1_NAME_ONLY) == 0);
    LxtCheckTrue(strcmp(Events[1]->name, INOTIFY_TEST_FILE2_NAME_ONLY) == 0);

    //
    // Test IN_DELETE and IN_DELETE_SELF.
    //

    LxtCheckErrnoZeroSuccess(unlink(TestFile1));

    //
    // Verify.
    //

    LxtCheckErrno(LxtFsInotifyReadAndProcess(Id1, InotifyBuf, sizeof(InotifyBuf), Events, INOTIFY_TEST_EVENTS_BUF_SIZE, &NumEvents, FALSE));

    LxtCheckEqual(NumEvents, 4, "%d");
    LxtCheckEqual(Events[0]->mask, IN_ATTRIB, "%d");
    LxtCheckEqual(Events[1]->mask, IN_DELETE_SELF, "%d");
    LxtCheckEqual(Events[2]->mask, IN_IGNORED, "%d");
    LxtCheckEqual(Events[3]->mask, IN_DELETE, "%d");
    LxtCheckEqual(Events[0]->wd, 1, "%d");
    LxtCheckEqual(Events[1]->wd, 1, "%d");
    LxtCheckEqual(Events[2]->wd, 1, "%d");
    LxtCheckEqual(Events[3]->wd, 2, "%d");
    //
    // Test IN_CREATE, and that inotify_rm_watch() generates IN_IGNORED.
    //

    LxtCheckErrno(Fd = creat(TestFile1, 0777));
    LxtCheckErrnoZeroSuccess(close(Fd));
    LxtCheckErrno(Wd[0] = inotify_add_watch(Id1, TestFile1, IN_ALL_EVENTS));
    LxtCheckErrnoZeroSuccess(inotify_rm_watch(Id1, Wd[0]));
    LxtCheckErrnoZeroSuccess(inotify_rm_watch(Id1, Wd[1]));

    //
    // Verify.
    //

    LxtCheckErrno(LxtFsInotifyReadAndProcess(Id1, InotifyBuf, sizeof(InotifyBuf), Events, INOTIFY_TEST_EVENTS_BUF_SIZE, &NumEvents, FALSE));

    LxtCheckEqual(NumEvents, 5, "%d");
    LxtCheckEqual(Events[0]->mask, IN_CREATE, "%d");
    LxtCheckEqual(Events[1]->mask, IN_OPEN, "%d");
    LxtCheckEqual(Events[2]->mask, IN_CLOSE_WRITE, "%d");
    LxtCheckEqual(Events[3]->mask, IN_IGNORED, "%d");
    LxtCheckEqual(Events[4]->mask, IN_IGNORED, "%d");
    LxtCheckEqual(Events[0]->wd, 2, "%d");
    LxtCheckEqual(Events[1]->wd, 2, "%d");
    LxtCheckEqual(Events[2]->wd, 2, "%d");
    LxtCheckEqual(Events[3]->wd, 3, "%d");
    LxtCheckEqual(Events[4]->wd, 2, "%d");

    //
    // Test that IN_ONESHOT generates only one event and then IN_IGNORED.
    //

    LxtCheckErrno(Wd[0] = inotify_add_watch(Id1, TestFile1, IN_ALL_EVENTS | IN_ONESHOT));
    LxtCheckErrno(Fd = open(TestFile1, O_RDONLY));
    LxtCheckErrnoZeroSuccess(close(Fd));

    //
    // Verify.
    //

    LxtCheckErrno(LxtFsInotifyReadAndProcess(Id1, InotifyBuf, sizeof(InotifyBuf), Events, INOTIFY_TEST_EVENTS_BUF_SIZE, &NumEvents, FALSE));

    LxtCheckEqual(NumEvents, 2, "%d");
    LxtCheckEqual(Events[0]->mask, IN_OPEN, "%d");
    LxtCheckEqual(Events[1]->mask, IN_IGNORED, "%d");

    //
    // Test IN_ONLYDIR on file, should fail.
    //

    LxtCheckErrnoFailure(Wd[1] = inotify_add_watch(Id1, TestFile1, IN_ALL_EVENTS | IN_ONLYDIR), ENOTDIR);

    //
    // Test operations on directories.
    //

    LxtCheckErrno(Wd[1] = inotify_add_watch(Id1, BaseDir, IN_ALL_EVENTS | IN_ONLYDIR));

    LxtCheckErrno(Fd = open(BaseDir, O_RDONLY));
    LxtCheckErrnoZeroSuccess(fchmod(Fd, 0666));
    LxtCheckErrnoZeroSuccess(fchmod(Fd, 0777));
    LxtCheckErrnoZeroSuccess(close(Fd));

    //
    // Verify.
    //

    LxtCheckErrno(LxtFsInotifyReadAndProcess(Id1, InotifyBuf, sizeof(InotifyBuf), Events, INOTIFY_TEST_EVENTS_BUF_SIZE, &NumEvents, FALSE));

    LxtCheckEqual(NumEvents, 3, "%d");
    LxtCheckEqual(Events[0]->mask, IN_OPEN | IN_ISDIR, "%d");
    LxtCheckEqual(Events[1]->mask, IN_ATTRIB | IN_ISDIR, "%d");
    LxtCheckEqual(Events[2]->mask, IN_CLOSE_NOWRITE | IN_ISDIR, "%d");

    //
    // Test creating a symbolic link.
    //

    LxtCheckErrno(Wd[0] = inotify_add_watch(Id1, TestFile1, IN_ALL_EVENTS));
    LxtCheckErrnoZeroSuccess(symlink(TestFile1, TestFile1Slink));

    //
    // Verify.
    //

    LxtCheckErrno(LxtFsInotifyReadAndProcess(Id1, InotifyBuf, sizeof(InotifyBuf), Events, INOTIFY_TEST_EVENTS_BUF_SIZE, &NumEvents, FALSE));

    LxtCheckEqual(NumEvents, 1, "%d");
    LxtCheckEqual(Events[0]->mask, IN_CREATE, "%d");
    LxtCheckEqual(Events[0]->wd, 5, "%d");
    LxtCheckTrue(strcmp(Events[0]->name, INOTIFY_TEST_FILE1_SLINK_NAME_ONLY) == 0);

    //
    // Test creating a hard link.
    //

    LxtCheckErrnoZeroSuccess(link(TestFile1, TestFile1Hlink));

    //
    // Verify. Note that Ubuntu generates 2 events, whereas WSL generates 4 events.
    // This is due to WSL performing unnecessary file opens, which will be fixed
    // in the future. Also, the ordering of the events differs between Ubuntu and WSL.
    //

    LxtCheckErrno(LxtFsInotifyReadAndProcess(Id1, InotifyBuf, sizeof(InotifyBuf), Events, INOTIFY_TEST_EVENTS_BUF_SIZE, &NumEvents, FALSE));

    LxtCheckTrue((NumEvents == 2) || (NumEvents == 4));
    if (Events[0]->mask == IN_ATTRIB)
    {
        AttribEvent = 0;
        CreateEvent = 1;
    }
    else
    {
        AttribEvent = 1;
        CreateEvent = 0;
    }

    LxtCheckEqual(Events[AttribEvent]->mask, IN_ATTRIB, "%d");
    LxtCheckEqual(Events[AttribEvent]->wd, 6, "%d");
    LxtCheckEqual(Events[CreateEvent]->mask, IN_CREATE, "%d");
    LxtCheckEqual(Events[CreateEvent]->wd, 5, "%d");
    LxtCheckTrue(strcmp(Events[CreateEvent]->name, INOTIFY_TEST_FILE1_HLINK_NAME_ONLY) == 0);

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    close(Id1);
    close(Id2);
    close(Fd);
    unlink(TestFile1);
    unlink(TestFile2);
    unlink(TestFile1Hlink);
    unlink(TestFile1Slink);
    rmdir(BaseDir);
    return Result;
}

int TestInotifyComprehensive2Lxfs(PLXT_ARGS Args)

{

    return TestInotifyComprehensive2Common(INOTIFY_TEST_BASE_DIR_LXFS);
}

int TestInotifyComprehensive2Common(char* BaseDir)

/*++
--*/

{

    int Fd;
    int Id1;
    int Id2;
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

    //
    // Setup inotify.
    //

    LxtCheckErrno(Id1 = inotify_init());
    LxtCheckErrno(Id2 = inotify_init());

    //
    // Test IN_EXCL_UNLINK on both the directory and the file to be unlinked.
    // Also test deleting a file that has open handles to it.
    //

    LxtCheckErrno(
        Wd[0] = // wd: 1
        inotify_add_watch(Id1, TestFile1, IN_ALL_EVENTS | IN_EXCL_UNLINK));

    LxtCheckErrno(
        Wd[1] = // wd: 2
        inotify_add_watch(Id1, BaseDir, IN_ALL_EVENTS | IN_EXCL_UNLINK));

    LxtCheckErrno(Fd = open(TestFile1, O_RDWR));
    unlink(TestFile1);
    LxtCheckErrno(Bytes = write(Fd, Buf, 10));
    LxtCheckErrnoZeroSuccess(close(Fd));

    //
    // Verify. Note that the write and close on the unlinked file did not
    // generate any events on either the directory or the file since the
    // IN_EXCL_UNLINK flag was set on both.
    //

    LxtCheckErrno(LxtFsInotifyReadAndProcess(Id1, InotifyBuf, sizeof(InotifyBuf), Events, INOTIFY_TEST_EVENTS_BUF_SIZE, &NumEvents, FALSE));

    LxtCheckEqual(NumEvents, 6, "%d");
    LxtCheckEqual(Wd[0], 1, "%d");
    LxtCheckEqual(Wd[1], 2, "%d");
    LxtCheckEqual(Events[0]->mask, IN_OPEN, "%d");
    LxtCheckEqual(Events[1]->mask, IN_OPEN, "%d");
    LxtCheckEqual(Events[2]->mask, IN_ATTRIB, "%d");
    LxtCheckEqual(Events[3]->mask, IN_DELETE, "%d");
    LxtCheckEqual(Events[4]->mask, IN_DELETE_SELF, "%d");
    LxtCheckEqual(Events[5]->mask, IN_IGNORED, "%d");
    LxtCheckEqual(Events[0]->wd, 2, "%d");
    LxtCheckEqual(Events[1]->wd, 1, "%d");
    LxtCheckEqual(Events[2]->wd, 1, "%d");
    LxtCheckEqual(Events[3]->wd, 2, "%d");
    LxtCheckEqual(Events[4]->wd, 1, "%d");
    LxtCheckEqual(Events[5]->wd, 1, "%d");

    //
    // Test IN_EXCL_UNLINK on the directory only.
    //

    LxtCheckErrno(Fd = creat(TestFile1, 0777));
    LxtCheckErrnoZeroSuccess(close(Fd));
    LxtCheckErrnoZeroSuccess(close(Id1));
    LxtCheckErrno(Id1 = inotify_init());
    LxtCheckErrno(
        Wd[0] = // wd: 1
        inotify_add_watch(Id1, TestFile1, IN_ALL_EVENTS));

    LxtCheckErrno(
        Wd[1] = // wd: 2
        inotify_add_watch(Id1, BaseDir, IN_ALL_EVENTS | IN_EXCL_UNLINK));

    LxtCheckErrno(Fd = open(TestFile1, O_RDWR));
    unlink(TestFile1);
    LxtCheckErrno(Bytes = write(Fd, Buf, 10));
    LxtCheckErrnoZeroSuccess(close(Fd));

    //
    // Verify. Note that the file events are still generated even though it was
    // unlinked, because the file does not have the IN_EXCL_UNLINK flag set.
    //

    LxtCheckErrno(LxtFsInotifyReadAndProcess(Id1, InotifyBuf, sizeof(InotifyBuf), Events, INOTIFY_TEST_EVENTS_BUF_SIZE, &NumEvents, FALSE));

    LxtCheckEqual(NumEvents, 8, "%d");
    LxtCheckEqual(Wd[0], 1, "%d");
    LxtCheckEqual(Wd[1], 2, "%d");
    LxtCheckEqual(Events[0]->mask, IN_OPEN, "%d");
    LxtCheckEqual(Events[1]->mask, IN_OPEN, "%d");
    LxtCheckEqual(Events[2]->mask, IN_ATTRIB, "%d");
    LxtCheckEqual(Events[3]->mask, IN_DELETE, "%d");
    LxtCheckEqual(Events[4]->mask, IN_MODIFY, "%d");
    LxtCheckEqual(Events[5]->mask, IN_CLOSE_WRITE, "%d");
    LxtCheckEqual(Events[6]->mask, IN_DELETE_SELF, "%d");
    LxtCheckEqual(Events[7]->mask, IN_IGNORED, "%d");
    LxtCheckEqual(Events[0]->wd, 2, "%d");
    LxtCheckEqual(Events[1]->wd, 1, "%d");
    LxtCheckEqual(Events[2]->wd, 1, "%d");
    LxtCheckEqual(Events[3]->wd, 2, "%d");
    LxtCheckEqual(Events[4]->wd, 1, "%d");
    LxtCheckEqual(Events[5]->wd, 1, "%d");
    LxtCheckEqual(Events[6]->wd, 1, "%d");
    LxtCheckEqual(Events[7]->wd, 1, "%d");

    //
    // Test IN_EXCL_UNLINK on the unlinked file only.
    //

    LxtCheckErrno(Fd = creat(TestFile1, 0777));
    LxtCheckErrnoZeroSuccess(close(Fd));
    LxtCheckErrnoZeroSuccess(close(Id1));
    LxtCheckErrno(Id1 = inotify_init());
    LxtCheckErrno(
        Wd[0] = // wd: 1
        inotify_add_watch(Id1, TestFile1, IN_ALL_EVENTS | IN_EXCL_UNLINK));

    LxtCheckErrno(
        Wd[1] = // wd: 2
        inotify_add_watch(Id1, BaseDir, IN_ALL_EVENTS));

    LxtCheckErrno(Fd = open(TestFile1, O_RDWR));
    unlink(TestFile1);
    LxtCheckErrno(Bytes = write(Fd, Buf, 10));
    LxtCheckErrnoZeroSuccess(close(Fd));

    //
    // Verify. Note that the directory still receives the events from the unlinked
    // child, because the directory does not have the IN_EXCL_UNLINK flag set.
    //

    LxtCheckErrno(LxtFsInotifyReadAndProcess(Id1, InotifyBuf, sizeof(InotifyBuf), Events, INOTIFY_TEST_EVENTS_BUF_SIZE, &NumEvents, FALSE));

    LxtCheckEqual(NumEvents, 8, "%d");
    LxtCheckEqual(Wd[0], 1, "%d");
    LxtCheckEqual(Wd[1], 2, "%d");
    LxtCheckEqual(Events[0]->mask, IN_OPEN, "%d");
    LxtCheckEqual(Events[1]->mask, IN_OPEN, "%d");
    LxtCheckEqual(Events[2]->mask, IN_ATTRIB, "%d");
    LxtCheckEqual(Events[3]->mask, IN_DELETE, "%d");
    LxtCheckEqual(Events[4]->mask, IN_MODIFY, "%d");
    LxtCheckEqual(Events[5]->mask, IN_CLOSE_WRITE, "%d");
    LxtCheckEqual(Events[6]->mask, IN_DELETE_SELF, "%d");
    LxtCheckEqual(Events[7]->mask, IN_IGNORED, "%d");
    LxtCheckEqual(Events[0]->wd, 2, "%d");
    LxtCheckEqual(Events[1]->wd, 1, "%d");
    LxtCheckEqual(Events[2]->wd, 1, "%d");
    LxtCheckEqual(Events[3]->wd, 2, "%d");
    LxtCheckEqual(Events[4]->wd, 2, "%d");
    LxtCheckEqual(Events[5]->wd, 2, "%d");
    LxtCheckEqual(Events[6]->wd, 1, "%d");
    LxtCheckEqual(Events[7]->wd, 1, "%d");

    //
    // Test watching the same file twice.
    //

    LxtCheckErrno(Fd = creat(TestFile1, 0777));
    LxtCheckErrnoZeroSuccess(close(Fd));
    LxtCheckErrnoZeroSuccess(close(Id1));
    LxtCheckErrno(Id1 = inotify_init());
    LxtCheckErrno(
        Wd[0] = // wd: 1
        inotify_add_watch(Id1, TestFile1, IN_OPEN));

    LxtCheckErrno(
        Wd[1] = // wd: 1
        inotify_add_watch(Id1, TestFile1, IN_CLOSE));

    LxtCheckErrno(Fd = open(TestFile1, O_WRONLY));
    LxtCheckErrnoZeroSuccess(close(Fd));

    //
    // Verify that IN_CLOSE_WRITE is received, and that IN_OPEN is not received.
    //

    LxtCheckErrno(LxtFsInotifyReadAndProcess(Id1, InotifyBuf, sizeof(InotifyBuf), Events, INOTIFY_TEST_EVENTS_BUF_SIZE, &NumEvents, FALSE));

    LxtCheckEqual(NumEvents, 1, "%d");
    LxtCheckEqual(Events[0]->mask, IN_CLOSE_WRITE, "%d");
    LxtCheckEqual(Events[0]->wd, 1, "%d");
    LxtCheckEqual(Wd[0], Wd[1], "%d");

    //
    // Test watching the same file twice, but with IN_MASK_ADD.
    //

    LxtCheckErrno(Fd = creat(TestFile1, 0777));
    LxtCheckErrnoZeroSuccess(close(Fd));
    LxtCheckErrnoZeroSuccess(close(Id1));
    LxtCheckErrno(Id1 = inotify_init());
    LxtCheckErrno(
        Wd[0] = // wd: 1
        inotify_add_watch(Id1, TestFile1, IN_OPEN));

    LxtCheckErrno(
        Wd[1] = // wd: 1
        inotify_add_watch(Id1, TestFile1, IN_CLOSE | IN_MASK_ADD));

    LxtCheckErrno(Fd = open(TestFile1, O_RDONLY));
    LxtCheckErrnoZeroSuccess(close(Fd));

    //
    // Verify that both IN_OPEN and IN_CLOSE_NOWRITE are received.
    //

    LxtCheckErrno(LxtFsInotifyReadAndProcess(Id1, InotifyBuf, sizeof(InotifyBuf), Events, INOTIFY_TEST_EVENTS_BUF_SIZE, &NumEvents, FALSE));

    LxtCheckEqual(NumEvents, 2, "%d");
    LxtCheckEqual(Events[0]->mask, IN_OPEN, "%d");
    LxtCheckEqual(Events[1]->mask, IN_CLOSE_NOWRITE, "%d");
    LxtCheckEqual(Events[0]->wd, 1, "%d");
    LxtCheckEqual(Events[1]->wd, 1, "%d");
    LxtCheckEqual(Wd[0], Wd[1], "%d");

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    close(Id1);
    close(Id2);
    close(Fd);
    unlink(TestFile1);
    rmdir(BaseDir);
    return Result;
}

int TestInotifyPosixUnlinkRenameLxfs(PLXT_ARGS Args)

{

    return LxtFsInotifyPosixUnlinkRenameCommon(INOTIFY_TEST_BASE_DIR_LXFS);
}

int TestInotifyUnmountBindLxfs(PLXT_ARGS Args)

{

    return LxtFsInotifyUnmountBindCommon(INOTIFY_TEST_BASE_DIR_LXFS);
}

int TestInotifyFtruncateLxfs(PLXT_ARGS Args)

{

    int ChildPid;
    int Fd;
    struct pollfd PollFd;
    int Result;
    char TestFile1[PATH_MAX];

    ChildPid = -1;
    Fd = -1;

    //
    // Initialize and also do cleanup if the files have not been removed.
    //

    sprintf(TestFile1, "%s%s", INOTIFY_TEST_BASE_DIR_LXFS, INOTIFY_TEST_FILE1_NAME_ONLY);

    unlink(TestFile1);
    rmdir(INOTIFY_TEST_BASE_DIR_LXFS);
    LxtCheckErrnoZeroSuccess(mkdir(INOTIFY_TEST_BASE_DIR_LXFS, 0777));
    LxtCheckErrno(Fd = creat(TestFile1, 0777));
    LxtCheckErrno(ftruncate(Fd, 1024));
    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        sleep(2);
        LxtCheckErrno(ftruncate(Fd, 1024));
        fsync(Fd);
        goto ErrorExit;
    }

    int Id = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    LxtCheckErrno(inotify_add_watch(Id, TestFile1, IN_ALL_EVENTS));
    PollFd.fd = Id;
    PollFd.events = POLLIN;
    LxtCheckErrno(ppoll(&PollFd, 1, NULL, NULL));
    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    if (Fd != -1)
    {
        LxtClose(Fd);
    }

    if (ChildPid == 0)
    {
        _exit(Result);
    }

    return Result;
}

int TestInotifyPseudoPlugin(PLXT_ARGS Args)

{

    int Id;
    int Result;

    LxtCheckErrno(Id = inotify_init());
    LxtCheckErrno(inotify_add_watch(Id, "/proc/self/ns/pid", IN_ALL_EVENTS));

ErrorExit:
    if (Id != -1)
    {
        LxtClose(Id);
    }

    return Result;
}
