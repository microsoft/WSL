/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    timer.c

Abstract:

    This file is a timer test.

--*/

#include "lxtcommon.h"
#include "unittests.h"
#include <signal.h>
#include <time.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>

#define LXT_NAME "timer"

#define LXT_SHORT_TIMER 1
#define LXT_SHORT_TIMER_WAIT_PID 5
#define LXT_SHORT_TIMER_US 250000
#define LXT_LONG_TIMER 10

#define LXT_INVALID_TIMER_ID ((timer_t) - 1)

int AlarmSyscall(PLXT_ARGS Args);

int ITimerInvalidParam(PLXT_ARGS Args);

int ITimerPerThreadGroup(PLXT_ARGS Args);

int ITimerSignal(PLXT_ARGS Args);

int ITimerPeriodicSignal(PLXT_ARGS Args);

int NanosleepInvalidParam(PLXT_ARGS Args);

int TimerCreateSyscall(PLXT_ARGS Args);

int TimerCreateInvalidParam(PLXT_ARGS Args);

int ClockGetTimeAlignment(PLXT_ARGS Args);

//
// Global constants
//

static const LXT_VARIATION g_LxtVariations[] = {
    {"nanosleep invalid param", NanosleepInvalidParam},
    {"ITimerPerThreadGroup", ITimerPerThreadGroup},
    {"ITimerSignal", ITimerSignal},
    {"AlarmSyscall", AlarmSyscall},
    {"ITimerPeriodicSignal", ITimerPeriodicSignal},
    {"ITimer invalid param", ITimerInvalidParam},
    {"timer_create", TimerCreateSyscall},
    {"timer_create invalid param", TimerCreateInvalidParam},
    {"clock_gettime alignment", ClockGetTimeAlignment}};

static const struct itimerval g_ZeroTimer;

//
// Global variables
//

static struct timespec g_SignalTime;
static int g_SignalCount;

int TimerTestEntry(int Argc, char* Argv[])

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

int ITimerInvalidParam(PLXT_ARGS Args)

{

    struct itimerval NewTimer;
    int Result;

    memset(&NewTimer, 0, sizeof(NewTimer));
    NewTimer.it_value.tv_sec = -1;
    LxtCheckErrnoFailure(setitimer(ITIMER_REAL, &NewTimer, NULL), EINVAL);
    memset(&NewTimer, 0, sizeof(NewTimer));
    NewTimer.it_value.tv_usec = 999999 + 1;
    LxtCheckErrnoFailure(setitimer(ITIMER_REAL, &NewTimer, NULL), EINVAL);
    memset(&NewTimer, 0, sizeof(NewTimer));
    NewTimer.it_interval.tv_sec = -1;
    LxtCheckErrnoFailure(setitimer(ITIMER_REAL, &NewTimer, NULL), EINVAL);
    memset(&NewTimer, 0, sizeof(NewTimer));
    NewTimer.it_interval.tv_usec = 999999 + 1;
    LxtCheckErrnoFailure(setitimer(ITIMER_REAL, &NewTimer, NULL), EINVAL);

ErrorExit:
    return Result;
}

void* ITimerPerThreadGroupWorker(void* ptr)

{

    struct itimerval NewTimer;
    struct itimerval OldTimer;
    int Result;

    //
    // Check that the timer is per threadgroup and the result is the remaining
    // time.
    //

    memset(&NewTimer, 0, sizeof(NewTimer));
    memset(&OldTimer, 1, sizeof(OldTimer));
    sleep(1);
    LxtCheckResult(setitimer(ITIMER_REAL, &NewTimer, &OldTimer));
    if (OldTimer.it_value.tv_sec >= LXT_LONG_TIMER)
    {
        Result = 1;
        LxtLogError("Unexpected OldTimer %d", OldTimer.it_value.tv_sec);
        goto ErrorExit;
    }

ErrorExit:
    return 0;
}

int ITimerPerThreadGroup(PLXT_ARGS Args)

/*++
--*/

