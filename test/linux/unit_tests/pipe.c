/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    pipe.c

Abstract:

    This is the test for pipes.

--*/

#include "lxtcommon.h"
#include "unittests.h"
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/fsuid.h>
#include <sys/cdefs.h>
#include <sys/prctl.h>
#include <linux/capability.h>
#include <limits.h>

#define LXT_NAME "Pipe"

#define min(_a, _b) ((_a) < (_b) ? (_a) : (_b))

#define _4KB (4 * 1024)

#define _32KB (32 * 1024)

#define _64KB (64 * 1024)

#define _1MB (1024 * 1024)

#define PIPE_DEFAULT_MAX_SIZE 1048576

#define ROUND_TO_PAGES(Size) (((unsigned long int)(Size) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))

#define PIPE_BUFFER_LENGTH (_1MB / sizeof(unsigned int))

#define PIPE_LOOPS (8)

#define PIPE_FIFO "/data/testfifo"
#define PIPE_FIFO_MESSAGE "Hello World!"

//
// Use /dev/fd, rather than /proc/self/fd directly, because that's what bash
// uses for process substitution.
//

#define PIPE_FD_PATH_FORMAT "/dev/fd/%d"
#define PIPE_CHILD_PATH_FORMAT "%s/%s"

int PipeCheckFdPath(int Fd);

LXT_VARIATION_HANDLER PipeEpoll;

LXT_VARIATION_HANDLER PipeFifo;

LXT_VARIATION_HANDLER PipeFifoNonBlock;

LXT_VARIATION_HANDLER PipeFifoReadWrite;

LXT_VARIATION_HANDLER PipeFifoReopen;

LXT_VARIATION_HANDLER PipeReopen;

LXT_VARIATION_HANDLER PipeStat;

LXT_VARIATION_HANDLER PipeFileLocking;

LXT_VARIATION_HANDLER PipeSecurity;

LXT_VARIATION_HANDLER PipeFcntl;

int PipeReader(unsigned int* Buffer, size_t BufferLength, int Pipe, bool Polling);

int PipeReaderHangup(PLXT_ARGS Args);

int PipeWriterHangup(PLXT_ARGS Args);

int PipeTest(bool Polling, bool UsePipe2);

int PipeVariation0(PLXT_ARGS Args);

int PipeVariation1(PLXT_ARGS Args);

int PipeVariationIoctl(PLXT_ARGS Args);

int PipeWriter(unsigned int* Buffer, size_t BufferLength, int Pipe, bool Polling);

int PipeZeroByteRead(PLXT_ARGS Args);

//
// Global constants
//

static const LXT_VARIATION g_LxtVariations[] = {
    {"Pipe0", PipeVariation0},
    {"Pipe1", PipeVariation1},
    {"Pipe reader hangup", PipeReaderHangup},
    {"Pipe writer hangup", PipeWriterHangup},
    {"Pipe ioctls", PipeVariationIoctl},
    {"Pipe - epoll", PipeEpoll},
    {"Pipe - Fifo", PipeFifo},
    {"Pipe - Fifo O_NONBLOCK", PipeFifoNonBlock},
    {"Pipe - Fifo O_RDWR", PipeFifoReadWrite},
    {"Pipe - Fifo re-open", PipeFifoReopen},
    {"Pipe - fstat", PipeStat},
    {"Pipe - File locking", PipeFileLocking},
    {"Pipe - /proc/self/fd reopen", PipeReopen},
    {"Pipe - Zero byte read", PipeZeroByteRead},
    {"Pipe - security attributes", PipeSecurity},
    {"Pipe - fcntl", PipeFcntl}};

int PipeTestEntry(int Argc, char* Argv[])

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

int PipeCheckFdPath(int Fd)

