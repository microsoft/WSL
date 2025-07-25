/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    sched.c

Abstract:

    This file is the scheduler test.

--*/

#include "lxtcommon.h"
#include "unittests.h"
#include <sched.h>
#include <stdio.h>
#include <sys/syscall.h>

#define LXT_NAME "sched"

int GetDefaultScheduler(PLXT_ARGS Args);

int SetScheduler(PLXT_ARGS Args);

int SetSchedulerChild(PLXT_ARGS Args);

int SetGetAffinity(PLXT_ARGS Args);

int SetGetAffinityNp(PLXT_ARGS Args);

//
// Global constants
//

static const LXT_VARIATION g_LxtVariations[] = {
    {"Get Scheduler Default", GetDefaultScheduler},
    {"Set Scheduler", SetScheduler},
    {"Set-Get Affinity", SetGetAffinity},
    {"Set-Get Affinity np", SetGetAffinityNp}};

int SchedTestEntry(int Argc, char* Argv[])

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

int GetDefaultScheduler(PLXT_ARGS Args)

/*++
--*/

{

    int Result;
    int Policy;

    Policy = sched_getscheduler(0);
    LxtLogInfo("Policy received %d", Policy);
    if (Policy == SCHED_OTHER)
    {
        Result = LXT_RESULT_SUCCESS;
    }
    else
    {
        LxtLogError("Bad policy. Expected(%d) != Returned(%d)", SCHED_OTHER, Policy);

        Result = LXT_RESULT_FAILURE;
    }

ErrorExit:
    return Result;
}

int SetScheduler(PLXT_ARGS Args)

/*++
--*/

{

    int Result;
    struct sched_param Param;
    int Policy;

    Policy = sched_getscheduler(0);
    LxtLogInfo("Policy received %d", Policy);
    if (Policy < 0)
    {
        LxtLogError("Bad policy. errno %d = %s", errno, strerror(errno));

        Result = LXT_RESULT_FAILURE;
        goto ErrorExit;
    }

    //
    // Set a different policy
    //

    if (Policy == SCHED_OTHER)
    {
        Policy = SCHED_FIFO;
    }
    else
    {
        Policy = SCHED_OTHER;
    }

    LxtLogInfo("Setting policy %d", Policy);
    Param.sched_priority = 17;
    Result = sched_setscheduler(0, Policy, &Param);
    if (Result < 0)
    {
        LxtLogError("Set scheduler failed errno %d = %s", errno, strerror(errno));

        Result = LXT_RESULT_FAILURE;
        goto ErrorExit;
    }

    Result = sched_getscheduler(0);
    LxtLogInfo("Policy received %d", Result);
    if (Policy != Result)
    {
        LxtLogError("Bad policy. Expected(%d) != Returned(%d)", Policy, Result);

        Result = LXT_RESULT_FAILURE;
        goto ErrorExit;
    }

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    return Result;
}

int SetSchedulerChild(PLXT_ARGS Args)

/*++
--*/

{

    int Result;
    int Policy;
    int Pid;

    //
    // The child should inherit this scheduler
    //

    sched_setscheduler(0, SCHED_OTHER, NULL);

    Pid = fork();
    if (Pid != 0)
    {
        sleep(1);

        Result = sched_setscheduler(Pid, SCHED_FIFO, NULL);
        if (Result < 0)
        {
            LxtLogError("Set scheduler failed errno %d = %s", errno, strerror(errno));

            Result = LXT_RESULT_FAILURE;
            goto ErrorExit;
        }

        sleep(2);
    }
    else
    {
        Policy = sched_getscheduler(0);
        LxtLogInfo("Child - Policy gotten %d", Policy);
        if (Policy != SCHED_OTHER)
        {
            LxtLogError("Bad policy. Expected(%d) != Returned(%d)", SCHED_OTHER, Policy);

            Result = LXT_RESULT_FAILURE;
            goto ErrorExit;
        }
        sleep(2);

        Policy = sched_getscheduler(0);
        printf("Child - Policy gotten %d", Policy);
        if (Policy != SCHED_FIFO)
        {
            LxtLogError("Bad policy. Expected(%d) != Returned(%d)", SCHED_FIFO, Policy);

            Result = LXT_RESULT_FAILURE;
            goto ErrorExit;
        }
    }

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    return Result;
}

int SetGetAffinity(PLXT_ARGS Args)

/*++
--*/