{

    int ChildPid;
    int Result;
    struct itimerval NewTimer;
    struct itimerval OldTimer;
    pthread_t Thread = {0};

    //
    // Check that the timer is per threadgroup and not preserved across fork.
    //

    memset(&NewTimer, 0, sizeof(NewTimer));
    NewTimer.it_value.tv_sec = LXT_LONG_TIMER;
    memset(&OldTimer, 1, sizeof(OldTimer));
    LxtCheckResult(setitimer(ITIMER_REAL, &NewTimer, &OldTimer));
    LxtCheckResult(LxtCompareMemory((void*)&g_ZeroTimer, (void*)&OldTimer, sizeof(g_ZeroTimer), "Zero", "Initial"));
    LxtCheckResult(ChildPid = fork());
    if (ChildPid == 0)
    {
        memset(&OldTimer, 1, sizeof(OldTimer));
        LxtCheckResult(setitimer(ITIMER_REAL, &NewTimer, &OldTimer));
        LxtCheckResult(LxtCompareMemory((void*)&g_ZeroTimer, (void*)&OldTimer, sizeof(g_ZeroTimer), "Zero", "Initial child"));
        _exit(LXT_RESULT_SUCCESS);
    }

    LxtCheckResult(LxtWaitPidPoll(ChildPid, LXT_RESULT_SUCCESS));
    LxtCheckErrno(pthread_create(&Thread, NULL, ITimerPerThreadGroupWorker, NULL));
    pthread_join(Thread, NULL);

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    return Result;
}

void ITimerSignalHandler(int Signal)

/*++
--*/

{
    if (Signal == SIGALRM)
    {
        LxtClockGetTime(CLOCK_REALTIME, &g_SignalTime);
    }

    return;
}

int ITimerSignal(PLXT_ARGS Args)

/*++
--*/

{

    struct sigaction Action;
    int ChildPid;
    int ExpectedWaitStatus;
    int Result;
    struct timespec StartTime;
    struct itimerval NewTimer;

    //
    // Check the different dispositions of the SIGALRM signal and cancelling the
    // timer.
    //

    //
    // Default disposition should terminate
    //

    LxtCheckResult(ChildPid = fork());
    if (ChildPid == 0)
    {
        memset(&NewTimer, 0, sizeof(NewTimer));
        NewTimer.it_value.tv_sec = LXT_SHORT_TIMER;
        LxtCheckResult(setitimer(ITIMER_REAL, &NewTimer, NULL));
        sleep(LXT_SHORT_TIMER * 2);
        _exit(LXT_RESULT_SUCCESS);
    }

    LxtCheckResult(LxtWaitPidPollOptions(ChildPid, SIGALRM, 0, LXT_SHORT_TIMER_WAIT_PID));

    //
    // Default disposition should not terminate if canceled.
    //

    LxtCheckResult(ChildPid = fork());
    if (ChildPid == 0)
    {
        memset(&NewTimer, 0, sizeof(NewTimer));
        NewTimer.it_value.tv_sec = LXT_SHORT_TIMER;
        LxtCheckResult(setitimer(ITIMER_REAL, &NewTimer, NULL));
        memset(&NewTimer, 0, sizeof(NewTimer));
        LxtCheckResult(setitimer(ITIMER_REAL, &NewTimer, NULL));
        sleep(LXT_SHORT_TIMER * 2);
        _exit(LXT_RESULT_SUCCESS);
    }

    LxtCheckResult(LxtWaitPidPollOptions(ChildPid, LXT_RESULT_SUCCESS, 0, LXT_SHORT_TIMER_WAIT_PID));

    //
    // Ignored should not terminate.
    //

    LxtCheckResult(ChildPid = fork());
    if (ChildPid == 0)
    {
        memset(&Action, 0, sizeof(Action));
        Action.sa_handler = SIG_IGN;
        LxtCheckErrnoZeroSuccess(sigaction(SIGALRM, &Action, NULL));
        memset(&NewTimer, 0, sizeof(NewTimer));
        NewTimer.it_value.tv_sec = LXT_SHORT_TIMER;
        LxtCheckResult(setitimer(ITIMER_REAL, &NewTimer, NULL));
        sleep(LXT_SHORT_TIMER * 2);
        _exit(LXT_RESULT_SUCCESS);
    }

    LxtCheckResult(LxtWaitPidPollOptions(ChildPid, LXT_RESULT_SUCCESS, 0, LXT_SHORT_TIMER_WAIT_PID));

    //
    // Check that the signal handler is invoked within a reasonable time
    // interval.
    //

    LxtCheckResult(ChildPid = fork());
    if (ChildPid == 0)
    {
        memset(&Action, 0, sizeof(Action));
        Action.sa_handler = ITimerSignalHandler;
        LxtCheckErrnoZeroSuccess(sigaction(SIGALRM, &Action, NULL));
        memset(&g_SignalTime, 0, sizeof(g_SignalTime));
        memset(&NewTimer, 0, sizeof(NewTimer));
        NewTimer.it_value.tv_sec = LXT_SHORT_TIMER;
        LxtClockGetTime(CLOCK_REALTIME, &StartTime);
        LxtCheckResult(setitimer(ITIMER_REAL, &NewTimer, NULL));
        sleep(LXT_SHORT_TIMER * 2);
        if (g_SignalTime.tv_sec - StartTime.tv_sec != 1)
        {
            LxtLogError("Unexpected seconds elapsed %d", g_SignalTime.tv_sec - StartTime.tv_sec);

            _exit(1);
        }

        _exit(LXT_RESULT_SUCCESS);
    }

    LxtCheckResult(LxtWaitPidPollOptions(ChildPid, LXT_RESULT_SUCCESS, 0, LXT_SHORT_TIMER_WAIT_PID));

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    return Result;
}

