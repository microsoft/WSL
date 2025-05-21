/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    timerfd.c

Abstract:

    This file is a timerfd test.

--*/

#include "lxtcommon.h"
#include "unittests.h"
// #include <sys/timerfd.h>
#include <sys/epoll.h>
#include <sys/time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>

#define LXT_NAME "timerfd"
#define LXT_EVENT_ARRAY_SIZE (10)
#define LXT_BASIC_TEST_LOOP_COUNT (3)

int TimerFdBasic(PLXT_ARGS Args);

int TimerFdEpoll(PLXT_ARGS Args);

//
// Global constants
//

static const LXT_VARIATION g_LxtVariations[] = {{"Timerfd Basic", TimerFdBasic}, {"Timerfd Epoll", TimerFdEpoll}};

//
// Global variables
//

static struct timespec g_SignalTime;
static int g_SignalCount;

int TimerFdTestEntry(int Argc, char* Argv[])

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

int TimerFdBasic(PLXT_ARGS Args)

/*++
--*/

{

    struct epoll_event Event;
    struct epoll_event Events[LXT_EVENT_ARRAY_SIZE];
    int EpollFd;
    struct stat FileStat;
    int Flags;
    int Index;
    int LoopIndex;
    struct itimerspec NewTimer;
    struct itimerspec OldTimer;
    int ReadyFdCount;
    int Result;
    struct timespec SleepDuration;
    uint64_t TimerExpirationCount;
    int TimerFd;

    //
    // Initialize locals.
    //

    EpollFd = -1;
    TimerFd = -1;

    //
    // Create invalid timers.
    //

    TimerFd = timerfd_create(-1, O_NONBLOCK);
    if (TimerFd >= 0)
    {
        LxtLogError("timerfd_create was supposed to fail because of invalid parameters but did not fail.");
        Result = TimerFd;
        goto ErrorExit;
    }

    TimerFd = timerfd_create(CLOCK_MONOTONIC, -1);
    if (TimerFd >= 0)
    {
        LxtLogError("timerfd_create was supposed to fail because of invalid parameters but did not fail.");
        Result = TimerFd;
        goto ErrorExit;
    }

    TimerFd = timerfd_create(-1, -1);
    if (TimerFd >= 0)
    {
        LxtLogError("timerfd_create was supposed to fail because of invalid parameters but did not fail.");
        Result = TimerFd;
        goto ErrorExit;
    }

    //
    // Create a timer fd.
    //

    TimerFd = timerfd_create(CLOCK_MONOTONIC, O_NONBLOCK);
    if (TimerFd < 0)
    {
        LxtLogError("timerfd_create failed");
        Result = TimerFd;
        goto ErrorExit;
    }

    //
    // Create epoll fd to monitor the file for read events.
    //

    EpollFd = epoll_create(LXT_EVENT_ARRAY_SIZE);
    if (EpollFd < 0)
    {
        LxtLogError("epoll create failed");
        Result = EpollFd;
        goto ErrorExit;
    }

    Event.events = EPOLLIN;
    Event.data.fd = TimerFd;
    Result = epoll_ctl(EpollFd, EPOLL_CTL_ADD, TimerFd, &Event);
    if (Result < 0)
    {
        LxtLogError("epoll_ctl failed.");
        goto ErrorExit;
    }

    //
    // Set timer fd to expire at one second interval.
    //

    Flags = 0;
    NewTimer.it_interval.tv_sec = 1;
    NewTimer.it_interval.tv_nsec = 0;
    NewTimer.it_value.tv_sec = 1;
    NewTimer.it_value.tv_nsec = 0;

    //
    // Set timer with invalid parameters.
    //

    Result = timerfd_settime(-1, Flags, &NewTimer, &OldTimer);
    if (Result >= 0)
    {
        LxtLogError("timerfd_settime was supposed to fail because of invalid parameters but did not fail.");
        goto ErrorExit;
    }

    //
    // Set timer with valid parameters.
    //

    Result = timerfd_settime(TimerFd, Flags, &NewTimer, &OldTimer);
    if (Result < 0)
    {
        LxtLogError("timerfd_settime failed");
        goto ErrorExit;
    }

    //
    // Read with invalid buffer size.
    //

    Result = read(TimerFd, &TimerExpirationCount, sizeof(TimerExpirationCount) - 1);
    if (Result >= 0)
    {
        LxtLogError("read was supposed to fail because of invalid parameters but did not fail.");
        goto ErrorExit;
    }

    //
    // Loop three times. In each iteration increase the wait time by one seconds.
    // this should increase the expiration count by one in each iteration.
    //

    for (LoopIndex = 1; LoopIndex <= LXT_BASIC_TEST_LOOP_COUNT; LoopIndex += 1)
    {

        //
        // Sleep for a factor of a second.
        //

        SleepDuration.tv_sec = LoopIndex;
        SleepDuration.tv_nsec = 0;
        nanosleep(&SleepDuration, NULL);
        ReadyFdCount = epoll_wait(EpollFd, Events, LXT_EVENT_ARRAY_SIZE, 0);
        if (ReadyFdCount < 0)
        {
            LxtLogError("epoll_wait failed.");
            Result = ReadyFdCount;
            goto ErrorExit;
        }

        for (Index = 0; Index < ReadyFdCount; Index += 1)
        {

            //
            // Look out for TimerFd.
            //

            if (Events[Index].data.fd == TimerFd)
            {
                TimerExpirationCount = 0;
                if (read(TimerFd, &TimerExpirationCount, sizeof(TimerExpirationCount)) != -1)
                {
                    LxtLogInfo("Number of times the timer expired in %d seconds is : %lld", SleepDuration.tv_sec, (long long)TimerExpirationCount);
                }
            }
        }
    }

    //
    // Read even before the timer has expired. Set a 10 second expiration window
    // and do a read after that. The read should fail with EAGAIN.
    //

    Flags = 0;
    NewTimer.it_interval.tv_sec = 10;
    NewTimer.it_interval.tv_nsec = 0;
    Result = timerfd_settime(TimerFd, Flags, &NewTimer, &OldTimer);
    if (Result < 0)
    {
        LxtLogError("timerfd_settime failed");
        goto ErrorExit;
    }

    Result = read(TimerFd, &TimerExpirationCount, sizeof(TimerExpirationCount));
    if (Result != -1)
    {
        LxtLogError("Read was supposed to fail with eagain but it did not %d", Result);
    }

    //
    // Call get time with invalid parameters.
    //

    Result = timerfd_gettime(-1, &OldTimer);
    if (Result >= 0)
    {
        LxtLogError("timerfd_gettime was supposed to fail because of invalid parameters but did not fail.");
        goto ErrorExit;
    }

    Result = timerfd_gettime(TimerFd, NULL);
    if (Result >= 0)
    {
        LxtLogError("timerfd_gettime was supposed to fail because of invalid parameters but did not fail.");
        goto ErrorExit;
    }

    Result = timerfd_gettime(-1, NULL);
    if (Result >= 0)
    {
        LxtLogError("timerfd_gettime was supposed to fail because of invalid parameters but did not fail.");
        goto ErrorExit;
    }

    NewTimer.it_value.tv_sec = 60;
    NewTimer.it_interval.tv_sec = 60;
    Result = timerfd_settime(TimerFd, Flags, &NewTimer, &OldTimer);
    if (Result < 0)
    {
        LxtLogError("timerfd_settime failed");
        goto ErrorExit;
    }

    Result = timerfd_gettime(TimerFd, &OldTimer);
    if (Result < 0)
    {
        LxtLogError("timerfd_gettime failed.");
        goto ErrorExit;
    }

    LxtLogInfo(
        "Current timer settings: Interval (seconds: %d, nanoseconds: %lu),"
        "time till next expiration (seconds %d, nanoseconds %ld, %d)",
        OldTimer.it_interval.tv_sec,
        OldTimer.it_interval.tv_nsec,
        OldTimer.it_value.tv_sec,
        OldTimer.it_value.tv_nsec,
        OldTimer.it_value.tv_nsec);

    //
    // Stat on the TimerFd.
    //

    Result = fstat(TimerFd, &FileStat);
    if (Result < 0)
    {
        LxtLogError("stat on timerfd failed.");
        goto ErrorExit;
    }

    LxtLogInfo("File Size: %d bytes", FileStat.st_size);
    LxtLogInfo("Number of Links: %d", FileStat.st_nlink);
    LxtLogInfo("File inode: %d", FileStat.st_ino);
    LxtLogInfo("Symbolic link: %c ", (S_ISLNK(FileStat.st_mode)) ? 'Y' : 'N');
    LxtLogInfo("Mode: %d", FileStat.st_mode);
    LxtLogInfo("Mode: %lu", FileStat.st_mode);

    //
    // Invalid parameter variations.
    //

    memset(&NewTimer, 0, sizeof(NewTimer));
    NewTimer.it_value.tv_sec = -1;
    LxtCheckErrnoFailure(timerfd_settime(TimerFd, 0, &NewTimer, NULL), EINVAL);
    memset(&NewTimer, 0, sizeof(NewTimer));
    NewTimer.it_value.tv_nsec = 999999999 + 1;
    LxtCheckErrnoFailure(timerfd_settime(TimerFd, 0, &NewTimer, NULL), EINVAL);
    memset(&NewTimer, 0, sizeof(NewTimer));
    NewTimer.it_interval.tv_sec = -1;
    LxtCheckErrnoFailure(timerfd_settime(TimerFd, 0, &NewTimer, NULL), EINVAL);
    memset(&NewTimer, 0, sizeof(NewTimer));
    NewTimer.it_interval.tv_nsec = 999999999 + 1;
    LxtCheckErrnoFailure(timerfd_settime(TimerFd, 0, &NewTimer, NULL), EINVAL);

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    if (EpollFd > 0)
    {
        LxtClose(EpollFd);
    }

    if (TimerFd > 0)
    {
        LxtClose(TimerFd);
    }

    return Result;
}

