/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    brk.c

Abstract:

    This file is the brk test.

--*/

#include "lxtcommon.h"
#include "unittests.h"
#include <unistd.h>
#include <stdio.h>

#define LXT_NAME "brk"

int BrkTest(PLXT_ARGS Args);

//
// Global constants
//

static const LXT_VARIATION g_LxtVariations[] = {
    {"Brk Test", BrkTest},
};

int BrkTestEntry(int Argc, char* Argv[])

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

int BrkTest(PLXT_ARGS Args)

/*++
--*/

{

    int Result;
    int Status;
    char* BreakAddress;
    char* NewBreakAddress;
    int PageSize;

    //
    // Get current brk address.
    //

    LxtLogInfo("Getting current break address");
    BreakAddress = (char*)sbrk(0);
    LxtLogInfo("Current break address is 0x%p", BreakAddress);

    //
    // Increase the brk address.
    //

    PageSize = 4096;
    BreakAddress += PageSize;
    Status = brk(BreakAddress);
    if (Status != 0)
    {
        LxtLogError("Brk call to increase the address failed");
        Result = LXT_RESULT_FAILURE;
        goto ErrorExit;
    }

    NewBreakAddress = (char*)sbrk(0);
    LxtLogInfo("New break address 0x%p", NewBreakAddress);
    if ((NewBreakAddress < BreakAddress) || (NewBreakAddress > (BreakAddress + PageSize)))
    {
        LxtLogError(
            "The returned brk address does not match the expected \
            break address");

        Result = LXT_RESULT_FAILURE;
        goto ErrorExit;
    }
    LxtLogInfo("New Break address set!");

    //
    // Decrease the break address.
    //

    BreakAddress = (NewBreakAddress - PageSize);
    LxtLogInfo("Decreasing the break address by a page");
    Status = brk(BreakAddress);
    if (Status != 0)
    {
        LxtLogError("Brk call to decrease the address failed");
        Result = LXT_RESULT_FAILURE;
        goto ErrorExit;
    }

    NewBreakAddress = (char*)sbrk(0);
    if (NewBreakAddress != BreakAddress)
    {
        LxtLogError(
            "The returned brk address after decreasing did not match\
            the expected break address");

        Result = LXT_RESULT_FAILURE;
        goto ErrorExit;
    }

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    return Result;
}