int AlarmSyscall(PLXT_ARGS Args)

/*++
--*/

{

    struct sigaction Action;
    int ChildPid;
    int ExpectedWaitStatus;
    int Result;
    struct timespec StartTime;

    //
    // Check the different dispositions of the SIGALRM signal and cancelling the
    // timer.
    //

    //
    // Default disposition should terminate
    //

    LxtCheckResult(ChildPid = fork());
    if (ChildPid == 0)
    {
        LxtCheckResult(alarm(LXT_SHORT_TIMER));
        sleep(LXT_SHORT_TIMER * 2);
        _exit(LXT_RESULT_SUCCESS);
    }

    LxtCheckResult(LxtWaitPidPollOptions(ChildPid, SIGALRM, 0, LXT_SHORT_TIMER_WAIT_PID));

    //
    // Default disposition should not terminate if canceled.
    //

    LxtCheckResult(ChildPid = fork());
    if (ChildPid == 0)
    {
        LxtCheckResult(alarm(LXT_SHORT_TIMER));
        LxtCheckResult(Result = alarm(0));
        if (Result > LXT_SHORT_TIMER)
        {
            LxtLogError("Unexpected value for previously armed timer %u", Result);
            _exit(1);
        }

        sleep(LXT_SHORT_TIMER * 2);
        _exit(LXT_RESULT_SUCCESS);
    }

    LxtCheckResult(LxtWaitPidPollOptions(ChildPid, LXT_RESULT_SUCCESS, 0, LXT_SHORT_TIMER_WAIT_PID));

    //
    // Ignored should not terminate.
    //

    LxtCheckResult(ChildPid = fork());
    if (ChildPid == 0)
    {
        memset(&Action, 0, sizeof(Action));
        Action.sa_handler = SIG_IGN;
        LxtCheckErrnoZeroSuccess(sigaction(SIGALRM, &Action, NULL));
        LxtCheckResult(alarm(LXT_SHORT_TIMER));
        sleep(LXT_SHORT_TIMER * 2);
        _exit(LXT_RESULT_SUCCESS);
    }

    LxtCheckResult(LxtWaitPidPollOptions(ChildPid, LXT_RESULT_SUCCESS, 0, LXT_SHORT_TIMER_WAIT_PID));

    //
    // Check that the signal handler is invoked within a reasonable time
    // interval.
    //

    LxtCheckResult(ChildPid = fork());
    if (ChildPid == 0)
    {
        memset(&Action, 0, sizeof(Action));
        Action.sa_handler = ITimerSignalHandler;
        LxtCheckErrnoZeroSuccess(sigaction(SIGALRM, &Action, NULL));
        memset(&g_SignalTime, 0, sizeof(g_SignalTime));
        LxtClockGetTime(CLOCK_REALTIME, &StartTime);
        LxtCheckResult(alarm(LXT_SHORT_TIMER));
        sleep(LXT_SHORT_TIMER * 2);
        if (g_SignalTime.tv_sec - StartTime.tv_sec != 1)
        {
            LxtLogError("Unexpected seconds elapsed %d", g_SignalTime.tv_sec - StartTime.tv_sec);

            _exit(1);
        }

        _exit(LXT_RESULT_SUCCESS);
    }

    LxtCheckResult(LxtWaitPidPollOptions(ChildPid, LXT_RESULT_SUCCESS, 0, LXT_SHORT_TIMER_WAIT_PID));

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    return Result;
}

