/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    EventFd.c

Abstract:

    This file is a eventfd test.

--*/

#include "lxtcommon.h"
#include "unittests.h"
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>

#define LXT_NAME "EventFd"

#define DEFAULT_USLEEP 5000

int EventFdVariationReadWrite(PLXT_ARGS Args);

//
// Global constants
//

static const LXT_VARIATION g_LxtVariations[] = {{"EventFdVariation - read, write", EventFdVariationReadWrite}};

int EventfdTestEntry(int Argc, char* Argv[])

/*++
--*/

{

    LXT_ARGS Args;
    int Result;

    LxtCheckResult(LxtInitialize(Argc, Argv, &Args, LXT_NAME));
    LXT_SYNCHRONIZATION_POINT_INIT();
    LxtCheckResult(LxtRunVariations(&Args, g_LxtVariations, LXT_COUNT_OF(g_LxtVariations)));

ErrorExit:
    LXT_SYNCHRONIZATION_POINT_DESTROY();
    LxtUninitialize();
    return !LXT_SUCCESS(Result);
}

int EventFdVariationReadWrite(PLXT_ARGS Args)

/*++
--*/

{

    pid_t ChildPid;
    struct epoll_event EpollEvent;
    int EpollFd;
    int Fd;
    int Index;
    fd_set ReadFds;
    int Result;
    ssize_t Size;
    int Status;
    struct timeval Timeout;
    uint64_t Value;
    uint64_t Values[] = {1, 2, 10, 0xFFFFFFFF, 0xFFFFFFFFFFFFFFFE};
    struct
    {
        uint64_t Value;
        char Buffer[32];
    } LargeValue;

    EpollFd = -1;
    LXT_SYNCHRONIZATION_POINT_START();

    //
    // FIXME: Add 0 variation
    //

    //
    // Write the values to the event and read them back out single threaded.
    //

    LxtCheckErrno(Fd = eventfd(0, 0));
    for (Index = 0; Index < LXT_COUNT_OF(Values); Index += 1)
    {

        //
        // Verify that the event is not ready for read (value is zero).
        //

        memset(&Timeout, 0, sizeof(Timeout));
        FD_ZERO(&ReadFds);
        FD_SET(Fd, &ReadFds);
        LxtCheckErrno(select((Fd + 1), &ReadFds, NULL, NULL, &Timeout));
        LxtCheckEqual(Result, 0, "%d");

        Value = Values[Index];
        Size = write(Fd, &Value, sizeof(Value));
        LxtCheckEqual(Size, sizeof(Value), "%lld");
        LxtCheckEqual(Value, Values[Index], "%llu");

        //
        // Verify that the event is ready for read (value is NOT zero).
        //

        memset(&Timeout, 0, sizeof(Timeout));
        FD_ZERO(&ReadFds);
        FD_SET(Fd, &ReadFds);
        LxtCheckErrno(select((Fd + 1), &ReadFds, NULL, NULL, &Timeout));
        LxtCheckEqual(Result, 1, "%d");

        //
        // Check again to verify nothing has changed.
        //

        memset(&Timeout, 0, sizeof(Timeout));
        FD_ZERO(&ReadFds);
        FD_SET(Fd, &ReadFds);
        LxtCheckErrno(select((Fd + 1), &ReadFds, NULL, NULL, &Timeout));
        LxtCheckEqual(Result, 1, "%d");

        Value = 0;
        Size = read(Fd, &Value, sizeof(Value));
        LxtCheckEqual(Size, sizeof(Value), "%lld");
        LxtCheckEqual(Value, Values[Index], "%llu");
    }

    //
    // Verify that the event is not ready for read (value is zero).
    //

    memset(&Timeout, 0, sizeof(Timeout));
    FD_ZERO(&ReadFds);
    FD_SET(Fd, &ReadFds);
    LxtCheckErrno(select((Fd + 1), &ReadFds, NULL, NULL, &Timeout));
    LxtCheckEqual(Result, 0, "%d");

    //
    // Write the values to the event and read them back out on another thread.
    //

    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        LxtCheckErrno(EpollFd = epoll_create(1));
        EpollEvent.events = EPOLLIN | EPOLLET;
        EpollEvent.data.fd = Fd;
        LxtCheckErrno(epoll_ctl(EpollFd, EPOLL_CTL_ADD, Fd, &EpollEvent));
    }

    LXT_SYNCHRONIZATION_POINT();
    for (Index = 0; Index < LXT_COUNT_OF(Values); Index += 1)
    {
        if (ChildPid == 0)
        {

            //
            // Ignore the first value completely and don't read the second
            // value to verify that epoll edge-triggered behavior is working as
            // expected.
            //

            if (Index > 0)
            {
                if (Index == 1)
                {

                    //
                    // Since the first write was ignored, need to make sure
                    // that the second has completed so that the edge-triggered
                    // epoll logic works as expected instead of racing with the
                    // next write.
                    //

                    LXT_SYNCHRONIZATION_POINT();
                }

                LxtLogInfo("Waiting from child thread...");
                LxtCheckErrno(epoll_wait(EpollFd, &EpollEvent, 1, -1));
                LxtCheckEqual(Result, 1, "%d") LxtCheckEqual(EpollEvent.data.fd, Fd, "%d");

                //
                // The epoll should be blocked.
                //

                LxtCheckErrno(epoll_wait(EpollFd, &EpollEvent, 1, 0));
                LxtCheckEqual(Result, 0, "%d");

                //
                // Skip the first two values to verify epoll edge-triggered
                // behavior and then start verifying the value data.
                //

                if (Index > 1)
                {
                    Value = 0;
                    Size = read(Fd, &Value, sizeof(Value));
                    LxtCheckEqual(Size, sizeof(Value), "%lld");
                    if (Index == 2)
                    {
                        LxtCheckEqual(Value, (Values[0] + Values[1] + Values[2]), "%llu");
                    }
                    else
                    {
                        LxtCheckEqual(Value, Values[Index], "%llu");
                    }
                }

                //
                // The epoll should still be blocked.
                //

                LxtCheckErrno(epoll_wait(EpollFd, &EpollEvent, 1, 0));
                LxtCheckEqual(Result, 0, "%d")
            }
        }
        else
        {
            LxtLogInfo("Writing 0x%llx from parent thread...", Values[Index]);
            Value = Values[Index];
            Size = write(Fd, &Value, sizeof(Value));
            LxtCheckEqual(Size, sizeof(Value), "%lld");
            LxtCheckEqual(Value, Values[Index], "%llu");
            if (Index == 1)
            {
                LXT_SYNCHRONIZATION_POINT();
            }
        }

        LXT_SYNCHRONIZATION_POINT();
    }

    if (ChildPid == 0)
    {
        goto ErrorExit;
    }

    //
    // Read and write out with a buffer larger than the default.
    //

    memset(&LargeValue, 0, sizeof(LargeValue));
    LargeValue.Value = 1;
    Size = write(Fd, &LargeValue, sizeof(LargeValue));
    LxtCheckEqual(Size, sizeof(LargeValue.Value), "%lld");
    LxtCheckEqual(LargeValue.Value, 1, "%llu");

    LargeValue.Value = 0;
    Size = read(Fd, &LargeValue, sizeof(LargeValue));
    LxtCheckEqual(Size, sizeof(LargeValue.Value), "%lld");
    LxtCheckEqual(LargeValue.Value, 1, "%llu");

    //
    // Check the error code for writing the maximum value.
    //

    Value = 0xFFFFFFFFFFFFFFFF;
    LxtCheckErrnoFailure(write(Fd, &Value, sizeof(Value)), EINVAL);

    //
    // Check the error code for reading and writing a size of 0.
    //

    LxtCheckErrnoFailure(read(Fd, &Value, 0), EINVAL);
    LxtCheckErrnoFailure(write(Fd, &Value, 0), EINVAL);

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    if (Fd > 0)
    {
        close(Fd);
    }

    if (EpollFd > 0)
    {
        close(EpollFd);
    }

    LXT_SYNCHRONIZATION_POINT_END();
    return Result;
}
