/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WaitPid.c

Abstract:

    This file is a WaitPid test.

--*/

#include "lxtcommon.h"
#include "unittests.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/prctl.h>
#include <stdio.h>
#include <stdlib.h>
#include "lxtutil.h"

#define LXT_NAME "WaitPid"

#define WAITPID_DEFAULT_WAIT_TIMEOUT_US 100000
#define WAITPID_DEFAULT_WAIT_COUNT 20
#define WAITPID_THREADGROUP_LEADER_UID 1044
#define WAITPID_PTHREAD_UID 1055

bool g_VmMode = false;

int GetPPidPoll(pid_t ExpectedPid);

LXT_VARIATION_HANDLER WaitPidVariationExitStatusBlock;

int WaitPidVariationExitStatusHelper(PLXT_ARGS Args, int Blocking);

LXT_VARIATION_HANDLER WaitPidVariationExitStatusPoll;
LXT_VARIATION_HANDLER WaitPidVariationInitPid;
LXT_VARIATION_HANDLER WaitPidVariationParentChild;
LXT_VARIATION_HANDLER WaitPidVariationProcessGroup;
LXT_VARIATION_HANDLER WaitPidVariationInvalidParameter;
LXT_VARIATION_HANDLER WaitPidVariationWaitId;
LXT_VARIATION_HANDLER WaitPidVariationCloneParent;
LXT_VARIATION_HANDLER WaitPidVariationZombie;
LXT_VARIATION_HANDLER WaitPidVariationZombieStress;

//
// Global constants
//
// FIXME: Enable parent\child test when clone gs issue is resolved.
//

static const LXT_VARIATION g_LxtVariations[] = {
    {"WaitPidVariation - Exit status poll", WaitPidVariationExitStatusPoll},
    {"WaitPidVariation - Exit status block", WaitPidVariationExitStatusBlock},
    {"WaitPidVariation - Init pid", WaitPidVariationInitPid},
    {"WaitPidVariation - Process groups", WaitPidVariationProcessGroup},
    {"WaitPidVariation - Invalid parameter", WaitPidVariationInvalidParameter},
    {"WaitPidVariation - waitid", WaitPidVariationWaitId},
    {"WaitPidVariation - CLONE_PARENT", WaitPidVariationCloneParent},
    {"WaitPidVariation - zombie support", WaitPidVariationZombie},
    {"WaitPidVariation - zombie stress", WaitPidVariationZombieStress}};

int WaitPidTestEntry(int Argc, char* Argv[])

/*++
--*/

{

    LXT_ARGS Args;
    int Result;

    if (LxtWslVersion() == 2)
    {
        g_VmMode = true;
    }

    LxtCheckResult(LxtInitialize(Argc, Argv, &Args, LXT_NAME));
    LXT_SYNCHRONIZATION_POINT_INIT();
    LxtCheckResult(LxtRunVariations(&Args, g_LxtVariations, LXT_COUNT_OF(g_LxtVariations)));

ErrorExit:
    LXT_SYNCHRONIZATION_POINT_DESTROY();
    LxtUninitialize();
    return !LXT_SUCCESS(Result);
}

int GetPPidPoll(pid_t ExpectedPPid)

/*++
--*/

{

    pid_t CurrentPPid;
    int Result;
    int WaitCount;

    //
    // Wait for the current ppid to reach the expected ppid.
    //

    for (WaitCount = 0; WaitCount < WAITPID_DEFAULT_WAIT_COUNT; ++WaitCount)
    {
        CurrentPPid = getppid();
        if (CurrentPPid == ExpectedPPid)
        {
            break;
        }

        usleep(WAITPID_DEFAULT_WAIT_TIMEOUT_US);
    }

    if (WaitCount == WAITPID_DEFAULT_WAIT_COUNT)
    {
        Result = LXT_RESULT_FAILURE;
        LxtLogError("Unexpected pid, %d != %d", CurrentPPid, ExpectedPPid);
        goto ErrorExit;
    }

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    return Result;
}

int LxtWaitPidHelper(pid_t ChildPid, int ExpectedWaitStatus, int Blocking)

/*++
--*/

{

    int SecondWaitPidStatus;
    int Result;
    int WaitCount;
    int WaitPidResult;
    int WaitPidStatus;

    if (Blocking == 0)
    {
        LxtCheckResult(LxtWaitPidPoll(ChildPid, ExpectedWaitStatus));
    }
    else
    {
        LxtCheckErrno((WaitPidResult = waitpid(ChildPid, &WaitPidStatus, 0)));
        if ((WaitPidStatus & 0xFFFF0000) != 0)
        {
            Result = LXT_RESULT_FAILURE;
            LxtLogError("Unexpected high short status: %d - %d", WaitPidStatus, ExpectedWaitStatus);

            goto ErrorExit;
        }

        if (WaitPidStatus != ExpectedWaitStatus)
        {
            Result = LXT_RESULT_FAILURE;
            LxtLogError("Unexpected status: %d != %d", WaitPidStatus, ExpectedWaitStatus);

            goto ErrorExit;
        }

        if (WIFEXITED(WaitPidStatus) != 0)
        {
            LxtCheckErrnoFailure(waitpid(ChildPid, &SecondWaitPidStatus, WNOHANG), ECHILD);
        }
    }

ErrorExit:
    return Result;
}

int WaitPidVariationInitPid(PLXT_ARGS Args)

/*++
--*/

{

    pid_t ChildPid;
    int ExitCodeIndex;
    unsigned char ExitCode;
    int ExpectedPid;
    int ExpectedWaitStatus;
    pid_t ParentParentPid;
    LXT_PIPE Pipe = {-1, -1};
    int Result;
    pid_t WaitPidResult;
    int WaitPidStatus;
    int WaitPipe;

    //
    // Determine who the subreaper of the test is. On WSL 1 it will be init, on
    // WSL 2 it will be the relay process (parent of this process).
    //

    ExpectedPid = LXT_INIT_PID;
    if (g_VmMode != false)
    {
        ExpectedPid = getppid();
    }

    LxtCheckResult(LxtCreatePipe(&Pipe));

    //
    // Check when a parent dies, the child has a parent pid of the subreaper.
    //

    ExitCode = 0;
    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        LxtCheckErrno(ChildPid = fork());
        if (ChildPid == 0)
        {
            LxtCheckResult(GetPPidPoll(ExpectedPid));
            LxtCheckErrno(write(Pipe.Write, &WaitPipe, sizeof(WaitPipe)));
        }

        _exit(ExitCode);
    }
    else
    {
        LxtCheckResult(LxtWaitPidPoll(-1, ExitCode));
        LxtCheckErrno(read(Pipe.Read, &WaitPipe, sizeof(WaitPipe)));
    }

    //
    // The init process should never be a child.
    //

    LxtCheckErrnoFailure(waitpid(LXT_INIT_PID, &WaitPidStatus, WNOHANG), ECHILD);