void ITimerPeriodicSignalHandler(int Signal)

/*++
--*/

{
    if (Signal == SIGALRM)
    {
        ++g_SignalCount;
    }

    return;
}

int ITimerPeriodicSignal(PLXT_ARGS Args)

/*++
--*/

{

    struct sigaction Action;
    int ChildPid;
    int ExpectedWaitStatus;
    int Result;
    struct itimerval NewTimer;

    //
    // Check that the signal handler is invoked within a reasonable time
    // interval.
    //

    LxtCheckResult(ChildPid = fork());
    if (ChildPid == 0)
    {
        memset(&Action, 0, sizeof(Action));
        Action.sa_handler = ITimerPeriodicSignalHandler;
        LxtCheckErrnoZeroSuccess(sigaction(SIGALRM, &Action, NULL));
        memset(&g_SignalCount, 0, sizeof(g_SignalCount));
        memset(&NewTimer, 0, sizeof(NewTimer));
        NewTimer.it_value.tv_sec = LXT_SHORT_TIMER;
        NewTimer.it_interval.tv_usec = LXT_SHORT_TIMER_US;
        LxtCheckResult(setitimer(ITIMER_REAL, &NewTimer, NULL));
        while (sleep(LXT_SHORT_TIMER * 2) != LXT_SHORT_TIMER * 2)
        {
            LxtLogInfo("Periodic timer detected: %d", g_SignalCount);
            if (g_SignalCount >= 3)
            {
                break;
            }
        }

        LxtLogInfo("Periodic timer count: %d", g_SignalCount);
        _exit(LXT_RESULT_SUCCESS);
    }

    LxtCheckResult(LxtWaitPidPollOptions(ChildPid, LXT_RESULT_SUCCESS, 0, LXT_SHORT_TIMER_WAIT_PID));

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    return Result;
}

int NanosleepInvalidParam(PLXT_ARGS Args)

{

    struct timespec SleepDuration;
    int Result;

    SleepDuration.tv_sec = 0;
    SleepDuration.tv_nsec = 999999999;
    LxtCheckErrno(nanosleep(&SleepDuration, NULL));

    //
    // N.B. The clock_nanosleep system call returns error codes on failure
    //      instead of setting errno.
    //

    LxtCheckEqual(0, clock_nanosleep(CLOCK_MONOTONIC, 0, &SleepDuration, NULL), "%d");

    SleepDuration.tv_nsec += 1;
    LxtCheckErrnoFailure(nanosleep(&SleepDuration, NULL), EINVAL);

    //
    // N.B. The clock_nanosleep system call returns error codes on failure
    //      instead of setting errno.
    //

    LxtCheckEqual(EINVAL, clock_nanosleep(CLOCK_MONOTONIC, 0, &SleepDuration, NULL), "%d");

ErrorExit:
    return Result;
}

void TimerCreateHandler(int Signal, siginfo_t* SignalInfo, void* SignalContext)
{

    int Overrun;
    int Result;
    timer_t TimerId;

    LxtLogInfo("Caught signal %d", Signal);

    TimerId = *((timer_t*)SignalInfo->si_value.sival_ptr);
    LxtLogInfo("SignalInfo->si_value.sival_ptr = %p", SignalInfo->si_value.sival_ptr);
    LxtLogInfo("*SignalInfo->si_value.sival_ptr = %d", (long)TimerId);
    LxtLogInfo("SignalInfo->si_overrun count = %d", SignalInfo->si_overrun);
    LxtCheckResult(Overrun = LxtTimer_GetOverrun(TimerId));
    LxtLogInfo("timer_getoverrun count = %d", Overrun);
    LxtCheckEqual(Overrun, SignalInfo->si_overrun, "%d");
    LxtLogInfo("SignalInfo->si_timerid = %d", SignalInfo->_sifields._timer);

ErrorExit:
    signal(Signal, SIG_IGN);
    return;
}

