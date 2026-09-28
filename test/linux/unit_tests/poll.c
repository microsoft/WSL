/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    Template.c

Abstract:

    This file is the poll.

--*/

#include "lxtcommon.h"
#include "unittests.h"
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#define LXT_NAME "Poll"

int PollVariation0(PLXT_ARGS Args);

//
// Global constants
//

static const LXT_VARIATION g_LxtVariations[] = {{"Poll0", PollVariation0}};

int PollTestEntry(int Argc, char* Argv[])

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

int CountFilledDescriptors(struct pollfd* PollDescriptors, int Count)
{
    int NumberFilled;
    int Index;

    NumberFilled = 0;

    for (Index = 0; Index < Count; Index += 1)
    {

        if (PollDescriptors[Index].revents != 0)
        {
            NumberFilled += 1;
        }
    }

    return NumberFilled;
}

int PollVariation0(PLXT_ARGS Args)
{
    int Result;
    int FileDescriptor1;
    int FileDescriptor2;
    struct pollfd PollDescriptors[3];
    int NumberFilled;

    //
    // Initialize locals.
    //

    FileDescriptor1 = -1;
    FileDescriptor2 = -1;

    //
    // Open a file that will be used for select.
    //

    FileDescriptor1 = open("/data/test/poll_test.bin", O_RDWR | O_CREAT, S_IRWXU);

    if (FileDescriptor1 == -1)
    {
        Result = errno;
        LxtLogError("Could not create test file! %d", Result);
        goto cleanup;
    }

    FileDescriptor2 = open("/data/test/poll_test.bin", O_RDWR | O_CREAT, S_IRWXU);

    if (FileDescriptor2 == -1)
    {
        Result = errno;
        LxtLogError("Could not create test file! %d", Result);
        goto cleanup;
    }

    //
    // Fill the poll descriptors.
    //

    PollDescriptors[0].fd = FileDescriptor1;
    PollDescriptors[0].events = POLLIN;
    PollDescriptors[0].revents = -1;

    PollDescriptors[1].fd = FileDescriptor2;
    PollDescriptors[1].events = POLLRDHUP;
    PollDescriptors[1].revents = -1;

    PollDescriptors[2].fd = 100;
    PollDescriptors[2].events = POLLOUT;
    PollDescriptors[2].revents = -1;

    Result = poll(PollDescriptors, 3, 60001);

    if (Result == -1)
    {
        Result = errno;
        LxtLogError("Waiting on poll failed! %d", Result);
        goto cleanup;
    }

    if (Result != 2)
    {
        LxtLogError("Waiting on poll returned more than 2 events! %d", Result);
        Result = -1;
        goto cleanup;
    }

    NumberFilled = CountFilledDescriptors(PollDescriptors, 3);

    if (NumberFilled != Result)
    {
        LxtLogError("Poll returned %d events but filled %d!", Result, NumberFilled);
        Result = -1;
        goto cleanup;
    }

    if (PollDescriptors[2].revents != POLLNVAL)
    {
        LxtLogError("Poll descriptor 3 was filled incorrectly!");
        Result = -1;
        goto cleanup;
    }

    NumberFilled = CountFilledDescriptors(PollDescriptors, 3);

    if (NumberFilled != Result)
    {
        LxtLogError("Poll returned %d events but filled %d!", Result, NumberFilled);
        Result = -1;
        goto cleanup;
    }

    //
    // Poll with zero descriptors and it should time out.
    //

    LxtLogInfo("Wait for 1s for poll to timeout...");

    Result = poll(PollDescriptors, 0, 1000);

    if (Result == -1)
    {
        Result = errno;
        LxtLogError("Waiting on poll failed! %d", Result);
        goto cleanup;
    }

    if (Result != 0)
    {
        LxtLogError("Waiting on poll returned data but should have timed out! %d", Result);
        Result = -1;
        goto cleanup;
    }

    //
    // Poll for read/write but get notified of error.
    //

    read(FileDescriptor2, NULL, 111);

    PollDescriptors[1].fd = FileDescriptor2;
    PollDescriptors[1].events = POLLIN | POLLOUT;
    PollDescriptors[1].revents = -1;

    PollDescriptors[2].fd = -FileDescriptor2;
    PollDescriptors[2].events = POLLIN | POLLOUT;
    PollDescriptors[2].revents = -1;

    Result = poll(PollDescriptors, 3, -2);

    if (Result == -1)
    {
        Result = errno;
        LxtLogError("Waiting on poll failed! %d", Result);
        goto cleanup;
    }

    if (Result != 2)
    {
        LxtLogError("Waiting on poll returned more than 2 events! %d", Result);
        Result = -1;
        goto cleanup;
    }

    NumberFilled = CountFilledDescriptors(PollDescriptors, 3);

    if (NumberFilled != Result)
    {
        LxtLogError("Poll returned %d events but filled %d!", Result, NumberFilled);
        Result = -1;
        goto cleanup;
    }

    //
    // Poll for nothing but still got notified of error.
    //

    PollDescriptors[1].fd = FileDescriptor2;
    PollDescriptors[1].events = 0;
    PollDescriptors[1].revents = -1;

    PollDescriptors[2].fd = -FileDescriptor2;
    PollDescriptors[2].events = POLLIN | POLLOUT;
    PollDescriptors[2].revents = -1;

    Result = poll(PollDescriptors, 3, -2);

    if (Result == -1)
    {
        Result = errno;
        LxtLogError("Waiting on poll failed! %d", Result);
        goto cleanup;
    }

    if (Result != 1)
    {
        LxtLogError("Waiting on poll returned more than 1 events! %d", Result);
        Result = -1;
        goto cleanup;
    }

    NumberFilled = CountFilledDescriptors(PollDescriptors, 3);

    if (NumberFilled != Result)
    {
        LxtLogError("Poll returned %d events but filled %d!", Result, NumberFilled);
        Result = -1;
        goto cleanup;
    }

    NumberFilled = CountFilledDescriptors(PollDescriptors, 1);

    if (NumberFilled != Result)
    {
        LxtLogError("Poll returned %d events but filled %d!", Result, NumberFilled);
        Result = -1;
        goto cleanup;
    }

    Result = LXT_RESULT_SUCCESS;

cleanup:

    if (FileDescriptor1 != -1)
    {
        close(FileDescriptor1);
    }

    if (FileDescriptor2 != -1)
    {
        close(FileDescriptor2);
    }

    return Result;
}