/*++

Description:

    This routine checks the path string for a pipe file descriptor.

    N.B. This routine should not be used for fifos.

Arguments:

    Fd - Supplies the pipe file descriptor.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    char Expected[100];
    int Result;
    struct stat Stat;

    LxtCheckErrnoZeroSuccess(fstat(Fd, &Stat));
    sprintf(Expected, "pipe:[%lu]", Stat.st_ino);
    LxtCheckResult(LxtCheckFdPath(Fd, Expected));

ErrorExit:
    return Result;
}

int PipeEpoll(PLXT_ARGS Args)

/*++

Description:

    This routine tests polling behavior for the read and write end of pipes.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    char Buf[3];
    int Count;
    int Index;
    int Pipes[2];
    int PollFd;
    int Result;
    struct epoll_event Event;

    PollFd = -1;
    for (Index = 0; Index < LXT_COUNT_OF(Pipes); Index += 1)
    {
        Pipes[Index] = -1;
    }

    //
    // Create a pipe and write to it so it's both read and write ready.
    //

    LxtCheckErrnoZeroSuccess(pipe(Pipes));
    LxtCheckErrno(write(Pipes[1], "test", 4));

    //
    // Make sure polling for write times out on the read descriptor, but
    // polling for read works.
    //

    LxtCheckErrno(PollFd = epoll_create1(0));
    Event.events = EPOLLOUT;
    Event.data.fd = Pipes[0];
    LxtCheckErrno(epoll_ctl(PollFd, EPOLL_CTL_ADD, Pipes[0], &Event));
    LxtCheckErrnoZeroSuccess(epoll_wait(PollFd, &Event, 1, 0));
    Event.events = EPOLLIN;
    LxtCheckErrno(epoll_ctl(PollFd, EPOLL_CTL_MOD, Pipes[0], &Event));
    memset(&Event, 0, sizeof(Event));
    LxtCheckErrno(Count = epoll_wait(PollFd, &Event, 1, 0));
    LxtCheckEqual(Count, 1, "%d");
    LxtCheckEqual(Event.events, EPOLLIN, "0x%x");

    //
    // Make sure polling for read times out on the write descriptor, but
    // polling for write works.
    //

    LxtCheckErrno(epoll_ctl(PollFd, EPOLL_CTL_DEL, Pipes[0], NULL));
    Event.events = EPOLLIN;
    Event.data.fd = Pipes[1];
    LxtCheckErrno(epoll_ctl(PollFd, EPOLL_CTL_ADD, Pipes[1], &Event));
    LxtCheckErrnoZeroSuccess(epoll_wait(PollFd, &Event, 1, 0));
    Event.events = EPOLLOUT;
    LxtCheckErrno(epoll_ctl(PollFd, EPOLL_CTL_MOD, Pipes[1], &Event));
    memset(&Event, 0, sizeof(Event));
    LxtCheckErrno(Count = epoll_wait(PollFd, &Event, 1, 0));
    LxtCheckEqual(Count, 1, "%d");
    LxtCheckEqual(Event.events, EPOLLOUT, "0x%x");

    //
    // Test edge triggered epoll events.
    //

    Event.events = EPOLLOUT | EPOLLET;
    LxtCheckErrno(epoll_ctl(PollFd, EPOLL_CTL_MOD, Pipes[1], &Event));
    Event.events = EPOLLIN | EPOLLET;
    Event.data.fd = Pipes[0];
    LxtCheckErrno(epoll_ctl(PollFd, EPOLL_CTL_ADD, Pipes[0], &Event));
    memset(&Event, 0, sizeof(Event));
    LxtCheckErrno(Count = epoll_wait(PollFd, &Event, 1, 0));
    LxtCheckEqual(Count, 1, "%d");
    LxtCheckEqual(Event.events, EPOLLOUT, "0x%x");
    LxtCheckErrno(Count = epoll_wait(PollFd, &Event, 1, 0));
    LxtCheckEqual(Count, 1, "%d");
    LxtCheckEqual(Event.events, EPOLLIN, "0x%x");

    //
    // Both edge-triggered events should have fired, so no new events should
    // be available.
    //

    LxtCheckErrnoZeroSuccess(epoll_wait(PollFd, &Event, 1, 0));

    //
    // Perform read and write operations and recheck epoll status.
    //

    LxtCheckErrno(write(Pipes[1], "more", 4));
    LxtCheckErrno(Count = epoll_wait(PollFd, &Event, 1, 0));
    LxtCheckEqual(Count, 1, "%d");
    if (Event.events == EPOLLOUT)
    {

        //
        // TODO_LX: The WSL pipe implementation shares the epoll between both
        //          endpoints, so the write endpoint is triggered here when it
        //          is not expected.
        //

        LxtCheckErrno(Count = epoll_wait(PollFd, &Event, 1, 0));
        LxtCheckEqual(Count, 1, "%d");
    }

    LxtCheckEqual(Event.events, EPOLLIN, "0x%x");
    LxtCheckErrnoZeroSuccess(epoll_wait(PollFd, &Event, 1, 0));
    LxtCheckErrno(read(Pipes[0], Buf, sizeof(Buf)));
    LxtCheckErrnoZeroSuccess(epoll_wait(PollFd, &Event, 1, 0));

ErrorExit:
    if (PollFd >= 0)
    {
        close(PollFd);
    }

    for (Index = 0; Index < LXT_COUNT_OF(Pipes); Index += 1)
    {
        if (Pipes[Index] >= 0)
        {
            close(Pipes[Index]);
        }
    }

    return Result;
}

int PipeFifo(PLXT_ARGS Args)

/*++

Description:

    This routine tests fifo files.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    unsigned int* Buffer;
    ssize_t Bytes;
    pid_t ChildPid;
    int Fd;
    int Fd2;
    int Result;
    struct stat Stat;

    Fd = -1;
    Fd2 = -1;
    Buffer = NULL;
    LXT_SYNCHRONIZATION_POINT_INIT();

    //
    // Create the fifo and make sure umask is applied.
    //

    umask(022);
    LxtCheckErrnoZeroSuccess(mkfifo(PIPE_FIFO, 0666));
    LxtCheckErrnoZeroSuccess(lstat(PIPE_FIFO, &Stat));
    LxtCheckEqual(Stat.st_mode, S_IFIFO | 0644, "0%o");

    //
    // Check error when the file exists.
    //

    LxtCheckErrnoFailure(mkfifo(PIPE_FIFO, 0666), EEXIST);

    //
    // Fork and connect.
    //

    Buffer = malloc(PIPE_BUFFER_LENGTH * sizeof(int));
    LxtCheckNotEqual(Buffer, NULL, "%p");
    LXT_SYNCHRONIZATION_POINT_START();
    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        LxtCheckErrno(Fd = open(PIPE_FIFO, O_RDONLY));
        LxtCheckResult(LxtCheckFdPath(Fd, PIPE_FIFO));
        LXT_SYNCHRONIZATION_POINT();
        LxtCheckResult(PipeReader(Buffer, PIPE_BUFFER_LENGTH, Fd, false));

        //
        // Connect a second writer.
        //

        LxtCheckErrno(Fd2 = open(PIPE_FIFO, O_WRONLY));
        Buffer[0] = 42;
        LxtCheckErrno(Bytes = write(Fd2, Buffer, sizeof(int)));
        LxtCheckEqual(Bytes, sizeof(int), "%ld");
        Buffer[0] = 0;
        LxtCheckErrno(Bytes = read(Fd, Buffer, PIPE_BUFFER_LENGTH * sizeof(int)));
        LxtCheckEqual(Bytes, sizeof(int), "%ld");
        LxtCheckEqual(Buffer[0], 42, "%u");
        close(Fd);
        close(Fd2);
        exit(0);
    }

    //
    // Sleep to test blocking behavior on open for the child.
    //

    sleep(1);
    LxtCheckErrno(Fd = open(PIPE_FIFO, O_WRONLY));
    LxtCheckResult(LxtCheckFdPath(Fd, PIPE_FIFO));

    //
    // Use the synchronization point to ensure open unblocks before calling
    // write.
    //

    LXT_SYNCHRONIZATION_POINT();
    LxtCheckResult(PipeWriter(Buffer, PIPE_BUFFER_LENGTH, Fd, false));
    LxtCheckResult(LxtWaitPidPoll(ChildPid, 0));

ErrorExit:
    if (Fd2 >= 0)
    {
        close(Fd2);
    }

    if (Fd >= 0)
    {
        close(Fd);
    }

    if (Buffer != NULL)
    {
        free(Buffer);
    }

    unlink(PIPE_FIFO);
    LXT_SYNCHRONIZATION_POINT_DESTROY();
    return Result;
}

int PipeFifoNonBlock(PLXT_ARGS Args)

/*++

Description:

    This routine tests fifo files with O_NONBLOCK.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    unsigned int* Buffer;
    pid_t ChildPid;
    int Fd;
    int Result;
    struct stat Stat;

    Fd = -1;
    Buffer = NULL;

    //
    // Create the fifo and make sure umask is applied.
    //

    umask(0);
    LxtCheckErrnoZeroSuccess(mkfifo(PIPE_FIFO, 0666));
    LxtCheckErrnoZeroSuccess(lstat(PIPE_FIFO, &Stat));
    LxtCheckEqual(Stat.st_mode, S_IFIFO | 0666, "0%o");

    //
    // Try to connect with write when there's no reader, non-blocking.
    //

    LxtCheckErrnoFailure(Fd = open(PIPE_FIFO, O_WRONLY | O_NONBLOCK), ENXIO);

    //
    // Fork and connect.
    //

    Buffer = malloc(PIPE_BUFFER_LENGTH * sizeof(int));
    LxtCheckNotEqual(Buffer, NULL, "%p");
    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        LxtCheckErrno(Fd = open(PIPE_FIFO, O_RDONLY | O_NONBLOCK));
        LxtCheckResult(LxtCheckFdPath(Fd, PIPE_FIFO));
        LxtCheckErrnoZeroSuccess(read(Fd, Buffer, PIPE_BUFFER_LENGTH)) LxtCheckResult(PipeReader(Buffer, PIPE_BUFFER_LENGTH, Fd, true));
        close(Fd);
        exit(0);
    }

    //
    // O_NONBLOCK for write works once there is a reader.
    //

    sleep(1);
    LxtCheckErrno(Fd = open(PIPE_FIFO, O_WRONLY | O_NONBLOCK));
    LxtCheckResult(LxtCheckFdPath(Fd, PIPE_FIFO));
    LxtCheckResult(PipeWriter(Buffer, PIPE_BUFFER_LENGTH, Fd, true));
    LxtCheckResult(LxtWaitPidPoll(ChildPid, 0));

ErrorExit:
    if (Fd >= 0)
    {
        close(Fd);
    }

    if (Buffer != NULL)
    {
        free(Buffer);
    }

    unlink(PIPE_FIFO);
    return Result;
}

int PipeFifoReadWrite(PLXT_ARGS Args)

/*++

Description:

    This routine tests opening a fifo for read/write.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    char Buffer[100];
    ssize_t Bytes;
    int Fd;
    int Result;
    struct stat Stat;

    Fd = -1;

    //
    // Create the fifo and make sure umask is applied.
    //

    umask(0);
    LxtCheckErrnoZeroSuccess(mkfifo(PIPE_FIFO, 0666));
    LxtCheckErrnoZeroSuccess(lstat(PIPE_FIFO, &Stat));
    LxtCheckEqual(Stat.st_mode, S_IFIFO | 0666, "0%o");

    //
    // Open the fifo for read/write which should not block.
    //

    LxtCheckErrno(Fd = open(PIPE_FIFO, O_RDWR));
    LxtCheckResult(LxtCheckFdPath(Fd, PIPE_FIFO));
    LxtCheckErrno(Bytes = write(Fd, PIPE_FIFO_MESSAGE, strlen(PIPE_FIFO_MESSAGE) + 1));

    LxtCheckEqual(Bytes, strlen(PIPE_FIFO_MESSAGE) + 1, "%ld");
    LxtCheckErrno(Bytes = read(Fd, Buffer, sizeof(Buffer)));
    LxtCheckEqual(Bytes, strlen(PIPE_FIFO_MESSAGE) + 1, "%ld");
    LxtCheckStringEqual(Buffer, PIPE_FIFO_MESSAGE);

ErrorExit:
    if (Fd >= 0)
    {
        close(Fd);
    }

    unlink(PIPE_FIFO);
    return Result;
}

int PipeFifoReopen(PLXT_ARGS Args)

/*++

Description:

    This routine tests reopening a pipe after closing one end.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    char Buffer[100];
    ssize_t Bytes;
    int ChildPid;
    int Count;
    int ReadFd;
    int Result;
    int WriteFd;
    struct pollfd PollFd;

    ReadFd = -1;
    WriteFd = -1;

    //
    // Fork because this test changes signal state.
    //

    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        LxtCheckResult(LxtSignalInitialize());
        LxtCheckResult(LxtSignalSetupHandler(SIGPIPE, SA_SIGINFO));

        //
        // Create and open the fifo.
        //

        LxtCheckErrnoZeroSuccess(mkfifo(PIPE_FIFO, 0666));
        LxtCheckErrno(ReadFd = open(PIPE_FIFO, O_RDONLY | O_NONBLOCK));
        LxtCheckErrno(WriteFd = open(PIPE_FIFO, O_WRONLY | O_NONBLOCK));
        memset(&PollFd, 0, sizeof(PollFd));

        //
        // Check the initial state of the write end.
        //

        PollFd.fd = WriteFd;
        PollFd.events = POLLIN | POLLOUT | POLLERR | POLLHUP;
        LxtCheckErrno(Count = poll(&PollFd, 1, 1000));
        LxtCheckEqual(Count, 1, "%d");
        LxtCheckEqual(PollFd.revents, POLLOUT, "0x%x");

        //
        // Close the read end and check the write end returns error.
        //

        LxtCheckErrnoZeroSuccess(close(ReadFd));
        ReadFd = -1;
        LxtCheckErrno(Count = poll(&PollFd, 1, 1000));
        LxtCheckEqual(Count, 1, "%d");
        LxtCheckEqual(PollFd.revents, POLLOUT | POLLERR, "0x%x");
        LxtCheckErrnoFailure(write(WriteFd, PIPE_FIFO_MESSAGE, strlen(PIPE_FIFO_MESSAGE) + 1), EPIPE);

        LxtCheckResult(LxtSignalCheckInfoReceived(SIGPIPE, SI_USER, getpid(), getuid()));

        //
        // Try to open an additional write end, which should fail because there
        // is no reader.
        //

        LxtCheckErrnoFailure(open(PIPE_FIFO, O_WRONLY | O_NONBLOCK), ENXIO);

        //
        // Open a new read end and check the write end is functional again.
        //

        LxtCheckErrno(ReadFd = open(PIPE_FIFO, O_RDONLY | O_NONBLOCK));
        LxtCheckErrno(Count = poll(&PollFd, 1, 1000));
        LxtCheckEqual(Count, 1, "%d");
        LxtCheckEqual(PollFd.revents, POLLOUT, "0x%x");
        LxtCheckErrno(Bytes = write(WriteFd, PIPE_FIFO_MESSAGE, strlen(PIPE_FIFO_MESSAGE) + 1));

        LxtCheckEqual(Bytes, strlen(PIPE_FIFO_MESSAGE) + 1, "%ld");

        //
        // Check the poll state of the read end.
        //

        PollFd.fd = ReadFd;
        LxtCheckErrno(Count = poll(&PollFd, 1, 1000));
        LxtCheckEqual(Count, 1, "%d");
        LxtCheckEqual(PollFd.revents, POLLIN, "0x%x");

        //
        // Close the write end and check the read end reports hangup.
        //

        LxtCheckErrnoZeroSuccess(close(WriteFd));
        WriteFd = -1;
        LxtCheckErrno(Count = poll(&PollFd, 1, 1000));
        LxtCheckEqual(Count, 1, "%d");
        LxtCheckEqual(PollFd.revents, POLLIN | POLLHUP, "0x%x");

        //
        // Open a new write end and check the read end returns the old data.
        //

        LxtCheckErrno(WriteFd = open(PIPE_FIFO, O_WRONLY | O_NONBLOCK));
        LxtCheckErrno(Count = poll(&PollFd, 1, 1000));
        LxtCheckEqual(Count, 1, "%d");
        LxtCheckEqual(PollFd.revents, POLLIN, "0x%x");
        LxtCheckErrno(Bytes = read(ReadFd, Buffer, sizeof(Buffer)));
        LxtCheckEqual(Bytes, strlen(PIPE_FIFO_MESSAGE) + 1, "%ld");
        LxtCheckStringEqual(Buffer, PIPE_FIFO_MESSAGE);

        //
        // Read should fail now because there's no more data.
        //

        LxtCheckErrnoFailure(read(ReadFd, Buffer, sizeof(Buffer)), EAGAIN);

        //
        // Close the write end and make sure the read end return EOF.
        //

        LxtCheckErrnoZeroSuccess(close(WriteFd));
        LxtCheckErrnoZeroSuccess(read(ReadFd, Buffer, sizeof(Buffer)));
        LxtCheckErrnoZeroSuccess(close(ReadFd));
        exit(0);
    }

    LxtCheckResult(LxtWaitPidPoll(ChildPid, 0));

ErrorExit:
    if (ReadFd >= 0)
    {
        close(ReadFd);
    }

    if (WriteFd >= 0)
    {
        close(WriteFd);
    }

    unlink(PIPE_FIFO);
    return Result;
}

int PipeReopen(PLXT_ARGS Args)

/*++

Description:

    This routine tests reopening a pipe through /proc/self/fd.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    char Buffer[100];
    int ChildPid;
    int Fd;
    struct stat FdStat;
    char ChildPath[PATH_MAX];
    char Path[PATH_MAX];
    int Pipes[2];
    struct stat PipeStat;
    int Result;

    Pipes[0] = -1;
    Pipes[1] = -1;
    Fd = -1;
    ChildPid = -1;
    LxtCheckErrnoZeroSuccess(pipe(Pipes));
    LxtCheckErrno(write(Pipes[1], PIPE_FIFO_MESSAGE, sizeof(PIPE_FIFO_MESSAGE)));

    //
    // Attempt to reopen the read end.
    //

    sprintf(Path, PIPE_FD_PATH_FORMAT, Pipes[0]);
    LxtCheckErrno(Fd = open(Path, O_RDONLY));
    memset(Buffer, 0, sizeof(Buffer));
    LxtCheckErrno(read(Fd, Buffer, sizeof(Buffer)));
    LxtCheckStringEqual(Buffer, PIPE_FIFO_MESSAGE);
    LxtCheckErrnoZeroSuccess(close(Fd));

    //
    // Reopen the write end through the read FD.
    //

    LxtCheckErrno(Fd = open(Path, O_WRONLY));
    LxtCheckErrno(write(Fd, PIPE_FIFO_MESSAGE, sizeof(PIPE_FIFO_MESSAGE)));
    memset(Buffer, 0, sizeof(Buffer));
    LxtCheckErrno(read(Pipes[0], Buffer, sizeof(Buffer)));
    LxtCheckStringEqual(Buffer, PIPE_FIFO_MESSAGE);

    //
    // Check the result of stat.
    //

    LxtCheckErrnoZeroSuccess(fstat(Pipes[0], &PipeStat));
    LxtCheckErrnoZeroSuccess(stat(Path, &FdStat));
    LxtCheckMemoryEqual(&PipeStat, &FdStat, sizeof(struct stat));

    //
    // Failing variations.
    //

    sprintf(ChildPath, PIPE_CHILD_PATH_FORMAT, Path, ".");
    LxtCheckErrnoFailure(open(ChildPath, O_RDONLY), ENOTDIR);
    sprintf(ChildPath, PIPE_CHILD_PATH_FORMAT, Path, "..");
    LxtCheckErrnoFailure(open(ChildPath, O_RDONLY), ENOTDIR);
    sprintf(ChildPath, PIPE_CHILD_PATH_FORMAT, Path, "foo");
    LxtCheckErrnoFailure(open(ChildPath, O_RDONLY), ENOTDIR);
    LxtCheckErrnoFailure(openat(Fd, "foo", O_RDONLY), ENOTDIR);

    //
    // Pipes have permissions set to 0600, so they can only be reopened by the
    // owner.
    //

    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        setuid(1000);
        LxtCheckErrnoFailure(open(Path, O_RDONLY), EACCES);
        goto ErrorExit;
    }

    LxtCheckResult(LxtWaitPidPoll(ChildPid, 0));

ErrorExit:
    if (Pipes[0] >= 0)
    {
        close(Pipes[0]);
    }

    if (Pipes[1] >= 0)
    {
        close(Pipes[1]);
    }

    if (Fd >= 0)
    {
        close(Fd);
    }

    if (ChildPid == 0)
    {
        exit(Result);
    }

    return Result;
}

int PipeStat(PLXT_ARGS Args)

/*++

Description:

    This routine tests the results of fstat on a pipe.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int ChildPid;
    int Pipes[2];
    int Result;
    struct stat Stat;

    Pipes[0] = -1;
    Pipes[1] = -1;
    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        LxtCheckErrno(setfsgid(1001));
        LxtCheckErrno(setfsuid(1000));
        LxtCheckErrnoZeroSuccess(pipe(Pipes));
        LxtCheckErrnoZeroSuccess(fstat(Pipes[0], &Stat));
        LxtCheckGreater(Stat.st_ino, 0, "%lu");
        LxtCheckEqual(Stat.st_uid, 1000, "%d");
        LxtCheckEqual(Stat.st_gid, 1001, "%d");
        LxtCheckEqual(Stat.st_mode, S_IFIFO | 0600, "%d");
        LxtCheckEqual(Stat.st_blksize, 4096, "%ld");
        LxtCheckEqual(Stat.st_blocks, 0, "%ld");
        LxtCheckEqual(Stat.st_size, 0, "%ld");
        LxtCheckEqual(Stat.st_nlink, 1, "%u");
        LxtCheckResult(PipeCheckFdPath(Pipes[0]));
        LxtCheckResult(PipeCheckFdPath(Pipes[1]));
        LxtCheckErrnoZeroSuccess(close(Pipes[0]));
        LxtCheckErrnoZeroSuccess(close(Pipes[1]));
        exit(0);
    }

    LxtCheckResult(LxtWaitPidPoll(ChildPid, 0));

ErrorExit:
    if (Pipes[0] >= 0)
    {
        close(Pipes[0]);
    }

    if (Pipes[1] >= 0)
    {
        close(Pipes[1]);
    }

    return Result;
}

int PipeTest(bool Polling, bool UsePipe2)

/*++
--*/