void* TimerCreateSyscallThread(void* Parameter)

/*++

Routine Description:

    This routine is the timer create thread callback.

Arguments:

    Parameter - Supplies the parameter.

Return Value:

    0 on success, -1 on failure.

--*/

{

    timer_t TimerId;
    int Result;
    struct itimerspec TimerSpec;

    //
    // Query the timer and validate that it is not set.
    //

    TimerId = (timer_t)Parameter;
    LxtCheckResult(LxtTimer_GetTime(TimerId, &TimerSpec));
    LxtCheckEqual(TimerSpec.it_value.tv_sec, 0, "%lx");
    LxtCheckEqual(TimerSpec.it_value.tv_nsec, 0, "%lx");
    LxtCheckEqual(TimerSpec.it_interval.tv_sec, 0, "%lx");
    LxtCheckEqual(TimerSpec.it_interval.tv_nsec, 0, "%lx");

ErrorExit:
    return (void*)(long)Result;
}

int TimerCreateSyscall(PLXT_ARGS Args)

{

    struct itimerspec OldTimerSpec;
    int Overrun;
    int Result;
    struct sigaction SigAction;
    int Signal;
    int SignalToTest;
    struct sigevent SigEvent;
    siginfo_t SignalInfo;
    sigset_t SigMask;
    pthread_t Thread;
    void* ThreadReturn;
    timer_t TimerId;
    struct itimerspec TimerSpec;

    SignalToTest = SIGRTMIN;
    TimerId = (timer_t)-1;

    //
    // Create signal handler and temporarily block signal delivery.
    //

    SigAction.sa_flags = SA_SIGINFO;
    SigAction.sa_sigaction = TimerCreateHandler;
    sigemptyset(&SigAction.sa_mask);
    LxtCheckResult(sigaction(SignalToTest, &SigAction, NULL));

    sigemptyset(&SigMask);
    sigaddset(&SigMask, SignalToTest);
    LxtCheckResult(sigprocmask(SIG_SETMASK, &SigMask, NULL));

    //
    // Create the timer.
    //

    SigEvent.sigev_value.sival_ptr = &TimerId;
    SigEvent.sigev_signo = SignalToTest;
    SigEvent.sigev_notify = SIGEV_SIGNAL;
    LxtCheckResult(LxtTimer_Create(CLOCK_REALTIME, &SigEvent, &TimerId));
    LxtLogInfo("create_timer TimerId = %d", TimerId);

    //
    // Set the timer - verify that the initial timer state is all zeros.
    //

    memset(&TimerSpec, 0, sizeof(TimerSpec));
    TimerSpec.it_value.tv_sec = 1;
    LxtCheckResult(LxtTimer_SetTime(TimerId, 0, &TimerSpec, &OldTimerSpec));
    LxtCheckEqual(OldTimerSpec.it_value.tv_sec, 0, "%lx");
    LxtCheckEqual(OldTimerSpec.it_value.tv_nsec, 0, "%lx");
    LxtCheckEqual(OldTimerSpec.it_interval.tv_sec, 0, "%lx");
    LxtCheckEqual(OldTimerSpec.it_interval.tv_nsec, 0, "%lx");

    //
    // Query the time of the timer that was just created.
    //

    LxtCheckResult(LxtTimer_GetTime(TimerId, &OldTimerSpec));
    LxtLogInfo("OldTimerSpec.it_value.tv_sec = 0x%lx", OldTimerSpec.it_value.tv_sec);
    LxtLogInfo("OldTimerSpec.it_value.tv_nsec = 0x%lx", OldTimerSpec.it_value.tv_nsec);
    LxtLogInfo("OldTimerSpec.it_interval.tv_sec = 0x%lx", OldTimerSpec.it_interval.tv_sec);
    LxtLogInfo("OldTimerSpec.it_interval.tv_nsec = 0x%lx", OldTimerSpec.it_interval.tv_nsec);
    if ((OldTimerSpec.it_value.tv_sec > 1) || ((OldTimerSpec.it_value.tv_sec < 1) && (OldTimerSpec.it_value.tv_nsec == 0)))
    {

        Result = LXT_RESULT_FAILURE;
        LxtLogError("timer_gettime returned tv_sec %lx tv_nsec %lx", OldTimerSpec.it_value.tv_sec, OldTimerSpec.it_value.tv_nsec);

        goto ErrorExit;
    }

    LxtCheckEqual(OldTimerSpec.it_interval.tv_sec, 0, "%lx");
    LxtCheckEqual(OldTimerSpec.it_interval.tv_nsec, 0, "%lx");

    //
    // Unblock signal delivery.
    //

    LxtCheckResult(sigprocmask(SIG_UNBLOCK, &SigMask, NULL));

    //
    // Wait for the signal to be delivered.
    //

    LxtCheckErrno(Signal = sigtimedwait(&SigMask, &SignalInfo, NULL));
    LxtCheckEqual(Signal, SignalToTest, "%d");
    LxtCheckEqual(SignalInfo.si_signo, SignalToTest, "%d");
    LxtCheckEqual(SignalInfo.si_code, SI_TIMER, "%d");
    LxtLogInfo("SignalInfo->si_value.sival_ptr = %p", SignalInfo.si_value.sival_ptr);
    LxtLogInfo("SignalInfo->si_value.sival_int = %d", SignalInfo.si_value.sival_int);
    LxtLogInfo("SignalInfo->si_overrun = %d", SignalInfo.si_overrun);
    LxtCheckResult(Overrun = LxtTimer_GetOverrun(TimerId));
    LxtLogInfo("timer_getoverrun count = %d", Overrun);
    LxtCheckEqual(Overrun, SignalInfo.si_overrun, "%d");
    LxtLogInfo("SignalInfo->si_timerid = %d", SignalInfo._sifields._timer);
    LxtCheckErrnoZeroSuccess(sigpending(&SigMask));

    //
    // Create a pthread and ensure that it can see the timer.
    //

    LxtCheckResultError(pthread_create(&Thread, NULL, TimerCreateSyscallThread, (void*)TimerId));

    pthread_join(Thread, &ThreadReturn);
    LxtCheckEqual((long)ThreadReturn, 0L, "%ld");

    //
    // Delete the timer, delete twice to verify the second deletion fails.
    //

    LxtCheckResult(LxtTimer_Delete(TimerId));
    LxtCheckErrnoFailure(LxtTimer_Delete(TimerId), EINVAL);
    TimerId = LXT_INVALID_TIMER_ID;

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    if (TimerId != LXT_INVALID_TIMER_ID)
    {
        LxtTimer_Delete(TimerId);
    }

    return Result;
}