ErrorExit:
    LxtClosePipe(&Pipe);

    return Result;
}

int WaitPidVariationExitStatusBlock(PLXT_ARGS Args)

/*++
--*/

{

    return WaitPidVariationExitStatusHelper(Args, 1);
}

int WaitPidVariationExitStatusHelper(PLXT_ARGS Args, int Blocking)

/*++
--*/

{

    int ExitCodeIndex;
    unsigned char ExitCodes[] = {0, 1, 128, 255};
    pid_t ChildPid[LXT_COUNT_OF(ExitCodes)];
    int ExpectedWaitStatus;
    pid_t ParentParentPid;
    pid_t ParentPid;
    int Result;

    //
    // Check that the correct _exit status is returned to a parent process and
    // that it can only be checked once serially.
    //

    ParentParentPid = getppid();
    ParentPid = getpid();
    for (ExitCodeIndex = 0; ExitCodeIndex < LXT_COUNT_OF(ExitCodes); ++ExitCodeIndex)
    {
        LxtCheckErrno(ChildPid[ExitCodeIndex] = fork());
        if (ChildPid[ExitCodeIndex] == 0)
        {
            ChildPid[ExitCodeIndex] = getpid();
            if (getppid() != ParentPid)
            {
                Result = LXT_RESULT_FAILURE;
                LxtLogError("Unexpected parent pid in child - %d != %d", ChildPid[ExitCodeIndex], ParentPid);

                goto ErrorExit;
            }

            _exit(ExitCodes[ExitCodeIndex]);
        }
        else
        {
            ExpectedWaitStatus = ExitCodes[ExitCodeIndex] << 8;
            LxtCheckResult(LxtWaitPidHelper(ChildPid[ExitCodeIndex], ExpectedWaitStatus, Blocking));
        }
    }

    //
    // Recheck the results after launching the children in parallel.
    //

    LxtLogInfo("Running forks in parallel...");
    for (ExitCodeIndex = 0; ExitCodeIndex < LXT_COUNT_OF(ExitCodes); ++ExitCodeIndex)
    {
        LxtCheckErrno(ChildPid[ExitCodeIndex] = fork());
        if (ChildPid[ExitCodeIndex] == 0)
        {
            ChildPid[ExitCodeIndex] = getpid();
            if (getppid() != ParentPid)
            {
                Result = LXT_RESULT_FAILURE;
                LxtLogError("Unexpected parent pid in child - %d != %d", ChildPid[ExitCodeIndex], ParentPid);

                goto ErrorExit;
            }

            _exit(ExitCodes[ExitCodeIndex]);
        }
    }

    while (ExitCodeIndex--)
    {
        ExpectedWaitStatus = ExitCodes[ExitCodeIndex] << 8;
        LxtCheckResult(LxtWaitPidHelper(ChildPid[ExitCodeIndex], ExpectedWaitStatus, Blocking));
    }

ErrorExit:
    return Result;
}

int WaitPidVariationExitStatusPoll(PLXT_ARGS Args)

/*++
--*/

{

    return WaitPidVariationExitStatusHelper(Args, 0);
}

typedef struct _WAIT_PID_PARENT_DATA
{
    int Generation;
    pid_t ParentPid;
    pid_t ParentTid;
    LXT_PIPE Pipes[4];
    LXT_CLONE_ARGS CloneArgs[2];
    pid_t ForkPid;
} WAIT_PID_PARENT_DATA, *PWAIT_PID_PARENT_DATA;

void WaitPidPrintData(PWAIT_PID_PARENT_DATA CurrentData, char* Message)

{

    int Index;
    pid_t WaitResult;
    int WaitStatus;

    //
    // TODO: Enable _CLONE
    //

    LxtLogInfo("***** %s", Message);
    WaitStatus = -1;
    for (Index = 0; Index < LXT_COUNT_OF(CurrentData->CloneArgs); ++Index)
    {
        LxtLogInfo("Before waitpid", Message);
        WaitResult = waitpid(CurrentData->CloneArgs[Index].CloneId, &WaitStatus, WNOHANG);
        LxtLogInfo("%s - Clone %d WNOHANG - %d, %d", Message, CurrentData->CloneArgs[Index].CloneId, WaitResult, WaitStatus);
        //      WaitResult = waitpid(CurrentData->CloneArgs[Index].CloneId, &WaitStatus, WNOHANG | __WCLONE);
        //      LxtLogInfo("%s - Clone %d WNOHANG | __WCLONE - %d, %d", Message, CurrentData->CloneArgs[Index].CloneId, WaitResult, WaitStatus);
    }

    WaitResult = waitpid(CurrentData->ForkPid, &WaitStatus, WNOHANG);
    LxtLogInfo("%s - Fork %d WNOHANG - %d, %d", Message, CurrentData->ForkPid, WaitResult, WaitStatus);
    //    WaitResult = waitpid(CurrentData->ForkPid, &WaitStatus, WNOHANG | __WCLONE);
    //    LxtLogInfo("%s - Fork %d WNOHANG | __WCLONE - %d, %d", Message, CurrentData->ForkPid, WaitResult, WaitStatus);
    LxtLogInfo("***** %s", Message);

    return;
}

int WaitPidVariationParentChildClone(void* Parameter)

/*++
--*/