{

    unsigned int* Buffer;
    bool Child;
    pid_t Pid;
    int Pipes[2];
    int Result;
    int ReturnStatus;

    Child = false;
    Pid = -1;
    Buffer = mmap(NULL, PIPE_BUFFER_LENGTH * sizeof(unsigned int), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    if (Buffer == MAP_FAILED)
    {
        Result = -1;
        LxtLogError("Could not map buffer! %d", errno);
        goto PipeTestEnd;
    }

    if (UsePipe2 == false)
    {
        Result = pipe(Pipes);
        if (Result == -1)
        {
            LxtLogError("Could not create pipes with pipe! %d", errno);
            goto PipeTestEnd;
        }
    }
    else
    {
        Result = LxtPipe2(Pipes, 0);
        if (Result == -1)
        {
            LxtLogError("Could not create pipes with pipe2! %d", errno);
            goto PipeTestEnd;
        }
    }

    if (Polling != false)
    {
        Result = fcntl(Pipes[0], F_SETFL, O_NONBLOCK);
        if (Result == -1)
        {
            LxtLogError("Could not set pipe to non-blocking! %d", errno);
            goto PipeTestEnd;
        }

        Result = fcntl(Pipes[1], F_SETFL, O_NONBLOCK);
        if (Result == -1)
        {
            LxtLogError("Could not set pipe to non-blocking! %d", errno);
            goto PipeTestEnd;
        }
    }

    Pid = fork();
    if (Pid == -1)
    {
        LxtLogError("Could not fork! %d", errno);
        goto PipeTestEnd;
    }

    if (Pid > 0)
    {
        close(Pipes[0]);
        Result = PipeWriter(Buffer, PIPE_BUFFER_LENGTH, Pipes[1], Polling);
        if (Result == -1)
        {
            LxtLogError("PipeWriter failed! %d", errno);
            goto PipeTestEnd;
        }
    }
    else
    {
        Child = true;
        close(Pipes[1]);
        Result = PipeReader(Buffer, PIPE_BUFFER_LENGTH, Pipes[0], Polling);
        if (Result == -1)
        {
            LxtLogError("PipeReader failed! %d", errno);
            goto PipeTestEnd;
        }
    }

    Result = LXT_RESULT_SUCCESS;

PipeTestEnd:
    if (Child != false)
    {
        _exit(0);
    }
    else if (Pid > 0)
    {
        Result = waitpid(Pid, &ReturnStatus, 0);
        if (Result == -1)
        {
            LxtLogError("PipeTest reader child failed: %d", errno);
        }
        else
        {
            LxtLogInfo("PipeTest read child has exited.");
        }
    }

    if (Buffer != NULL)
    {
        munmap(Buffer, PIPE_BUFFER_LENGTH * sizeof(unsigned int));
    }

    return Result;
}

int PipeVariation0(PLXT_ARGS Args)

/*++
--*/

{

    return PipeTest(false, false);
}

int PipeWriter(unsigned int* Buffer, size_t BufferLength, int Pipe, bool Polling)

/*++
--*/

{

    char* Current;
    size_t Index;
    size_t Loop;
    int Result;
    struct pollfd PollFd;
    size_t Size;
    size_t SizeRemaining;
    unsigned int Value;
    size_t WriteSize;
    ssize_t Written;

    PollFd.fd = Pipe;
    PollFd.events = POLLOUT;
    Value = 0;
    Size = BufferLength * sizeof(unsigned int);

    for (Loop = 0; Loop < PIPE_LOOPS; Loop += 1)
    {
        LxtLogInfo("Loop #%u...", Loop);
        for (Index = 0; Index < BufferLength; Index += 1)
        {
            Buffer[Index] = Value;
            Value += 1;
        }

        if (Polling != false)
        {
            SizeRemaining = Size;
            Current = (char*)Buffer;
            while (SizeRemaining > 0)
            {
                Result = poll(&PollFd, 1, -1);
                if (Result == -1)
                {
                    Result = errno;
                    LxtLogError("Failed to poll for write! %d", Result);
                    goto WriterEnd;
                }

                if ((PollFd.revents & POLLOUT) == 0)
                {
                    LxtLogError("Error condition on write poll!");
                    Result = EINVAL;
                    goto WriterEnd;
                }

                while (SizeRemaining > 0)
                {
                    WriteSize = min(SizeRemaining, _32KB);
                    Written = write(Pipe, Current, WriteSize);
                    if (Written == -1)
                    {
                        if (errno == EAGAIN)
                        {
                            LxtLogInfo("Write would have blocked");
                            break;
                        }

                        Result = errno;
                        LxtLogError("Failed to write! %d", Result);
                        goto WriterEnd;
                    }

                    SizeRemaining -= Written;
                    Current += Written;
                }
            }
        }
        else
        {
            Written = write(Pipe, Buffer, Size);
            if (Written != (ssize_t)Size)
            {
                if (Written >= 0)
                {
                    LxtLogError("Wrote fewer bytes (%d) than expected (%d)!", Written, Size);
                    Result = EINVAL;
                }
                else
                {
                    Result = errno;
                    LxtLogError("Failed to write! %d", Result);
                }

                goto WriterEnd;
            }
        }
    }

    Result = 0;

WriterEnd:
    if (Result != 0)
    {
        errno = Result;
        Result = -1;
    }

    return Result;
}

int PipeReader(unsigned int* Buffer, size_t BufferLength, int Pipe, bool Polling)

/*++
--*/

{

    int BytesAvailable;
    char* Current;
    size_t Index;
    size_t Loop;
    struct pollfd PollFd;
    ssize_t Read;
    int Result;
    size_t Size;
    size_t SizeRemaining;
    unsigned int Value;

    PollFd.fd = Pipe;
    PollFd.events = POLLIN;
    Value = 0;
    Size = BufferLength * sizeof(unsigned int);
    for (Loop = 0; Loop < PIPE_LOOPS; Loop += 1)
    {
        LxtLogInfo("Loop #%u...", Loop);
        Current = (char*)Buffer;
        SizeRemaining = Size;
        while (SizeRemaining > 0)
        {
            if (Polling != false)
            {
                Result = poll(&PollFd, 1, -1);
                if (Result == -1)
                {
                    Result = errno;
                    LxtLogError("Failed to poll for read! %d", Result);
                    goto ErrorExit;
                }

                if ((PollFd.revents & POLLIN) == 0)
                {
                    LxtLogError("Error condition on read poll: 0x%X!", PollFd.revents);
                    Result = EINVAL;
                    goto ErrorExit;
                }
            }

            //
            // Query the number of bytes available via the FIONREAD ioctl.
            //

            LxtCheckErrno(ioctl(Pipe, FIONREAD, &BytesAvailable));
            Read = read(Pipe, Current, _4KB);
            if (Read == -1)
            {
                Result = errno;
                LxtLogError("Failed to read! %d", Result);
                goto ErrorExit;
            }

            if ((Read == 0) || (Read % sizeof(unsigned int) != 0))
            {
                LxtLogError("Read an invalid number of bytes (%d)!", Read);
                Result = EINVAL;
                goto ErrorExit;
            }

            Current += Read;
            SizeRemaining -= Read;
        }

        //
        // Validate the buffer.
        //

        for (Index = 0; Index < BufferLength; Index += 1)
        {
            if (Buffer[Index] != Value)
            {
                LxtLogError("PipeReader buffer invalid - contains %u instead of expected %u!", Buffer[Index], Value);
                Result = EINVAL;
                goto ErrorExit;
            }

            Value += 1;
        }
    }

    LxtLogInfo("Reads finished");
    Result = 0;

ErrorExit:
    if (Result != 0)
    {
        errno = Result;
        Result = -1;
    }

    return Result;
}

int PipeVariation1(PLXT_ARGS Args)

/*++
--*/

{

    return PipeTest(true, true);
}

int PipeReaderHangup(PLXT_ARGS Args)

/*++

Routine Description:

    This routine validates the reader hangup epoll semantics.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    ssize_t BytesRead;
    ssize_t BytesWritten;
    ssize_t ExpectedResult;
    char* Message = "This is a test string for piping";
    int Pipes[2] = {-1, -1};
    struct pollfd PollFd;
    char ReadBuffer[100];
    int Result;

    //
    // Create a pipe.
    //

    LxtCheckErrno(pipe(Pipes));

    //
    // Set the pipe FD's to non-blocking.
    //

    LxtLogInfo("Creating a pipe...");
    LxtCheckErrno(fcntl(Pipes[0], F_SETFL, O_NONBLOCK));
    LxtCheckErrno(fcntl(Pipes[1], F_SETFL, O_NONBLOCK));

    //
    // Write some data to the pipe.
    //

    LxtLogInfo("Writing some data to the pipe.");
    ExpectedResult = strlen(Message);
    LxtCheckErrno((BytesWritten = write(Pipes[1], Message, ExpectedResult)));
    LxtCheckEqual(ExpectedResult, BytesWritten, "%d");

    //
    // Validate that the only epoll flag set on the reader end is EPOLLIN.
    //

    LxtLogInfo(
        "Validating that the EPOLLIN is set on the reader end of the "
        "pipe.");

    PollFd.revents = 0;
    PollFd.fd = Pipes[0];
    PollFd.events = POLLIN | POLLOUT;
    LxtCheckErrno(poll(&PollFd, 1, -1));
    if (PollFd.revents != POLLIN)
    {
        LxtLogError(
            "Error condition on reader poll: 0x%X, Expected: "
            "0x%X! (POLLIN)",
            PollFd.revents,
            POLLIN);

        Result = EINVAL;
        goto ErrorExit;
    }

    //
    // Read data from the other end of the pipe.
    //

    LxtLogInfo("Reading the data from the reader end.") ExpectedResult = strlen(Message);
    LxtCheckErrno((BytesRead = read(Pipes[0], ReadBuffer, sizeof(ReadBuffer))));
    LxtCheckEqual(ExpectedResult, BytesRead, "%d");

    //
    // Validate that the EPOLLIN is not set on the reader end now that the data
    // has been read. This call will block, so specify a timeout value.
    //

    LxtLogInfo(
        "Validating that the EPOLLIN is *not* set on the reader end of "
        "the pipe.");

    PollFd.revents = 0;
    PollFd.fd = Pipes[0];
    PollFd.events = POLLIN | POLLOUT;
    LxtCheckErrno(poll(&PollFd, 1, 1));
    if (PollFd.revents != 0)
    {
        LxtLogError("Error condition on reader poll: 0x%X, Expected: 0x%X!", PollFd.revents, 0);

        Result = EINVAL;
        goto ErrorExit;
    }

    //
    // Hangup the reader end.
    //

    LxtLogInfo("Hanging the reader");
    close(Pipes[0]);
    Pipes[0] = -1;

    //
    // Validate that when the reader hangs up, both EPOLLOUT and EPOLLERR are
    // set.
    //

    LxtLogInfo(
        "Validating that the correct EPOLL flags are set on the writer "
        "end of the pipe.");

    PollFd.revents = 0;
    PollFd.fd = Pipes[1];
    PollFd.events = POLLIN | POLLOUT;
    LxtCheckErrno(poll(&PollFd, 1, -1));
    if (PollFd.revents != (POLLOUT | POLLERR))
    {
        LxtLogError(
            "Error condition on writer poll: 0x%X, Expected: "
            "0x%X! (POLLHUP)",
            PollFd.revents,
            (POLLOUT | POLLERR));

        Result = EINVAL;
        goto ErrorExit;
    }

    Result = 0;

ErrorExit:
    if (Pipes[0] != -1)
    {
        close(Pipes[0]);
    }

    if (Pipes[1] != -1)
    {
        close(Pipes[1]);
    }

    return Result;
}

int PipeWriterHangup(PLXT_ARGS Args)

/*++

Routine Description:

    This routine validates the writer hangup epoll semantics.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    ssize_t BytesRead;
    ssize_t BytesWritten;
    ssize_t ExpectedResult;
    char* Message = "This is a test string for piping";
    int Pipes[2] = {-1, -1};
    struct pollfd PollFd;
    char ReadBuffer[100] = {0};
    int Result;

    //
    // Create a pipe.
    //

    LxtCheckErrno(pipe(Pipes));

    //
    // Set the pipe FD's to non-blocking.
    //

    LxtLogInfo("Creating a pipe...");
    LxtCheckErrno(fcntl(Pipes[0], F_SETFL, O_NONBLOCK));
    LxtCheckErrno(fcntl(Pipes[1], F_SETFL, O_NONBLOCK));

    //
    // Write some data to the pipe.
    //

    LxtLogInfo("Writing some data to the pipe.");
    ExpectedResult = strlen(Message);
    LxtCheckErrno((BytesWritten = write(Pipes[1], Message, ExpectedResult)));
    LxtCheckEqual(ExpectedResult, BytesWritten, "%d");

    //
    // Hangup the writer end.
    //

    LxtLogInfo("Hanging the writer");
    close(Pipes[1]);
    Pipes[1] = -1;

    //
    // Validate that both EPOLLIN and EPOLLHUP is set on the reader side.
    //

    LxtLogInfo(
        "Validating that the correct EPOLL flags are set on the reader "
        "end of the pipe.");

    PollFd.revents = 0;
    PollFd.fd = Pipes[0];
    PollFd.events = POLLIN | POLLOUT;
    LxtCheckErrno(poll(&PollFd, 1, -1));
    if (PollFd.revents != (POLLHUP | POLLIN))
    {
        LxtLogError(
            "Error condition on reader poll: 0x%X. Expected: "
            "0x%X (POLLHUP | POLLIN)",
            PollFd.revents,
            (POLLHUP | POLLIN));

        Result = EINVAL;
        goto ErrorExit;
    }

    //
    // Drain the read side of the pipe.
    //

    LxtLogInfo("Reading all the data from the pipe.") ExpectedResult = strlen(Message);
    LxtCheckErrno((BytesRead = read(Pipes[0], ReadBuffer, sizeof(ReadBuffer))));
    LxtCheckEqual(ExpectedResult, BytesRead, "%d");

    LxtLogInfo(
        "Validating that only EPOLLHUP is set on the reader side now "
        "that the pipe has been drained.");

    PollFd.revents = 0;
    PollFd.fd = Pipes[0];
    PollFd.events = POLLIN | POLLOUT;
    LxtCheckErrno(poll(&PollFd, 1, -1));
    if (PollFd.revents != POLLHUP)
    {
        LxtLogError(
            "Error condition on reader poll: 0x%X. Expected: "
            "0x%X (POLLHUP)",
            PollFd.revents,
            POLLHUP);

        Result = EINVAL;
        goto ErrorExit;
    }

    Result = 0;

ErrorExit:
    if (Pipes[0] != -1)
    {
        close(Pipes[0]);
    }

    if (Pipes[1] != -1)
    {
        close(Pipes[1]);
    }

    return Result;
}

int PipeVariationIoctl(PLXT_ARGS Args)

/*++

Routine Description:

    This routine validates the pipe ioctls.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int Pipes[2] = {-1, -1};
    char Buffer[256] = {0};
    int Result;

    //
    // Create a pipe and check the ioctls
    //

    LxtCheckErrno(pipe(Pipes));
    LxtCheckErrnoFailure(ioctl(Pipes[0], TCGETS, Buffer), ENOTTY);

ErrorExit:
    if (Pipes[0] != -1)
    {
        close(Pipes[0]);
    }

    if (Pipes[1] != -1)
    {
        close(Pipes[1]);
    }

    return Result;
}

int PipeFileLocking(PLXT_ARGS Args)

/*++

Routine Description:

    This routine validates the pipe support for file locking.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int Pipes[2] = {-1, -1};
    int Result;

    //
    // Create a pipe and check the flock and lockf (fnctl+F_*LK).
    //

    LxtCheckErrno(pipe(Pipes));
    LxtCheckErrno(flock(Pipes[0], LOCK_SH));
    LxtCheckErrno(flock(Pipes[1], LOCK_SH));
    LxtCheckErrno(flock(Pipes[0], LOCK_UN));
    LxtCheckErrno(flock(Pipes[1], LOCK_UN));
    LxtCheckErrno(flock(Pipes[0], LOCK_EX));
    LxtCheckErrno(flock(Pipes[0], LOCK_UN));
    LxtCheckErrno(flock(Pipes[1], LOCK_EX));
    LxtCheckErrno(flock(Pipes[1], LOCK_UN));
    LxtCheckErrno(lockf(Pipes[0], F_TEST, 0));
    LxtCheckErrno(lockf(Pipes[1], F_TEST, 0));
    LxtCheckErrno(lockf(Pipes[1], F_LOCK, 0));
    LxtCheckErrno(lockf(Pipes[1], F_ULOCK, 0));

ErrorExit:
    if (Pipes[0] != -1)
    {
        close(Pipes[0]);
    }

    if (Pipes[1] != -1)
    {
        close(Pipes[1]);
    }

    return Result;
}

int PipeZeroByteRead(PLXT_ARGS Args)

/*++

Description:

    This routine tests zero byte read on pipes.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    char Buffer;
    int Pipes[2];
    int Result;

    Pipes[0] = -1;
    Pipes[1] = -1;
    LxtCheckErrnoZeroSuccess(pipe(Pipes));
    LxtCheckErrno(read(Pipes[0], &Buffer, 0));

ErrorExit:
    if (Pipes[0] >= 0)
    {
        close(Pipes[0]);
    }

    if (Pipes[1] >= 0)
    {
        close(Pipes[1]);
    }

    return Result;
}

int PipeSecurityHelper(int Pipe, int Uid, int Gid)

/*++

Description:

    This routine tests security attributes on pipes.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int Result;
    struct stat Stat;

    //
    // Check the original values.
    //

    LxtCheckErrnoZeroSuccess(fstat(Pipe, &Stat));
    LxtCheckEqual(Stat.st_uid, Uid, "%d");
    LxtCheckEqual(Stat.st_gid, Gid, "%d");
    LxtCheckEqual(Stat.st_mode, S_IFIFO | 0600, "%d");

    //
    // Check the values after chmod.
    //

    LxtCheckErrnoZeroSuccess(fchmod(Pipe, 0777));
    LxtCheckErrnoZeroSuccess(fstat(Pipe, &Stat));
    LxtCheckEqual(Stat.st_uid, Uid, "%d");
    LxtCheckEqual(Stat.st_gid, Gid, "%d");
    LxtCheckEqual(Stat.st_mode, S_IFIFO | 0777, "%d");

    //
    // Check the values after chown to the current user\group.
    //

    LxtCheckErrnoZeroSuccess(fchown(Pipe, Uid, Gid));
    LxtCheckErrnoZeroSuccess(fstat(Pipe, &Stat));
    LxtCheckEqual(Stat.st_uid, Uid, "%d");
    LxtCheckEqual(Stat.st_gid, Gid, "%d");
    LxtCheckEqual(Stat.st_mode, S_IFIFO | 0777, "%d");

    //
    // Update the user\group and check that it changes as root. As non-root
    // the user doesn't have permissions to make the updates.
    //

    if (Uid == 0)
    {
        LxtCheckErrnoZeroSuccess(fchown(Pipe, Uid + 1, Gid));
        LxtCheckErrnoZeroSuccess(fstat(Pipe, &Stat));
        LxtCheckEqual(Stat.st_uid, Uid + 1, "%d");
        LxtCheckEqual(Stat.st_gid, Gid, "%d");
        LxtCheckEqual(Stat.st_mode, S_IFIFO | 0777, "%d");

        LxtCheckErrnoZeroSuccess(fchown(Pipe, Uid, Gid + 1));
        LxtCheckErrnoZeroSuccess(fstat(Pipe, &Stat));
        LxtCheckEqual(Stat.st_uid, Uid, "%d");
        LxtCheckEqual(Stat.st_gid, Gid + 1, "%d");
        LxtCheckEqual(Stat.st_mode, S_IFIFO | 0777, "%d");
    }
    else
    {
        LxtCheckErrnoFailure(fchown(Pipe, Uid + 1, Gid), EPERM);
        LxtCheckErrnoFailure(fchown(Pipe, Uid, Gid + 1), EPERM);
    }

    //
    // Check user\group updates with -1.
    //

    LxtCheckErrnoZeroSuccess(fchown(Pipe, Uid, -1));
    LxtCheckErrnoZeroSuccess(fchown(Pipe, -1, Gid));
    LxtCheckErrnoZeroSuccess(fchown(Pipe, -1, -1));

ErrorExit:
    return Result;
}

int PipeSecurity(PLXT_ARGS Args)

/*++

Description:

    This routine tests security attributes on pipes.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int ChildPid;
    int Pipes[2];
    int Result;
    struct stat Stat;

    Pipes[0] = -1;
    Pipes[1] = -1;
    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {

        //
        // Check the security as root.
        //

        LxtCheckErrnoZeroSuccess(pipe(Pipes));
        LxtCheckResult(PipeSecurityHelper(Pipes[0], 0, 0));
        LxtClose(Pipes[0]);
        Pipes[0] = -1;
        LxtClose(Pipes[1]);
        Pipes[1] = -1;

        //
        // Check the security as a different user\group which drops
        // capabilities.
        //

        LxtCheckErrno(setfsuid(1000));
        LxtCheckErrno(setfsgid(1001));
        LxtCheckErrnoZeroSuccess(pipe(Pipes));
        LxtCheckResult(PipeSecurityHelper(Pipes[0], 1000, 1001));

        _exit(0);
    }

    LxtCheckResult(LxtWaitPidPoll(ChildPid, 0));

ErrorExit:
    if (Pipes[0] >= 0)
    {
        close(Pipes[0]);
    }

    if (Pipes[1] >= 0)
    {
        close(Pipes[1]);
    }

    if (ChildPid == 0)
    {
        _exit(Result);
    }

    return Result;
}

int PipeFcntlRoundUpToPower2(int Value)
{

    if (Value < 0)
    {
        Value = 0;
    }
    else
    {
        --Value;
        Value |= Value >> 1;
        Value |= Value >> 2;
        Value |= Value >> 4;
        Value |= Value >> 8;
        Value |= Value >> 16;
        Value += 1;
    }

    return Value;
}

int PipeFcntl(PLXT_ARGS Args)

/*++

Description:

    This routine tests the pipe fcntl commands.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    char Buffer[_4KB + 1];
    struct __user_cap_data_struct CapData[2];
    struct __user_cap_header_struct CapHeader;
    int ChildPid;
    int EventFd;
    int Index;
    int Pipes[2];
    int Result;
    int Size;
    int SizeSet;
    int SizesToSet[] = {
        0,
        1,
        _4KB - 1,
        _4KB,
        _64KB - 1,
        _64KB,
        _64KB + 1,
        _64KB * 2,
        _64KB * 2 + 1,
        _64KB * 4 - 1,
        _64KB * 4,
        _64KB * 4 + 1,
        PIPE_DEFAULT_MAX_SIZE - 1,
        PIPE_DEFAULT_MAX_SIZE,
        PIPE_DEFAULT_MAX_SIZE + 1,
        _64KB};

    ChildPid = -1;
    EventFd = -1;
    Pipes[0] = -1;
    Pipes[1] = -1;
    LxtCheckErrnoZeroSuccess(pipe(Pipes));
    LxtCheckErrno(EventFd = eventfd(0, 0));

    //
    // Check the initial values for F_GETPIPE_SZ.
    //

    LxtCheckErrno(Size = fcntl(Pipes[0], F_GETPIPE_SZ));
    LxtCheckEqual(Size, _64KB, "%d");
    LxtCheckErrno(Size = fcntl(Pipes[1], F_GETPIPE_SZ));
    LxtCheckEqual(Size, _64KB, "%d");

    //
    // Update the size and check for the expected values.
    //
    // From the man pages, "In the current implementation, the allocation is the
    // next higher power-of-two page-size multiple of the requested size"
    //

    srand(time(NULL));
    for (Index = 0; Index < LXT_COUNT_OF(SizesToSet); ++Index)
    {

        LxtCheckErrno(SizeSet = fcntl(Pipes[rand() % 2], F_SETPIPE_SZ, SizesToSet[Index]));
        LxtLogInfo("Setting %d -> %d", SizesToSet[Index], SizeSet);

        Size = SizesToSet[Index];
        if (Size < PAGE_SIZE)
        {
            Size = PAGE_SIZE;
        }
        else
        {
            Size = PipeFcntlRoundUpToPower2(Size);
        }

        LxtCheckEqual(SizeSet, Size, "%d");
        LxtCheckErrno(Size = fcntl(Pipes[0], F_GETPIPE_SZ));
        LxtCheckEqual(Size, SizeSet, "%d");
        LxtCheckErrno(Size = fcntl(Pipes[1], F_GETPIPE_SZ));
        LxtCheckEqual(Size, SizeSet, "%d");
    }

    //
    // Try to shrink the data below the maximum amount.
    //

    LxtCheckErrno(write(Pipes[1], Buffer, sizeof(Buffer)));
    LxtCheckErrnoFailure(fcntl(Pipes[1], F_SETPIPE_SZ, sizeof(Buffer) - 1), EBUSY);

    //
    // Try to increase the buffer from an unprivileged thread.
    //

    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {

        //
        // Drop the CAP_SYS_RESOURCE capability.
        //

        memset(&CapHeader, 0, sizeof(CapHeader));
        CapHeader.version = _LINUX_CAPABILITY_VERSION_3;
        LxtCheckErrno(LxtCapGet(&CapHeader, CapData)) LxtCheckErrno(prctl(PR_SET_KEEPCAPS, 1));
        CapData[CAP_TO_INDEX(CAP_IPC_OWNER)].permitted &= ~CAP_TO_MASK(CAP_SYS_RESOURCE);
        CapData[0].effective = CapData[0].permitted;
        CapData[1].effective = CapData[1].permitted;
        LxtCheckErrno(LxtCapSet(&CapHeader, CapData));
        LxtCheckErrnoFailure(fcntl(Pipes[1], F_SETPIPE_SZ, PIPE_DEFAULT_MAX_SIZE + 1), EPERM);
        _exit(0);
    }

    LxtCheckResult(LxtWaitPidPoll(ChildPid, 0));

    //
    // Check the maximum value permissions check.
    //

    //
    // Negative variations.
    //

    LxtCheckErrnoFailure(fcntl(Pipes[1], F_SETPIPE_SZ, -1), EINVAL);
    LxtCheckErrnoFailure(Size = fcntl(EventFd, F_GETPIPE_SZ), EBADF);
    LxtCheckErrnoFailure(Size = fcntl(EventFd, F_SETPIPE_SZ, _64KB), EBADF);

ErrorExit:
    if (ChildPid == 0)
    {
        _exit(Result);
    }

    LxtClose(EventFd);
    LxtClose(Pipes[0]);
    LxtClose(Pipes[1]);

    return Result;
}