{

    int Result;
    int Size = -1;
    cpu_set_t Set;
    cpu_set_t Desired;
    int Sizes[] = {-8, 8, 16, 24, 32, 40, 64, 128, 256};
    int SizeExpected;
    int Index;

    CPU_ZERO(&Set);

    LxtLogInfo("sizeof(cpu_set_t) = %Iu", sizeof(cpu_set_t));
    LxtCheckErrno(Size = LxtSched_GetAffinity(0, sizeof(Set), &Set));
    LxtCheckEqual(Size, 64, "%d");
    LxtLogInfo("Affinity before: %08x", *(uint32_t*)&Set);
    CPU_ZERO(&Desired);
    CPU_SET(0, &Desired);
    LxtCheckErrno(LxtSched_SetAffinity(0, 1, &Desired));
    LxtCheckErrno(Size = LxtSched_GetAffinity(0, sizeof(Set), &Set));
    LxtCheckEqual(Size, 64, "%d");
    LxtCheckErrno(LxtSched_SetAffinity(0, 3, &Desired));
    LxtCheckErrno(Size = LxtSched_GetAffinity(0, sizeof(Set), &Set));
    LxtCheckEqual(Size, 64, "%d");
    LxtCheckErrno(LxtSched_SetAffinity(0, sizeof(Desired), &Desired));
    LxtCheckErrno(Size = LxtSched_GetAffinity(0, sizeof(Set), &Set));
    LxtCheckEqual(Size, 64, "%d");
    LxtLogInfo("Affinity after: %08x", *(uint32_t*)&Set);
    if (!CPU_EQUAL(&Set, &Desired))
    {
        LxtLogError("sched_setaffinity failed to set the affinity. ");
        Result = LXT_RESULT_FAILURE;
        goto ErrorExit;
    }

    //
    // Test with various buffer sizes.
    //

    for (Index = 0; Index < (int)LXT_COUNT_OF(Sizes); Index++)
    {
        LxtLogInfo("Testing size %d", Sizes[Index]);
        LxtCheckErrno(Size = LxtSched_GetAffinity(0, Sizes[Index], &Set));
        SizeExpected = Sizes[Index];
        if ((SizeExpected > 64) || (SizeExpected < 0))
        {
            SizeExpected = 64;
        }

        LxtCheckEqual(Size, SizeExpected, "%d");
        if (!CPU_EQUAL(&Set, &Desired))
        {
            LxtLogError("sched_setaffinity failed to set the affinity. ");
            Result = LXT_RESULT_FAILURE;
            goto ErrorExit;
        }
    }

    LxtCheckErrno(Size = LxtSched_GetAffinity(getpid(), sizeof(Set), &Set));

    //
    // Invalid parameter variations.
    //

    LxtCheckErrnoFailure(LxtSched_GetAffinity(0, 0, &Set), EINVAL);
    LxtCheckErrnoFailure(LxtSched_GetAffinity(0, 1, &Set), EINVAL);
    LxtCheckErrnoFailure(LxtSched_GetAffinity(0, 2, &Set), EINVAL);
    LxtCheckErrnoFailure(LxtSched_GetAffinity(0, 7, &Set), EINVAL);
    LxtCheckErrnoFailure(LxtSched_GetAffinity(0, 9, &Set), EINVAL);
    LxtCheckErrnoFailure(LxtSched_GetAffinity(0, 10, &Set), EINVAL);
    LxtCheckErrnoFailure(LxtSched_GetAffinity(0, 31, &Set), EINVAL);
    LxtCheckErrnoFailure(LxtSched_GetAffinity(0, 33, &Set), EINVAL);
    LxtCheckErrnoFailure(LxtSched_GetAffinity(0, 63, &Set), EINVAL);
    LxtCheckErrnoFailure(LxtSched_GetAffinity(0, 65, &Set), EINVAL);
    LxtCheckErrnoFailure(LxtSched_GetAffinity(0, -1, &Set), EINVAL);
    LxtCheckErrnoFailure(LxtSched_GetAffinity(0, -63, &Set), EINVAL);
    LxtCheckErrnoFailure(LxtSched_GetAffinity(0, -1, NULL), EINVAL);
    LxtCheckErrnoFailure(LxtSched_GetAffinity(0, -1, -1), EINVAL);
    LxtCheckErrnoFailure(LxtSched_GetAffinity(-1, -1, &Set), EINVAL);
    LxtCheckErrnoFailure(LxtSched_GetAffinity(0, sizeof(Set), NULL), EFAULT);
    LxtCheckErrnoFailure(LxtSched_GetAffinity(0, sizeof(Set), -1), EFAULT);
    LxtCheckErrnoFailure(LxtSched_GetAffinity(-1, sizeof(Set), &Set), ESRCH);
    LxtCheckErrnoFailure(LxtSched_GetAffinity(-1, sizeof(Set), NULL), ESRCH);
    LxtCheckErrnoFailure(LxtSched_GetAffinity(-1, sizeof(Set), -1), ESRCH);
    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    return Result;
}

int SetGetAffinityNp(PLXT_ARGS Args)

/*++
--*/

{

    int Result;
    cpu_set_t Set;
    CPU_ZERO(&Set);
    CPU_SET(0, &Set);
    LxtCheckErrno(LxtSched_SetAffinity(0, sizeof(Set), &Set));
    LxtCheckErrno(LxtSched_GetAffinity(0, sizeof(Set), &Set));

    //
    // N.B Affinity cannot be validated because its not guaranteed for it to
    //     take effect.
    //

    LxtLogInfo("Current Affinity: %08x", *(uint32_t*)&Set);
    CPU_ZERO(&Set);
    CPU_SET(1, &Set);
    LxtCheckErrno(LxtSched_SetAffinity(0, sizeof(Set), &Set));
    LxtCheckErrno(LxtSched_GetAffinity(0, sizeof(Set), &Set));
    LxtLogInfo("Current Affinity: %08x", *(uint32_t*)&Set);
    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    return Result;
}