{

    int CloneIndex;
    PWAIT_PID_PARENT_DATA CurrentData;
    pid_t CurrentPid;
    pid_t CurrentTid;
    int Index;
    int ParentIndex;
    int Result;
    int WaitPipe;

    CurrentData = Parameter;
    CurrentTid = gettid();
    for (Index = 0; Index < LXT_COUNT_OF(CurrentData->CloneArgs); ++Index)
    {
        if (CurrentData->CloneArgs[Index].CloneId == CurrentTid)
        {
            CloneIndex = Index;
            break;
        }
    }

    if (Index == LXT_COUNT_OF(CurrentData->CloneArgs))
    {
        Result = LXT_RESULT_FAILURE;
        LxtLogError("Unable to find clone tid %d", CurrentTid);
        goto ErrorExit;
    }

    LxtCheckErrno(read(CurrentData->Pipes[CloneIndex].Read, &WaitPipe, sizeof(WaitPipe)));
    CurrentPid = getpid();
    if (CurrentPid != CurrentData->ParentPid)
    {
        Result = LXT_RESULT_FAILURE;
        LxtLogError("Unexpected pid for clone %d: %d != %d", CloneIndex, CurrentPid, CurrentData->ParentPid);

        goto ErrorExit;
    }

    WaitPidPrintData(CurrentData, "Clone");
    ParentIndex = LXT_COUNT_OF(CurrentData->CloneArgs) + 1;
    LxtCheckErrno(write(CurrentData->Pipes[ParentIndex].Write, &WaitPipe, sizeof(WaitPipe)));
    LxtCheckErrno(read(CurrentData->Pipes[CloneIndex].Read, &WaitPipe, sizeof(WaitPipe)));

ErrorExit:
    LxtLogInfo("Clone %d exit", CloneIndex);
    return Result;
}

int WaitPidVariationParentChildFork(PWAIT_PID_PARENT_DATA ParentData)

/*++
--*/

{

    WAIT_PID_PARENT_DATA CurrentData;
    int ForkIndex;
    pid_t ParentPid;
    int ParentIndex;
    int Result;
    int WaitPipe;

    ForkIndex = LXT_COUNT_OF(ParentData->CloneArgs);
    LxtCheckErrno(read(ParentData->Pipes[ForkIndex].Read, &WaitPipe, sizeof(WaitPipe)));
    CurrentData.ParentPid = getpid();
    CurrentData.ParentTid = gettid();
    if (CurrentData.ParentPid != CurrentData.ParentTid)
    {
        Result = LXT_RESULT_FAILURE;
        LxtLogError("Current fork thread is not thread group leader %d != %d", CurrentData.ParentPid, CurrentData.ParentTid);
        goto ErrorExit;
    }

    ParentPid = getppid();
    if (ParentPid != ParentData->ParentPid)
    {
        Result = LXT_RESULT_FAILURE;
        LxtLogError("Unexpected ppid %d != %d", ParentPid, ParentData->ParentPid);
        goto ErrorExit;
    }

    if (CurrentData.ParentPid == ParentData->ParentPid)
    {
        Result = LXT_RESULT_FAILURE;
        LxtLogError("Unexpected pid %d == %d", CurrentData.ParentPid, ParentData->ParentPid);
        goto ErrorExit;
    }

    ParentData->ForkPid = getpid();
    WaitPidPrintData(ParentData, "Fork");
    ParentIndex = LXT_COUNT_OF(ParentData->CloneArgs) + 1;
    LxtCheckErrno(write(ParentData->Pipes[ParentIndex].Write, &WaitPipe, sizeof(WaitPipe)));
    LxtCheckErrno(read(ParentData->Pipes[ForkIndex].Read, &WaitPipe, sizeof(WaitPipe)));

ErrorExit:
    _exit(Result);
}

int WaitPidVariationCreateParentData(PWAIT_PID_PARENT_DATA CurrentData)

/*++
--*/

{

    int Index;
    int ParentIndex;
    int Result;
    int WaitPipe;
    pid_t WaitResult;
    int WaitStatus;

    CurrentData->Generation = CurrentData->Generation + 1;
    CurrentData->ParentPid = getpid();
    CurrentData->ParentTid = gettid();
    if (CurrentData->ParentPid != CurrentData->ParentTid)
    {
        Result = LXT_RESULT_FAILURE;
        LxtLogError("Current thread is not thread group leader %d != %d", CurrentData->ParentPid, CurrentData->ParentTid);
        goto ErrorExit;
    }

    for (Index = 0; Index < LXT_COUNT_OF(CurrentData->Pipes); ++Index)
    {
        LxtCheckResult(LxtCreatePipe(&CurrentData->Pipes[Index]));
    }

    //
    // TODO: Enable _CLONE
    //

    for (Index = 0; Index < LXT_COUNT_OF(CurrentData->CloneArgs); ++Index)
    {
        LxtCheckResult(LxtClone(WaitPidVariationParentChildClone, CurrentData, LXT_CLONE_FLAGS_DEFAULT, &CurrentData->CloneArgs[Index]));
    }

    LxtCheckErrno(CurrentData->ForkPid = fork());
    if (CurrentData->ForkPid == 0)
    {
        WaitPidVariationParentChildFork(CurrentData);
    }

    WaitPidPrintData(CurrentData, "Parent");
    ParentIndex = LXT_COUNT_OF(CurrentData->CloneArgs) + 1;
    for (Index = 0; Index < LXT_COUNT_OF(CurrentData->Pipes) - 1; ++Index)
    {
        LxtCheckErrno(write(CurrentData->Pipes[Index].Write, &WaitPipe, sizeof(WaitPipe)));
        LxtCheckErrno(read(CurrentData->Pipes[ParentIndex].Read, &WaitPipe, sizeof(WaitPipe)));
    }

    //
    // Release the waiters
    //

    for (Index = 0; Index < LXT_COUNT_OF(CurrentData->Pipes) - 1; ++Index)
    {
        LxtCheckErrno(write(CurrentData->Pipes[Index].Write, &WaitPipe, sizeof(WaitPipe)));
    }

ErrorExit:
    for (Index = 0; Index < LXT_COUNT_OF(CurrentData->Pipes); ++Index)
    {
        LxtClosePipe(&CurrentData->Pipes[Index]);
    }

    return Result;
}

int WaitPidVariationParentChild(PLXT_ARGS Args)

/*++
--*/

{

    WAIT_PID_PARENT_DATA CurrentData;
    int Result;

    memset(&CurrentData, 0, sizeof(CurrentData));
    LxtCheckResult(WaitPidVariationCreateParentData(&CurrentData));

ErrorExit:
    return Result;
}

int WaitPidVariationProcessGroup(PLXT_ARGS Args)