int TimerCreateInvalidParam(PLXT_ARGS Args)

{

    pid_t ChildPid;
    int Index;
    struct itimerspec OldTimerSpec;
    struct rlimit ResourceLimit = {0};
    int Result;
    struct sigevent SigEvent;
    sigset_t SigMask;
    int Status;
    timer_t TimerId;
    timer_t* TimerIdArray;
    struct itimerspec TimerSpec;

    ChildPid = -1;
    TimerId = LXT_INVALID_TIMER_ID;
    TimerIdArray = NULL;

    //
    // timer_create invalid user buffers.
    //

    SigEvent.sigev_value.sival_ptr = &TimerId;
    SigEvent.sigev_notify = SIGEV_SIGNAL;
    SigEvent.sigev_signo = SIGRTMIN;
    LxtCheckErrnoFailure(LxtTimer_Create(CLOCK_REALTIME, -1, &TimerId), EFAULT);
    LxtCheckErrnoFailure(LxtTimer_Create(CLOCK_REALTIME, &SigEvent, NULL), EFAULT);
    LxtCheckErrnoFailure(LxtTimer_Create(CLOCK_REALTIME, &SigEvent, -1), EFAULT);

    //
    // timer_create invalid clock id's.
    //

    LxtCheckErrnoFailure(LxtTimer_Create(CLOCK_MONOTONIC_RAW, &SigEvent, &TimerId), ENOTSUP);
    LxtCheckErrnoFailure(LxtTimer_Create(CLOCK_REALTIME_COARSE, &SigEvent, &TimerId), ENOTSUP);
    LxtCheckErrnoFailure(LxtTimer_Create(CLOCK_MONOTONIC_COARSE, &SigEvent, &TimerId), ENOTSUP);
    LxtCheckErrnoFailure(LxtTimer_Create(-1, &SigEvent, &TimerId), EINVAL);

    //
    // timer_create invalid sigevent structures.
    //

    //
    // Invalid notify methods.
    //

    SigEvent.sigev_notify = 5;
    LxtCheckErrnoFailure(LxtTimer_Create(CLOCK_REALTIME, &SigEvent, &TimerId), EINVAL);
    SigEvent.sigev_notify = -1;
    LxtCheckErrnoFailure(LxtTimer_Create(CLOCK_REALTIME, &SigEvent, &TimerId), EINVAL);

    //
    // Invalid signal numbers.
    //

    SigEvent.sigev_notify = SIGEV_SIGNAL;
    SigEvent.sigev_signo = 0;
    LxtCheckErrnoFailure(LxtTimer_Create(CLOCK_REALTIME, &SigEvent, &TimerId), EINVAL);
    SigEvent.sigev_signo = 65;
    LxtCheckErrnoFailure(LxtTimer_Create(CLOCK_REALTIME, &SigEvent, &TimerId), EINVAL);
    SigEvent.sigev_signo = -1;
    LxtCheckErrnoFailure(LxtTimer_Create(CLOCK_REALTIME, &SigEvent, &TimerId), EINVAL);

    //
    // N.B. timer_create with a NULL SigEvent argument succeeds.
    //

    TimerId = LXT_INVALID_TIMER_ID;
    LxtCheckResult(LxtTimer_Create(CLOCK_REALTIME, NULL, &TimerId));
    LxtLogInfo("create_timer TimerId = %d", TimerId);

    //
    // timer_settime invalid user buffers.
    //

    LxtCheckErrnoFailure(LxtTimer_SetTime(TimerId, 0, NULL, &OldTimerSpec), EINVAL);
    LxtCheckErrnoFailure(LxtTimer_SetTime(TimerId, 0, -1, &OldTimerSpec), EFAULT);

    //
    // N.B. timer_settime with NULL old value succeeds.
    //

    TimerSpec.it_value.tv_sec = 1;
    TimerSpec.it_value.tv_nsec = 0;
    TimerSpec.it_interval.tv_sec = 1;
    TimerSpec.it_interval.tv_nsec = 1;
    LxtCheckResult(LxtTimer_SetTime(TimerId, 0, &TimerSpec, NULL));

    //
    // N.B. Even though the timer_settime call fails with an invalid buffer for
    //      old value, the timer is still modified.
    //

    TimerSpec.it_interval.tv_sec = 4;
    TimerSpec.it_interval.tv_nsec = 4;
    LxtCheckErrnoFailure(LxtTimer_SetTime(TimerId, 0, &TimerSpec, -1), EFAULT);
    OldTimerSpec = TimerSpec;
    TimerSpec.it_interval.tv_sec = 5;
    TimerSpec.it_interval.tv_nsec = 5;
    LxtCheckResult(LxtTimer_SetTime(TimerId, 0, &TimerSpec, &OldTimerSpec));
    LxtCheckEqual(OldTimerSpec.it_interval.tv_sec, 4, "%lx");
    LxtCheckEqual(OldTimerSpec.it_interval.tv_nsec, 4, "%lx");

    //
    // Invalid timerspec values.
    //

    memset(&TimerSpec, 0, sizeof(TimerSpec));
    TimerSpec.it_value.tv_sec = -1;
    LxtCheckErrnoFailure(LxtTimer_SetTime(TimerId, 0, &TimerSpec, NULL), EINVAL);
    memset(&TimerSpec, 0, sizeof(TimerSpec));
    TimerSpec.it_value.tv_nsec = 999999999 + 1;
    LxtCheckErrnoFailure(LxtTimer_SetTime(TimerId, 0, &TimerSpec, NULL), EINVAL);
    memset(&TimerSpec, 0, sizeof(TimerSpec));
    TimerSpec.it_interval.tv_sec = -1;
    LxtCheckErrnoFailure(LxtTimer_SetTime(TimerId, 0, &TimerSpec, NULL), EINVAL);
    memset(&TimerSpec, 0, sizeof(TimerSpec));
    TimerSpec.it_interval.tv_nsec = 999999999 + 1;
    LxtCheckErrnoFailure(LxtTimer_SetTime(TimerId, 0, &TimerSpec, NULL), EINVAL);

    //
    // timer_getoverrun and timer_gettime invalid param.
    //

    LxtCheckErrnoFailure(LxtTimer_GetOverrun(-1), EINVAL);
    LxtCheckErrnoFailure(LxtTimer_GetTime(-1, &TimerSpec), EINVAL);
    LxtCheckErrnoFailure(LxtTimer_GetTime(TimerId, NULL), EFAULT);
    LxtCheckErrnoFailure(LxtTimer_GetTime(TimerId, -1), EFAULT);
    LxtCheckResult(LxtTimer_Delete(TimerId));
    TimerId = LXT_INVALID_TIMER_ID;

    //
    // Query how many timers can be created, ensure that we are able to create
    // exactly that many timers.
    //

    LxtCheckResult(getrlimit(RLIMIT_SIGPENDING, &ResourceLimit));
    LxtLogInfo("getrlimit(RLIMIT_SIGPENDING) Current %lu, Max %lu", ResourceLimit.rlim_cur, ResourceLimit.rlim_max);

    TimerIdArray = malloc(ResourceLimit.rlim_cur * sizeof(timer_t));
    if (TimerIdArray == NULL)
    {
        goto ErrorExit;
    }

    memset(TimerIdArray, 0xff, (ResourceLimit.rlim_cur * sizeof(timer_t)));
    for (Index = 0; Index < ResourceLimit.rlim_cur; Index += 1)
    {
        LxtCheckResult(LxtTimer_Create(CLOCK_REALTIME, NULL, &TimerIdArray[Index]));
    }

    //
    // Create one more timer, this should fail. Delete one and create another.
    //

    LxtCheckErrnoFailure(LxtTimer_Create(CLOCK_REALTIME, NULL, &TimerId), EAGAIN);
    LxtCheckResult(LxtTimer_Delete(TimerIdArray[0]));
    TimerIdArray[0] = LXT_INVALID_TIMER_ID;
    LxtCheckResult(LxtTimer_Create(CLOCK_REALTIME, NULL, &TimerId));
    LxtCheckResult(LxtTimer_Delete(TimerId));
    TimerId = LXT_INVALID_TIMER_ID;

    //
    // Set the timer limit to zero and attempt to create a new timer.
    //

    LXT_SYNCHRONIZATION_POINT_START();
    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        ResourceLimit.rlim_cur = 0;
        LxtCheckResult(setrlimit(RLIMIT_SIGPENDING, &ResourceLimit));
        LxtCheckErrnoFailure(LxtTimer_Create(CLOCK_REALTIME, NULL, &TimerId), EAGAIN);
        LXT_SYNCHRONIZATION_POINT();
        goto ErrorExit;
    }

    LXT_SYNCHRONIZATION_POINT();
    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    if (TimerId != LXT_INVALID_TIMER_ID)
    {
        LxtTimer_Delete(TimerId);
    }

    if (TimerIdArray != NULL)
    {
        for (Index = 0; Index < ResourceLimit.rlim_max; Index += 1)
        {
            if (TimerIdArray[Index] != LXT_INVALID_TIMER_ID)
            {
                LxtTimer_Delete(TimerIdArray[Index]);
            }
        }

        free(TimerIdArray);
    }

    LXT_SYNCHRONIZATION_POINT_END();
    return Result;
}

typedef struct _LXSS_BYTE_ALIGNED_TIMESPEC
{
    char Padding;
    char Buffer[10];
} LXSS_BYTE_ALIGNED_TIMESPEC;

int ClockGetTimeAlignment(PLXT_ARGS Args)

{
    int Result;
    LXSS_BYTE_ALIGNED_TIMESPEC Timespec;

    LxtLogInfo("calling clock_gettime with user buffer %p", &Timespec.Buffer);
    LxtCheckErrnoZeroSuccess(clock_gettime(CLOCK_MONOTONIC, (struct timespec*)&Timespec.Buffer));

ErrorExit:
    return Result;
}