int TimerFdEpoll(PLXT_ARGS Args)

/*++

Routine Description:

    This routine validates the various epoll states of timer FD.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    struct epoll_event Event;
    struct epoll_event Events[LXT_EVENT_ARRAY_SIZE];
    int EpollFd;
    struct itimerspec NewTimer;
    struct itimerspec OldTimer;
    int ReadyFdCount;
    int Result;
    struct timespec SleepDuration;
    uint64_t TimerExpirationCount;
    int TimerFd;

    //
    // Initialize locals.
    //

    EpollFd = -1;
    TimerFd = -1;

    //
    // Create a timer fd.
    //

    LxtCheckErrno((TimerFd = timerfd_create(CLOCK_MONOTONIC, O_NONBLOCK)));

    //
    // Create epoll fd to monitor the file for read events.
    //

    LxtCheckErrno((EpollFd = epoll_create(LXT_EVENT_ARRAY_SIZE)));

    //
    // EPOLLIN is the only interesting event for timer FD.
    //

    Event.events = EPOLLIN;
    Event.data.fd = TimerFd;
    LxtCheckErrno(epoll_ctl(EpollFd, EPOLL_CTL_ADD, TimerFd, &Event));

    //
    // Soon after creation, there shouldn't be any epoll set on the timer FD.
    //

    LxtCheckErrno((ReadyFdCount = epoll_wait(EpollFd, &Event, 1, 10)));
    LxtCheckEqual(ReadyFdCount, 0, "%d");

    //
    // Set the timer to fire soon (1ms).
    //

    NewTimer.it_interval.tv_sec = 0;
    NewTimer.it_interval.tv_nsec = 0;
    NewTimer.it_value.tv_sec = 0;
    NewTimer.it_value.tv_nsec = 1000;

    //
    // Set the timer.
    //

    LxtCheckErrno(timerfd_settime(TimerFd, 0, &NewTimer, &OldTimer));

    //
    // Wait for EPOLL for a timeout > timer. `epoll_wait` takes time in ms.
    //

    LxtCheckErrno((ReadyFdCount = epoll_wait(EpollFd, &Event, 1, 1000)));
    LxtCheckEqual(ReadyFdCount, 1, "%d");

    //
    // Setting the timer to 0 should reset the timer and the epoll.
    //

    NewTimer.it_interval.tv_sec = 0;
    NewTimer.it_interval.tv_nsec = 0;
    NewTimer.it_value.tv_sec = 0;
    NewTimer.it_value.tv_nsec = 0;
    LxtCheckErrno(timerfd_settime(TimerFd, 0, &NewTimer, &OldTimer));
    LxtCheckErrno((ReadyFdCount = epoll_wait(EpollFd, &Event, 1, 10)));
    LxtCheckEqual(ReadyFdCount, 0, "%d");
    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    if (EpollFd > 0)
    {
        close(EpollFd);
    }

    if (TimerFd > 0)
    {
        close(TimerFd);
    }

    return Result;
}