/*++

Description:

    This routine tests waiting on all children in the same process group.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    const int OtherGroupChildCount = 2;
    pid_t OtherGroupChild[OtherGroupChildCount];
    int Result;
    pid_t SameGroupChild;
    int Status;
    int WaitResult;

    LxtCheckResult(LxtSignalBlock(SIGUSR1));
    for (int Child = 0; Child < OtherGroupChildCount; Child++)
    {
        LxtCheckErrno(OtherGroupChild[Child] = fork());
        if (OtherGroupChild[Child] == 0)
        {

            //
            // Change process group, then signal the parent.
            //

            LxtCheckErrnoZeroSuccess(setpgid(0, 0));
            LxtCheckErrnoZeroSuccess(kill(getppid(), SIGUSR1));
            _exit(0);
        }

        //
        // Wait to make sure the child changed its process group.
        //

        LxtCheckResult(LxtSignalWaitBlocked(SIGUSR1, OtherGroupChild[Child], 2));
    }

    LxtCheckErrno(SameGroupChild = fork());
    if (SameGroupChild == 0)
    {
        _exit(0);
    }

    //
    // Wait for the same process group, which should return the status of the
    // second child.
    //

    LxtCheckResult(WaitResult = LxtWaitPidPoll(0, 0));
    LxtCheckEqual(WaitResult, SameGroupChild, "%d");

    //
    // Wait again, which should fail because there are no more children in this
    // process group.
    //

    LxtCheckErrnoFailure(waitpid(0, &Status, WNOHANG), ECHILD);

    //
    // Wait on the specific process group of one of the remaining children.
    //

    LxtCheckResult(WaitResult = LxtWaitPidPoll(-OtherGroupChild[0], 0));
    LxtCheckEqual(WaitResult, OtherGroupChild[0], "%d");

    //
    // Wait on all children, which should return the child in the other process
    // group.
    //

    LxtCheckResult(WaitResult = LxtWaitPidPoll(-1, 0));
    LxtCheckEqual(WaitResult, OtherGroupChild[1], "%d");

ErrorExit:
    return Result;
}

int WaitPidVariationInvalidParameter(PLXT_ARGS Args)

/*++

Description:

    This routine tests invalid parameter handling for the waitpid system call.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int Result;
    int Status;

    LxtCheckErrnoFailure(waitpid(0, &Status, WEXITED), EINVAL);
    LxtCheckErrnoFailure(waitpid(0, &Status, WNOWAIT), EINVAL);
    LxtCheckErrnoFailure(waitpid(0, NULL, WNOHANG), ECHILD);
    LxtCheckErrnoFailure(waitpid(0, (void*)-1, WNOHANG), ECHILD);

ErrorExit:
    return Result;
}

void* WaitPidVariationWaitIdThread(void* Parameter)

/*++

Description:

    This routine is the thread handler for the waitid test.

Arguments:

    Parameter - Supplies the thread parameter.

Return Value:

    Returns the thread id on success, -1 on failure.

--*/

{

    int Result;
    LxtLogInfo("WaitPid child tid %d", gettid());
    LxtCheckErrno(LxtSetUid(WAITPID_PTHREAD_UID));

    //
    // Enter a very long sleep, this will be interrupted when the threadgroup
    // leader dies.
    //

    sleep(-1);
    Result = 0;

ErrorExit:

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wint-to-pointer-cast"

    return (void*)Result;

#pragma GCC diagnostic pop
}

int WaitPidVariationWaitId(PLXT_ARGS Args)

