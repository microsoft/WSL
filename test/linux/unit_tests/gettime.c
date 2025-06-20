/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    gettime.c

Abstract:

    This file is a test utility, not a formal test.

--*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <netdb.h>
#include <time.h>
#include "lxtcommon.h"
#include "unittests.h"

#define LXT_NAME "gettime"

struct timespec diff(struct timespec start, struct timespec end);

int GetTimeTestEntry(int Argc, char* Argv[])
{
    LXT_ARGS Args;
    int ClockId;
    int i;
    struct timespec ProcessTime1, ProcessTime2;
    struct timespec Resolution;
    struct timespec ThreadTime1, ThreadTime2;
    int Result;
    int temp;

    LxtCheckResult(LxtInitialize(Argc, Argv, &Args, LXT_NAME));

    //
    // Get clock resolutions.
    //

    for (i = 0; i < 0x20; i++)
    {
        ClockId = (-1 & ~0x7) | i;
        Result = LxtClockGetRes(ClockId, &Resolution);
        LxtLogInfo("ClockId %x First 3 bits %x Result %d %d", ClockId, i, Result, errno);
        errno = 0;
    }

    //
    // Test thread and process clocks.
    //

    LxtClockGetTime(CLOCK_THREAD_CPUTIME_ID, &ThreadTime1);
    LxtClockGetTime(CLOCK_PROCESS_CPUTIME_ID, &ProcessTime1);
    LxtLogInfo("ThreadTime1.tv_sec %d ThreadTime1.tv_nsec %d", ThreadTime1.tv_sec, ThreadTime1.tv_nsec);

    LxtLogInfo("ProcessTime1.tv_sec %d ProcessTime1.tv_nsec %d", ProcessTime1.tv_sec, ProcessTime1.tv_nsec);

    for (i = 0; i < 1000; i++)
    {
        LxtClockGetTime(CLOCK_THREAD_CPUTIME_ID, &ThreadTime2);
        LxtLogInfo("ThreadTime2.tv_sec %d ThreadTime2.tv_nsec %d", ThreadTime2.tv_sec, ThreadTime2.tv_nsec);

        LxtClockGetTime(CLOCK_PROCESS_CPUTIME_ID, &ProcessTime2);
        LxtLogInfo("ProcessTime2.tv_sec %d ProcessTime2.tv_nsec %d", ProcessTime2.tv_sec, ProcessTime2.tv_nsec);
    }

    LxtLogInfo(
        "diff(ThreadTime1,ThreadTime2).tv_sec %d diff(ThreadTime1,ThreadTime2).tv_nsec %d",
        diff(ThreadTime1, ThreadTime2).tv_sec,
        diff(ThreadTime1, ThreadTime2).tv_nsec);

    LxtLogInfo(
        "diff(ProcessTime1,ProcessTime2).tv_sec %d diff(ProcessTime1,ProcessTime2).tv_nsec %d",
        diff(ProcessTime1, ProcessTime2).tv_sec,
        diff(ProcessTime1, ProcessTime2).tv_nsec);

ErrorExit:
    LxtUninitialize();
    return 0;
}

struct timespec diff(struct timespec start, struct timespec end)
{
    struct timespec temp;
    if ((end.tv_nsec - start.tv_nsec) < 0)
    {
        temp.tv_sec = end.tv_sec - start.tv_sec - 1;
        temp.tv_nsec = 1000000000 + end.tv_nsec - start.tv_nsec;
    }
    else
    {
        temp.tv_sec = end.tv_sec - start.tv_sec;
        temp.tv_nsec = end.tv_nsec - start.tv_nsec;
    }
    return temp;
}