/*++

Description:

    This routine tests the waitid system call.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int ChildPid;
    int ExpectedStatus;
    int Result;
    siginfo_t SigInfo;
    pthread_t Thread;
    struct rusage Usage;

    ExpectedStatus = 44;
    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {

        //
        // Create a child thread, set the uid of the threadgroup leader,
        // and exit.
        //

        LxtCheckResultError(pthread_create(&Thread, NULL, WaitPidVariationWaitIdThread, NULL));

        LxtLogInfo("Waitid parent tid %d", gettid());

        //
        // Briefly sleep to allow the pthread to run.
        //

        sleep(1);
        LxtCheckErrno(LxtSetUid(WAITPID_THREADGROUP_LEADER_UID));
        _exit(ExpectedStatus);
    }

    LxtCheckErrnoZeroSuccess(Result = LxtWaitId(P_ALL, 0, &SigInfo, WEXITED, &Usage));
    LxtCheckEqual(Result, 0, "%d");
    LxtCheckEqual(SigInfo.si_code, CLD_EXITED, "%d");
    LxtCheckEqual(SigInfo.si_status, ExpectedStatus, "%d");
    LxtCheckEqual(SigInfo.si_pid, ChildPid, "%d");
    LxtCheckEqual(SigInfo.si_uid, WAITPID_THREADGROUP_LEADER_UID, "%d");

    //
    // Wait for a specific child.
    //

    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        _exit(ExpectedStatus);
    }

    LxtCheckErrnoZeroSuccess(Result = LxtWaitId(P_PID, ChildPid, &SigInfo, WEXITED, &Usage));
    LxtCheckEqual(Result, 0, "%d");
    LxtCheckEqual(SigInfo.si_code, CLD_EXITED, "%d");
    LxtCheckEqual(SigInfo.si_status, ExpectedStatus, "%d");
    LxtCheckEqual(SigInfo.si_pid, ChildPid, "%d");

    //
    // Wait with WNOHANG specified.
    //
    // N.B. Parent must sleep to allow the child to exit before waiting.
    //

    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        _exit(ExpectedStatus);
    }

    sleep(1);
    LxtCheckErrnoZeroSuccess(Result = LxtWaitId(P_PID, ChildPid, &SigInfo, (WEXITED | WNOHANG), &Usage));
    LxtCheckEqual(Result, 0, "%d");
    LxtCheckEqual(SigInfo.si_code, CLD_EXITED, "%d");
    LxtCheckEqual(SigInfo.si_status, ExpectedStatus, "%d");
    LxtCheckEqual(SigInfo.si_pid, ChildPid, "%d");

    //
    // Wait with a null siginfo structure.
    //
    // N.B. The man page states that the pid of the child should be returned but
    //      this is not the case. The wait should still be consumed so the
    //      second wait should fail with ECHILD.
    //

    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        _exit(0);
    }

    LxtCheckErrnoZeroSuccess(Result = LxtWaitId(P_ALL, 0, NULL, WEXITED, NULL));
    LxtCheckErrnoFailure(LxtWaitId(P_ALL, 0, NULL, WEXITED, NULL), ECHILD);

    //
    // Wait with the WNOWAIT option supplied which means the wait is not
    // consumed. Wait again to consume the wait and verify it was not consumed
    // by the first call.
    //

    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        _exit(ExpectedStatus);
    }

    LxtCheckErrnoZeroSuccess(Result = LxtWaitId(P_PID, ChildPid, &SigInfo, (WEXITED | WNOWAIT), &Usage));
    LxtCheckEqual(Result, 0, "%d");
    LxtCheckEqual(SigInfo.si_code, CLD_EXITED, "%d");
    LxtCheckEqual(SigInfo.si_status, ExpectedStatus, "%d");
    LxtCheckEqual(SigInfo.si_pid, ChildPid, "%d");
    LxtCheckErrnoZeroSuccess(Result = LxtWaitId(P_PID, ChildPid, &SigInfo, WEXITED, &Usage));
    LxtCheckEqual(Result, 0, "%d");
    LxtCheckEqual(SigInfo.si_code, CLD_EXITED, "%d");
    LxtCheckEqual(SigInfo.si_status, ExpectedStatus, "%d");
    LxtCheckEqual(SigInfo.si_pid, ChildPid, "%d");

    //
    // Call getrusage with the supported values.
    //

    LxtCheckErrno(getrusage(RUSAGE_SELF, &Usage));
    LxtCheckErrno(getrusage(RUSAGE_THREAD, &Usage));
    LxtCheckErrno(getrusage(RUSAGE_CHILDREN, &Usage));

    //
    // Invalid parameter variations.
    //

    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        _exit(0);
    }

    LxtCheckErrnoFailure(LxtWaitId(P_ALL, 0, NULL, WNOHANG, NULL), EINVAL);
    LxtCheckErrnoFailure(LxtWaitId(-1, 0, NULL, WEXITED, NULL), EINVAL);
    LxtCheckErrnoFailure(LxtWaitId(P_PGID + 5, 0, NULL, WEXITED, NULL), EINVAL);
    LxtCheckErrnoFailure(LxtWaitId(P_ALL, 0, NULL, 0x10, NULL), EINVAL);

    //
    // N.B. Providing an invalid pointer to a siginfo structure returns efault
    //      but consumes the wait.
    //

    LxtCheckErrnoFailure(LxtWaitId(P_ALL, 0, (void*)-1, WEXITED, NULL), EFAULT);
    LxtCheckErrnoFailure(LxtWaitId(P_ALL, 0, NULL, WEXITED, NULL), ECHILD);

    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        _exit(0);
    }

    //
    // N.B. Providing an invalid pointer to a rusage structure returns efault
    //      but consumes the wait.
    //

    LxtCheckErrnoFailure(LxtWaitId(P_ALL, 0, NULL, WEXITED, (void*)-1), EFAULT);
    LxtCheckErrnoFailure(LxtWaitId(P_ALL, 0, NULL, WEXITED, NULL), ECHILD);

ErrorExit:
    if (ChildPid == 0)
    {
        _exit(Result);
    }

    return Result;
}

int WaitPidVariationCloneParentChild(void)

/*++

Description:

    This routine is the child process for WaitPidVariationCloneParent.

Arguments:

    None..

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    pid_t ChildPid;
    pid_t GrandChildParent;
    pid_t ChildParent;
    int Result;
    int WaitPidStatus;

    Result = 0;

    //
    // Create a child process with the CLONE_PARENT flag.
    //
    // The new process should not be reported as a child.
    //

    ChildParent = getppid();
    LxtLogInfo("ChildParent %d", ChildParent);
    LxtCheckResult(ChildPid = LxtCloneSyscall(CLONE_PARENT | SIGCHLD, NULL, NULL, NULL, NULL));
    if (ChildPid == 0)
    {
        GrandChildParent = getppid();
        LxtCheckEqual(ChildParent, GrandChildParent, "%d");
        LxtLogInfo("Grand child %d exiting", LxtGetTid());
    }
    else
    {
        LxtCheckErrnoFailure(waitpid(ChildPid, &WaitPidStatus, 0), ECHILD);
        LxtLogInfo("Child %d exiting", LxtGetTid());
    }

ErrorExit:
    _exit(Result);
}

int WaitPidVariationCloneParent(PLXT_ARGS Args)

/*++

Description:

    This routine tests invalid parameter handling for the waitpid system call.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    pid_t ChildPid;
    pid_t GrandChildPid;
    int Result;
    int Status;

    ChildPid = -1;

    //
    // Create a child process, that in turn creates a grandchild process
    // with CLONE_PARENT.
    //

    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        WaitPidVariationCloneParentChild();
    }

    //
    // Wait for the child and the grandchild.
    //

    LxtCheckResult(LxtWaitPidPoll(ChildPid, 0));
    LxtCheckResult(GrandChildPid = LxtWaitPidPoll(0, 0));
    LxtLogInfo("Waited on grandchild %d", GrandChildPid);

    //
    // Check that the same scenario works when parent dies before
    // CLONE_PARENT.
    //

    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        LxtCheckErrno(ChildPid = fork());
        if (ChildPid == 0)
        {
            sleep(1);
            WaitPidVariationCloneParentChild();
        }

        _exit(0);
    }

    //
    // Wait for the child and give the grandchild time to finish.
    //

    LxtCheckResult(LxtWaitPidPoll(ChildPid, 0));
    LxtLogInfo("Waited on child %d", ChildPid);
    sleep(2);

ErrorExit:
    if (ChildPid == 0)
    {
        _exit(Result);
    }

    return Result;
}

typedef struct _WAITPID_THREAD_PARAMETERS
{
    pid_t ChildPid;
    pid_t GrandChildPid1;
    pid_t GrandChildPid2;
    LXT_SYNCHRONIZATION_POINT_DECLARE_FOR(ChildPid);
    LXT_SYNCHRONIZATION_POINT_DECLARE_FOR(GrandChildPid2);
    LXT_PIPE Pipe;
    int ChildLevel;
    int Variation;
} WAITPID_THREAD_PARAMETERS, *PWAITPID_THREAD_PARAMETERS;

int ThreadZombieThread(void* Context)

/*++

Description:

    This routine is the thread proc used by WaitPidVariationThreadZombie.

Arguments:

    Context - Supplies the thread context.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int ChildLevel;
    pid_t ChildPid;
    int Flags;
    pid_t GrandChildPid1;
    pid_t GrandChildPid2;
    LXT_CLONE_ARGS CloneArgs;
    WAITPID_THREAD_PARAMETERS LocalParameter;
    PWAITPID_THREAD_PARAMETERS Parameter;
    int Result;
    int Status;
    LXT_SYNCHRONIZATION_POINT_DECLARE_FOR(ChildPid);
    LXT_SYNCHRONIZATION_POINT_DECLARE_FOR(GrandChildPid2);

    memcpy(&LocalParameter, Context, sizeof(LocalParameter));
    ChildPid = 0;
    ChildLevel = LocalParameter.ChildLevel;
    if (ChildLevel != 0)
    {
        free(Context);
    }

    LxtSyncChildPidParent = LocalParameter.LxtSyncChildPidParent;
    LxtSyncChildPidChild = LocalParameter.LxtSyncChildPidChild;
    GrandChildPid1 = -1;
    GrandChildPid2 = -1;
    LxtSyncGrandChildPid2Parent = LocalParameter.LxtSyncGrandChildPid2Parent;
    LxtSyncGrandChildPid2Child = LocalParameter.LxtSyncGrandChildPid2Child;
    Parameter = NULL;

    LxtSignalInitializeThread();

    if (ChildLevel == 0)
    {
        LxtLogInfo("Child %d starting, variation = %d...", getpid(), LocalParameter.Variation);

        LxtCheckResult(LxtSignalInitialize());
        LxtCheckResult(LxtSignalSetupHandler(SIGCHLD, SA_SIGINFO));
        LxtCheckErrno(setsid());
        switch (LocalParameter.Variation)
        {
        case 0:
            Flags = SIGCHLD;
            break;

        case 1:
            Flags = CLONE_FS | CLONE_FILES | SIGCHLD;
            break;

        case 2:
            Flags = CLONE_FS | CLONE_FILES;
            break;

        case 3:
            Flags = CLONE_THREAD | CLONE_VM | CLONE_SIGHAND | CLONE_FS | CLONE_FILES;
            break;
        };

        Parameter = malloc(sizeof(*Parameter));
        if (Parameter != NULL)
        {
            memcpy(Parameter, &LocalParameter, sizeof(*Parameter));
        }

        LxtCheckNotEqual(Parameter, NULL, "%p");
        Parameter->ChildLevel = 1;

        //
        // The clone stack is leaked but the current process will exit shortly.
        //

        LxtCheckErrno(LxtClone(ThreadZombieThread, Parameter, Flags, &CloneArgs));

        Parameter = NULL;
        GrandChildPid1 = CloneArgs.CloneId;
        LocalParameter.GrandChildPid1 = GrandChildPid1;
        Parameter = malloc(sizeof(*Parameter));
        if (Parameter != NULL)
        {
            memcpy(Parameter, &LocalParameter, sizeof(*Parameter));
        }

        LxtCheckNotEqual(Parameter, NULL, "%p");
        Parameter->ChildLevel = 2;

        //
        // The clone stack is leaked but the current process will exit shortly.
        //

        LxtCheckErrno(LxtClone(ThreadZombieThread, Parameter, Flags, &CloneArgs));

        Parameter = NULL;
        LocalParameter.GrandChildPid2 = CloneArgs.CloneId;

        LxtSignalWait();
        if (LocalParameter.Variation < 2)
        {
            LxtCheckResult(LxtSignalCheckSigChldReceived(CLD_EXITED, GrandChildPid1, getuid(), 0));
        }
        else if (LocalParameter.Variation != 3)
        {
            LxtCheckResult(LxtSignalCheckNoSignal());
        }

        LXT_SYNCHRONIZATION_POINT_CHILD();
        LxtCheckErrno(write(LocalParameter.Pipe.Write, &LocalParameter, sizeof(LocalParameter)));

        LXT_SYNCHRONIZATION_POINT_CHILD();

        //
        // Exiting with one zombie child, and one child waiting for an exit
        // signal.
        //

        LxtLogInfo("Exiting pid = %d", getpid());
        Result = 0;
        goto ErrorExit;
    }
    else if (ChildLevel == 1)
    {

        //
        // First child created via this thread, grandchild of the original.
        //

        GrandChildPid1 = 0;
        LxtLogInfo("Grandchild %d starting...", getpid());
        LxtLogInfo("Exiting pid = %d", getpid());
        Result = 0;
        goto ErrorExit;
    }
    else
    {

        //
        // Second child created via this thread, second grandchild of the
        // original.
        //

        GrandChildPid2 = 0;
        LxtLogInfo("Grandchild %d starting...", getpid());
        LxtCheckResult(LxtSignalInitialize());
        LxtCheckResult(LxtSignalSetupHandler(SIGHUP, SA_SIGINFO));
        LXT_SYNCHRONIZATION_POINT_CHILD_FOR(GrandChildPid2);
        LxtCheckResult(LxtSignalCheckNoSignal());
        LxtLogInfo("Exiting pid = %d", getpid());
        Result = 0;
        goto ErrorExit;
    }

ErrorExit:
    if (Result != 0)
    {

        //
        // Intentionally orphaning the thread unless there was an error.
        //

        LXT_SYNCHRONIZATION_POINT_END_FOR(GrandChildPid2, FALSE);
    }

    if (ChildLevel == 0)
    {
        if (LocalParameter.Variation != 3)
        {
            LXT_SYNCHRONIZATION_POINT_END();
        }
        else
        {
            LXT_SYNCHRONIZATION_POINT_PTHREAD_END_THREAD();
        }
    }

    if (Parameter != NULL)
    {
        free(Parameter);
    }

    syscall(SYS_exit, Result);
}

int WaitPidVariationZombie(PLXT_ARGS Args)

/*++

Description:

    This routine tests zombie handling with waitpid system call.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    char Buffer[50];
    LXT_CLONE_ARGS ChildClone;
    int ChildDir;
    pid_t ChildPid;
    FILE* ChildStatusFile;
    pthread_t ChildThread;
    int GrandChildDir;
    pid_t GrandChildPid1;
    pid_t GrandChildPid2;
    FILE* GrandChildStatusFile;
    int Index;
    bool IsFork;
    char Path[32];
    LXT_PIPE Pipe = {-1, -1};
    void* PointerResult;
    int Result;
    struct stat StatBuffer;
    int Status;
    char StatusDescription[25];
    char* StatusFileEntry;
    size_t StatusFileEntryLength;
    char StatusToken;
    WAITPID_THREAD_PARAMETERS ThreadParam;
    LXT_SYNCHRONIZATION_POINT_DECLARE_FOR(GrandChildPid2);

    ChildDir = -1;
    ChildPid = -1;
    ChildStatusFile = NULL;
    ChildThread = 0;
    GrandChildDir = -1;
    GrandChildPid1 = -1;
    GrandChildPid2 = -1;
    GrandChildStatusFile = NULL;
    IsFork = true;
    StatusFileEntry = NULL;
    LXT_SYNCHRONIZATION_POINT_INIT_FOR(GrandChildPid2);

    //
    // TODO: test needs to be debugged for WSL2.
    //

    if (g_VmMode != false)
    {
        Result = 0;
        goto ErrorExit;
    }

    LxtCheckResult(LxtSignalInitialize());
    LxtCheckResult(LxtSignalSetupHandler(SIGCHLD, SA_SIGINFO));
    LxtCheckResult(LxtSignalIgnore(SIGPIPE));

    memset(&ThreadParam, 0, sizeof(ThreadParam));
    ThreadParam.LxtSyncChildPidParent = LxtSyncChildPidParent;
    ThreadParam.LxtSyncChildPidChild = LxtSyncChildPidChild;
    ThreadParam.LxtSyncGrandChildPid2Parent = LxtSyncGrandChildPid2Parent;
    ThreadParam.LxtSyncGrandChildPid2Child = LxtSyncGrandChildPid2Child;
    for (Index = 0; Index < 4; Index += 1)
    {
        ChildPid = -1;
        GrandChildPid1 = -1;
        GrandChildPid2 = -1;
        LXT_SYNCHRONIZATION_POINT_START();
        LXT_SYNCHRONIZATION_POINT_START_FOR(GrandChildPid2);
        ThreadParam.ChildPid = -1;
        ThreadParam.GrandChildPid2 = -1;
        ThreadParam.GrandChildPid2 = -1;
        LxtCheckResult(LxtCreatePipe(&Pipe));
        memcpy(&ThreadParam.Pipe, &Pipe, sizeof(Pipe));
        ThreadParam.Variation = Index;
        LxtCheckErrno(ChildPid = fork());
        if (ChildPid == 0)
        {
            return ThreadZombieThread(&ThreadParam);
        }

        ThreadParam.ChildPid = ChildPid;

        LXT_SYNCHRONIZATION_POINT();
        LxtCheckErrno(read(Pipe.Read, &ThreadParam, sizeof(ThreadParam)));
        GrandChildPid1 = ThreadParam.GrandChildPid1;
        GrandChildPid2 = ThreadParam.GrandChildPid2;

        //
        // Check basic info of child after one of its children has entered
        // zombie state.
        //

        LxtCheckErrno(sprintf(Path, "/proc/%d", ChildPid));
        LxtCheckErrno(ChildDir = open(Path, O_RDONLY));
        LxtCheckErrno(fstat(ChildDir, &StatBuffer));
        LxtCheckErrno(sprintf(Path, "/proc/%d/status", ChildPid));
        LxtCheckNullErrno(ChildStatusFile = fopen(Path, "r"));
        while ((Result = getline(&StatusFileEntry, &StatusFileEntryLength, ChildStatusFile)) >= 0)
        {

            if (strncmp(StatusFileEntry, "State:\t", 7) == 0)
            {
                break;
            }

            free(StatusFileEntry);
            StatusFileEntry = NULL;
        }

        LxtCheckErrno(Result);
        Result = sscanf(StatusFileEntry, "State:  %c (%s)", &StatusToken, StatusDescription);

        LxtCheckEqual(Result, 2, "%d");
        if (StatusToken == 'R')
        {
            LxtCheckStringEqual(StatusDescription, "running)");
        }
        else if (StatusToken == 'S')
        {
            LxtCheckStringEqual(StatusDescription, "sleeping)");
        }
        else
        {
            LxtLogError("Unexpected status: %c (%s)", StatusToken, StatusDescription);
            Result = -1;
            goto ErrorExit;
        }

        LxtCheckErrno(getpgid(ChildPid));
        LxtCheckErrno(kill(ChildPid, SIGWINCH));
        LxtCheckErrno(sprintf(Path, "/proc/%d/ns/mnt", ChildPid));
        LxtCheckErrno(readlink(Path, Buffer, sizeof(Buffer)));
        LxtCheckErrno(sprintf(Path, "/proc/%d/fd/0", ChildPid));
        LxtCheckErrno(stat(Path, &StatBuffer));

        //
        // Check basic info of first zombie.
        //

        LxtCheckErrno(sprintf(Path, "/proc/%d", GrandChildPid1));
        if (Index != 3)
        {
            LxtCheckErrno(getpgid(GrandChildPid1));
            LxtCheckErrno(kill(GrandChildPid1, SIGWINCH));
            LxtCheckErrno(GrandChildDir = open(Path, O_RDONLY));
            LxtCheckErrno(fstat(GrandChildDir, &StatBuffer));
            LxtCheckErrno(sprintf(Path, "/proc/%d/fd/0", GrandChildPid1));

            //
            // TODO_LX: Zombied procfs "fd" entry should be accessible but
            //          empty.
            //
            // LxtCheckErrnoFailure(stat(Path, &StatBuffer), ENOENT);
            //

            LxtCheckErrno(sprintf(Path, "/proc/%d/status", GrandChildPid1));
            LxtCheckNullErrno(GrandChildStatusFile = fopen(Path, "r"));
            free(StatusFileEntry);
            StatusFileEntry = NULL;
            while ((Result = getline(&StatusFileEntry, &StatusFileEntryLength, GrandChildStatusFile)) >= 0)
            {

                if (strncmp(StatusFileEntry, "State:\t", 7) == 0)
                {
                    break;
                }

                free(StatusFileEntry);
                StatusFileEntry = NULL;
            }

            LxtCheckErrno(Result);
            Result = sscanf(StatusFileEntry, "State:  %c (%s)", &StatusToken, StatusDescription);

            LxtCheckEqual(Result, 2, "%d");
            LxtCheckEqual(StatusToken, 'Z', "%c");
            LxtCheckStringEqual(StatusDescription, "zombie)");

            LxtCheckErrno(sprintf(Path, "/proc/%d/fd/0", GrandChildPid1));

            //
            // TODO_LX: Zombied procfs "fd" entry should be accessible but
            //          empty.
            //
            // LxtCheckErrnoFailure(open(Path, O_RDONLY), ENOENT);
            //
        }
        else
        {
            LxtCheckErrnoFailure(GrandChildDir = open(Path, O_RDONLY), ENOENT);
        }

        //
        // Close the read end of the pipe.
        //

        close(Pipe.Read);
        Pipe.Read = -1;
        LxtCheckErrno(write(Pipe.Write, &Result, sizeof(Result)));

        //
        // Allow the child to exit.
        //

        LXT_SYNCHRONIZATION_POINT();
        LxtSignalWait();
        if (Index != 3)
        {
            LxtCheckResult(LxtSignalCheckSigChldReceived(CLD_EXITED, ChildPid, getuid(), 0));

            LxtSignalResetReceived();
        }
        else
        {

            //
            // The threadgroup leader should have exited but there is still a
            // thread running so the signal will not yet be sent.
            //

            LxtCheckResult(LxtSignalCheckNoSignal());
        }

        //
        // Check basic info of running grandchild after child exit.
        //

        if (Index != 3)
        {

            //
            // TODO_LX: These calls fail due to lack of proper thread support.
            //

            LxtCheckErrno(getpgid(GrandChildPid2));
            LxtCheckErrno(kill(GrandChildPid2, SIGWINCH));
        }

        LxtCheckErrno(sprintf(Path, "/proc/%d/ns/mnt", GrandChildPid2));
        LxtCheckErrno(readlink(Path, Buffer, sizeof(Buffer)));
        LxtCheckErrno(sprintf(Path, "/proc/%d/fd/0", GrandChildPid2));
        LxtCheckErrno(stat(Path, &StatBuffer));

        //
        // Signal last grandchild to exit.
        //

        LXT_SYNCHRONIZATION_POINT_PARENT_FOR(GrandChildPid2);
        LxtSignalWait();
        if (Index == 3)
        {
            LxtCheckResult(LxtSignalCheckSigChldReceived(CLD_EXITED, ChildPid, getuid(), 0));

            LxtSignalResetReceived();
        }

        //
        // Check the child information after it is a zombie.
        //

        LxtCheckErrno(fstat(ChildDir, &StatBuffer));
        rewind(ChildStatusFile);
        free(StatusFileEntry);
        StatusFileEntry = NULL;
        while ((Result = getline(&StatusFileEntry, &StatusFileEntryLength, ChildStatusFile)) >= 0)
        {

            if (strncmp(StatusFileEntry, "State:\t", 7) == 0)
            {
                break;
            }

            free(StatusFileEntry);
            StatusFileEntry = NULL;
        }

        LxtCheckErrno(Result);
        Result = sscanf(StatusFileEntry, "State:  %c (%s)", &StatusToken, StatusDescription);

        LxtCheckEqual(Result, 2, "%d");
        LxtCheckEqual(StatusToken, 'Z', "%c");
        LxtCheckStringEqual(StatusDescription, "zombie)");
        LxtCheckErrno(getpgid(ChildPid));
        LxtCheckErrno(kill(ChildPid, SIGWINCH));
        LxtCheckErrno(sprintf(Path, "/proc/%d/ns/mnt", ChildPid));
        LxtCheckErrnoFailure(readlink(Path, Buffer, sizeof(Buffer)), ENOENT);
        sleep(2);
        LxtCheckErrnoFailure(write(Pipe.Write, &Result, sizeof(Result)), EPIPE);

        LXT_SYNCHRONIZATION_POINT_END();
        ChildPid = -1;
        LxtCheckErrnoFailure(waitpid(-1, &Status, WNOHANG), ECHILD);
        LxtCheckEqual(Result, 0, "%d");
        LxtClosePipe(&Pipe);
    }

ErrorExit:
    free(StatusFileEntry);
    LXT_SYNCHRONIZATION_POINT_END();
    if (GrandChildPid2 > 0)
    {
        LXT_SYNCHRONIZATION_POINT_PTHREAD_END_THREAD_FOR(GrandChildPid2);
    }

    LXT_SYNCHRONIZATION_POINT_DESTROY_FOR(GrandChildPid2);
    if (GrandChildStatusFile != NULL)
    {
        fclose(GrandChildStatusFile);
    }

    if (GrandChildDir >= 0)
    {
        close(GrandChildDir);
    }

    if (ChildStatusFile != NULL)
    {
        fclose(ChildStatusFile);
    }

    if (ChildDir >= 0)
    {
        close(ChildDir);
    }

    //
    // Intentionally leaking pipes from children / grandchildren to test
    // file descriptor cleanup on exit in the fork case.
    //

    LxtClosePipe(&Pipe);

    LxtSignalDefault(SIGPIPE);
    LxtSignalDefault(SIGCHLD);
    return Result;
}

int WaitPidVariationZombieStress(PLXT_ARGS Args)

/*++

Description:

    This routine stress tests zombie handling.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    pid_t ChildPid;
    pid_t GrandChildPid;
    pid_t GreatGrandChildPid;
    int Iterations;
    int Result;
    int Status;

    GrandChildPid = -1;
    GreatGrandChildPid = -1;
    for (Iterations = 0; Iterations < 100; Iterations += 1)
    {
        ChildPid = -1;
        LxtCheckErrno(ChildPid = fork());
        if (ChildPid == 0)
        {
            LxtCheckErrno(setsid());
            LxtCheckErrno(prctl(PR_SET_CHILD_SUBREAPER, 1));
            for (Iterations = 0; Iterations < 3; Iterations += 1)
            {
                LxtCheckErrno(GrandChildPid = fork());
                if (GrandChildPid == 0)
                {
                    ChildPid = -1;
                    LxtCheckErrno(GreatGrandChildPid = fork());
                    if (GreatGrandChildPid == 0)
                    {
                        usleep(random() % 100);
                        goto ErrorExit;
                    }

                    usleep(random() % 100);
                    (void)waitpid(GreatGrandChildPid, &Status, WNOHANG);
                    goto ErrorExit;
                }

                (void)waitpid(GrandChildPid, &Status, WNOHANG);
            }

            usleep(random() % 100);
            (void)waitpid(GrandChildPid, &Status, WNOHANG);
            goto ErrorExit;
        }

        LXT_SYNCHRONIZATION_POINT_END();
        ChildPid = -1;
    }

ErrorExit:
    if ((GreatGrandChildPid == 0) || (GrandChildPid == 0))
    {
        _exit(Result);
    }

    LXT_SYNCHRONIZATION_POINT_END();
    return Result;
}