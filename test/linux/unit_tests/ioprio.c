/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    Ioprio.c

Abstract:

    This file is a ioprio test.

--*/

#include "lxtcommon.h"
#include "unittests.h"
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/prctl.h>
#include "lxtutil.h"

#if !defined(__amd64__) && !defined(__aarch64__)

#include <sys/capability.h>

#else

#include <sys/cdefs.h>
#include <linux/capability.h>

#define _LINUX_CAPABILITY_VERSION_3 0x20080522

#ifndef O_PATH
#define O_PATH 010000000
#endif

#endif

#define IOPRIO_UID 1007
#define IOPRIO_GID 1007

//
// When the ioprio.h header is available, these types can be removed.
//

#define LX_IOPRIO_WHO_PROCESS 1
#define LX_IOPRIO_WHO_PGRP 2
#define LX_IOPRIO_WHO_USER 3

#define LX_IOPRIO_CLASS_NONE 0
#define LX_IOPRIO_CLASS_RT 1
#define LX_IOPRIO_CLASS_BE 2
#define LX_IOPRIO_CLASS_IDLE 3

#define LX_IOPRIO_CLASS_SHIFT 13
#define LX_IOPRIO_PRIO_MASK ((1 << LX_IOPRIO_CLASS_SHIFT) - 1)
#define LX_IOPRIO_PRIO_CLASS(_mask) ((_mask) >> LX_IOPRIO_CLASS_SHIFT)
#define LX_IOPRIO_PRIO_DATA(_mask) ((_mask) & LX_IOPRIO_PRIO_MASK)
#define LX_IOPRIO_PRIO_VALUE(_class, _data) (((_class) << LX_IOPRIO_CLASS_SHIFT) | (_data))
#define LX_IO_DEFAULT_PRIORITY 4

#define LXT_NAME "Ioprio"

int IoprioVariationGetProcess(PLXT_ARGS Args);

int IoprioVariationSetProcess(PLXT_ARGS Args);

int IoprioVariationGetSetPriority(PLXT_ARGS Args);

//
// Global constants
//

static const LXT_VARIATION g_LxtVariations[] = {
    {"ioprio_get process", IoprioVariationGetProcess},
    {"getpriority / setpriority", IoprioVariationGetSetPriority},
    {"ioprio_set process", IoprioVariationSetProcess}};

int IoprioTestEntry(int Argc, char* Argv[])

/*++
--*/

{

    LXT_ARGS Args;
    int Result;

    LxtInitialize(Argc, Argv, &Args, LXT_NAME);
    LxtCheckResult(LxtRunVariations(&Args, g_LxtVariations, LXT_COUNT_OF(g_LxtVariations)));

ErrorExit:
    LxtUninitialize();
    return !LXT_SUCCESS(Result);
}

int IoprioVariationGetProcess(PLXT_ARGS Args)

/*++
--*/

{

    int DefaultPriority;
    int IoPrio;
    int Result;

    //
    // Negative variations.
    //

    LxtCheckErrnoFailure(IoPrio = LxtIoprio_get(LX_IOPRIO_WHO_PROCESS, -1), ESRCH);
    LxtCheckErrnoFailure(IoPrio = LxtIoprio_get(0, 0), EINVAL);

    //
    // Get the default priority for the current and parent thread.
    //

    LxtCheckErrno(DefaultPriority = getpriority(PRIO_PROCESS, 0));
    LxtLogInfo("DefaultPriority: %d", DefaultPriority);
    LxtCheckErrno(IoPrio = LxtIoprio_get(LX_IOPRIO_WHO_PROCESS, 0));
    LxtCheckEqual(IoPrio, LX_IO_DEFAULT_PRIORITY, "%d");
    LxtCheckErrno(IoPrio = LxtIoprio_get(LX_IOPRIO_WHO_PROCESS, getpid()));
    LxtCheckEqual(IoPrio, LX_IO_DEFAULT_PRIORITY, "%d");
    LxtCheckErrno(IoPrio = LxtIoprio_get(LX_IOPRIO_WHO_PROCESS, getppid()));
    LxtCheckEqual(IoPrio, LX_IO_DEFAULT_PRIORITY, "%d");

    //
    // Set the priority of the current thread and check that it does not change
    // the io priority.
    //

    LxtCheckErrno(setpriority(PRIO_PROCESS, 0, LX_IO_DEFAULT_PRIORITY + 1));
    LxtCheckErrno(IoPrio = LxtIoprio_get(LX_IOPRIO_WHO_PROCESS, 0));
    LxtCheckNotEqual(IoPrio, LX_IO_DEFAULT_PRIORITY + 1, "%d");

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    setpriority(PRIO_PROCESS, 0, LX_IO_DEFAULT_PRIORITY);
    return Result;
}

int IoprioVariationSetProcess(PLXT_ARGS Args)

/*++
--*/

{

    struct __user_cap_data_struct CapData[2];
    struct __user_cap_header_struct CapHeader;
    int ChildPid;
    int DefaultPriority;
    int Index;
    int InitialIoPrio;
    int IoPrio;
    int NewIoPrio;
    int Result;

    ChildPid = -1;

    //
    // Get the default priority for the current and parent thread.
    //
    // N.B. Setting the IO priority to this default value fails.
    //

    LxtCheckErrno(InitialIoPrio = LxtIoprio_get(LX_IOPRIO_WHO_PROCESS, 0));
    LxtLogInfo("InitialIoPrio = %d", InitialIoPrio);
    IoPrio = InitialIoPrio;
    LxtCheckErrnoFailure(LxtIoprio_set(LX_IOPRIO_WHO_PROCESS, 0, IoPrio), EINVAL);

    //
    // LX_IOPRIO_CLASS_NONE
    //

    for (Index = 0; Index < 1; Index += 1)
    {
        IoPrio = LX_IOPRIO_PRIO_VALUE(LX_IOPRIO_CLASS_NONE, Index);
        LxtCheckErrno(LxtIoprio_set(LX_IOPRIO_WHO_PROCESS, 0, IoPrio));
        LxtCheckErrno(NewIoPrio = LxtIoprio_get(LX_IOPRIO_WHO_PROCESS, 0));
        LxtCheckEqual(IoPrio, NewIoPrio, "%d");
    }

    IoPrio = LX_IOPRIO_PRIO_VALUE(LX_IOPRIO_CLASS_NONE, Index);
    LxtCheckErrnoFailure(LxtIoprio_set(LX_IOPRIO_WHO_PROCESS, 0, IoPrio), EINVAL);

    //
    // LX_IOPRIO_CLASS_RT
    //

    for (Index = 0; Index < 8; Index += 1)
    {
        IoPrio = LX_IOPRIO_PRIO_VALUE(LX_IOPRIO_CLASS_RT, Index);
        LxtCheckErrno(LxtIoprio_set(LX_IOPRIO_WHO_PROCESS, 0, IoPrio));
        LxtCheckErrno(NewIoPrio = LxtIoprio_get(LX_IOPRIO_WHO_PROCESS, 0));
        LxtCheckEqual(IoPrio, NewIoPrio, "%d");
    }

    IoPrio = LX_IOPRIO_PRIO_VALUE(LX_IOPRIO_CLASS_RT, Index);
    LxtCheckErrnoFailure(LxtIoprio_set(LX_IOPRIO_WHO_PROCESS, 0, IoPrio), EINVAL);

    //
    // LX_IOPRIO_CLASS_BE
    //

    for (Index = 0; Index < 8; Index += 1)
    {
        IoPrio = LX_IOPRIO_PRIO_VALUE(LX_IOPRIO_CLASS_BE, Index);
        LxtCheckErrno(LxtIoprio_set(LX_IOPRIO_WHO_PROCESS, 0, IoPrio));
        LxtCheckErrno(NewIoPrio = LxtIoprio_get(LX_IOPRIO_WHO_PROCESS, 0));
        LxtCheckEqual(IoPrio, NewIoPrio, "%d");
    }

    IoPrio = LX_IOPRIO_PRIO_VALUE(LX_IOPRIO_CLASS_BE, Index);
    LxtCheckErrnoFailure(LxtIoprio_set(LX_IOPRIO_WHO_PROCESS, 0, IoPrio), EINVAL);

    //
    // LX_IOPRIO_CLASS_IDLE
    //

    for (Index = 0; Index < 0x8000; Index += 1)
    {
        IoPrio = LX_IOPRIO_PRIO_VALUE(LX_IOPRIO_CLASS_IDLE, Index);
        LxtCheckErrno(LxtIoprio_set(LX_IOPRIO_WHO_PROCESS, 0, IoPrio));
        LxtCheckErrno(NewIoPrio = LxtIoprio_get(LX_IOPRIO_WHO_PROCESS, 0));
        LxtCheckEqual(IoPrio, NewIoPrio, "%d");
    }

    IoPrio = LX_IOPRIO_PRIO_VALUE(LX_IOPRIO_CLASS_IDLE, Index);
    LxtCheckErrnoFailure(LxtIoprio_set(LX_IOPRIO_WHO_PROCESS, 0, IoPrio), EINVAL);

    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        IoPrio = LX_IOPRIO_PRIO_VALUE(LX_IOPRIO_CLASS_NONE, 0);
        LxtCheckErrno(LxtIoprio_set(LX_IOPRIO_WHO_PROCESS, getppid(), IoPrio));

        memset(&CapData, 0, sizeof(CapData));
        memset(&CapHeader, 0, sizeof(CapHeader));
        CapHeader.version = _LINUX_CAPABILITY_VERSION_3;
        CapData[CAP_TO_INDEX(CAP_SYS_ADMIN)].permitted |= CAP_TO_MASK(CAP_SYS_ADMIN);
        CapData[0].effective = CapData[0].permitted;
        CapData[1].effective = CapData[1].permitted;

        //
        // Drop all privileges but CAP_SYS_ADMIN.
        //

        LxtCheckErrno(prctl(PR_SET_KEEPCAPS, 1));
        LxtCheckErrno(setgid(IOPRIO_UID));
        LxtCheckErrno(setuid(IOPRIO_GID));
        LxtCheckErrno(LxtCapSet(&CapHeader, CapData));

        //
        // Attempt to change parent's io priority now that UID / GID do not match.
        //

        IoPrio = LX_IOPRIO_PRIO_VALUE(LX_IOPRIO_CLASS_NONE, 0);
        LxtCheckErrnoFailure(LxtIoprio_set(LX_IOPRIO_WHO_PROCESS, getppid(), IoPrio), EPERM);

        for (Index = 0; Index < 8; Index += 1)
        {
            IoPrio = LX_IOPRIO_PRIO_VALUE(LX_IOPRIO_CLASS_RT, Index);
            LxtCheckErrno(LxtIoprio_set(LX_IOPRIO_WHO_PROCESS, 0, IoPrio));
            LxtCheckErrno(NewIoPrio = LxtIoprio_get(LX_IOPRIO_WHO_PROCESS, 0));
            LxtCheckEqual(IoPrio, NewIoPrio, "%d");
        }

        IoPrio = LX_IOPRIO_PRIO_VALUE(LX_IOPRIO_CLASS_RT, Index);
        LxtCheckErrnoFailure(LxtIoprio_set(LX_IOPRIO_WHO_PROCESS, 0, IoPrio), EINVAL);

        //
        // Drop all privileges.
        //

        memset(&CapData, 0, sizeof(CapData));
        memset(&CapHeader, 0, sizeof(CapHeader));
        CapHeader.version = _LINUX_CAPABILITY_VERSION_3;
        CapData[0].effective = CapData[0].permitted;
        CapData[1].effective = CapData[1].permitted;
        LxtCheckErrno(LxtCapSet(&CapHeader, CapData));
        IoPrio = LX_IOPRIO_PRIO_VALUE(LX_IOPRIO_CLASS_RT, 0);
        LxtCheckErrnoFailure(LxtIoprio_set(LX_IOPRIO_WHO_PROCESS, 0, IoPrio), EPERM);
        IoPrio = LX_IOPRIO_PRIO_VALUE(LX_IOPRIO_CLASS_RT, 8);
        LxtCheckErrnoFailure(LxtIoprio_set(LX_IOPRIO_WHO_PROCESS, 0, IoPrio), EPERM);

        //
        // LX_IOPRIO_CLASS_NONE
        //

        IoPrio = LX_IOPRIO_PRIO_VALUE(LX_IOPRIO_CLASS_NONE, 0);
        LxtCheckErrno(LxtIoprio_set(LX_IOPRIO_WHO_PROCESS, 0, IoPrio));
        IoPrio = LX_IOPRIO_PRIO_VALUE(LX_IOPRIO_CLASS_NONE, 1);
        LxtCheckErrnoFailure(LxtIoprio_set(LX_IOPRIO_WHO_PROCESS, 0, IoPrio), EINVAL);

        //
        // LX_IOPRIO_CLASS_BE
        //

        for (Index = 0; Index < 8; Index += 1)
        {
            IoPrio = LX_IOPRIO_PRIO_VALUE(LX_IOPRIO_CLASS_BE, Index);
            LxtCheckErrno(LxtIoprio_set(LX_IOPRIO_WHO_PROCESS, 0, IoPrio));
            LxtCheckErrno(NewIoPrio = LxtIoprio_get(LX_IOPRIO_WHO_PROCESS, 0));
            LxtCheckEqual(IoPrio, NewIoPrio, "%d");
        }

        IoPrio = LX_IOPRIO_PRIO_VALUE(LX_IOPRIO_CLASS_BE, Index);
        LxtCheckErrnoFailure(LxtIoprio_set(LX_IOPRIO_WHO_PROCESS, 0, IoPrio), EINVAL);
        goto ErrorExit;
    }

    LxtCheckResult(LxtWaitPidPoll(ChildPid, LXT_RESULT_SUCCESS));

    //
    // Negative variations.
    //

    IoPrio = LX_IOPRIO_PRIO_VALUE(LX_IOPRIO_CLASS_NONE, 0);
    LxtCheckErrnoFailure(LxtIoprio_set(LX_IOPRIO_WHO_PROCESS, -1, IoPrio), ESRCH);
    LxtCheckErrnoFailure(LxtIoprio_set(0, 0, IoPrio), EINVAL);
    IoPrio = LX_IOPRIO_PRIO_VALUE(LX_IOPRIO_CLASS_IDLE + 1, 0);
    LxtCheckErrnoFailure(LxtIoprio_set(LX_IOPRIO_WHO_PROCESS, 0, IoPrio), EINVAL);
    IoPrio = LX_IOPRIO_PRIO_VALUE(-1, 0);
    LxtCheckErrnoFailure(LxtIoprio_set(LX_IOPRIO_WHO_PROCESS, 0, IoPrio), EINVAL);
    LxtCheckErrnoFailure(LxtIoprio_set(0, 0, IoPrio), EINVAL);
    IoPrio = LX_IOPRIO_PRIO_VALUE(LX_IOPRIO_CLASS_NONE, 0);
    LxtCheckErrnoFailure(LxtIoprio_set(LX_IOPRIO_WHO_USER + 1, 0, IoPrio), EINVAL);
    LxtCheckErrnoFailure(LxtIoprio_set(LX_IOPRIO_WHO_USER + 1, -1, IoPrio), EINVAL);
    IoPrio = LX_IOPRIO_PRIO_VALUE(LX_IOPRIO_CLASS_NONE, 1);
    LxtCheckErrnoFailure(LxtIoprio_set(LX_IOPRIO_WHO_USER + 1, 0, IoPrio), EINVAL);
    LxtCheckErrnoFailure(LxtIoprio_set(LX_IOPRIO_WHO_USER + 1, -1, IoPrio), EINVAL);

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    if (ChildPid == 0)
    {
        _exit(Result);
    }

    return Result;
}

int IoprioSetPriority(int Which, int Who, int Priority, int ExpectedPriority)

{

    int Result;

    LxtCheckErrno(setpriority(Which, Who, Priority));
    errno = 0;
    Result = getpriority(Which, Who);
    if ((Result != ExpectedPriority) || (errno != 0))
    {
        LxtLogError("getpriority returned %d expected %d - errno %d", Result, ExpectedPriority, errno);

        Result = LXT_RESULT_FAILURE;
        goto ErrorExit;
    }

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    return Result;
}

int IoprioVariationGetSetPriority(PLXT_ARGS Args)

{
    int ChildPid;
    int OriginalPriority;
    pid_t ParentPid;
    int Result;
    struct rlimit Rlimit;

    ChildPid = -1;
    errno = 0;
    OriginalPriority = getpriority(PRIO_PROCESS, 0);
    LxtLogInfo("OriginalPriority %d", OriginalPriority);

    //
    // Validate setting the priority to a negative number.
    //

    LxtCheckResult(IoprioSetPriority(PRIO_PROCESS, 0, -1, -1));

    //
    // Validate setting the priority to a positive number.
    //

    LxtCheckResult(IoprioSetPriority(PRIO_PROCESS, 0, 5, 5));

    //
    // Validate the lower bound of the priority (-20).
    //

    LxtCheckResult(IoprioSetPriority(PRIO_PROCESS, 0, -20, -20));
    LxtCheckResult(IoprioSetPriority(PRIO_PROCESS, 0, -21, -20));
    LxtCheckResult(IoprioSetPriority(PRIO_PROCESS, 0, -444, -20));

    //
    // Validate the upper bound of the priority (19).
    //

    LxtCheckResult(IoprioSetPriority(PRIO_PROCESS, 0, 19, 19));
    LxtCheckResult(IoprioSetPriority(PRIO_PROCESS, 0, 20, 19));
    LxtCheckResult(IoprioSetPriority(PRIO_PROCESS, 0, 444, 19));

    //
    // Invalid parameter variations.
    //

    LxtCheckErrnoFailure(getpriority(-1, 0), EINVAL);

    //
    // Permissions checks. Reset the priority to a known value.
    //

    LxtCheckResult(IoprioSetPriority(PRIO_PROCESS, 0, 5, 5));
    ParentPid = getpid();
    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {

        //
        // Change the UID to drop CAP_SYS_NICE and verify that the priority can
        // be set to the same value.
        //

        LxtCheckErrno(setuid(1000));
        LxtCheckResult(IoprioSetPriority(PRIO_PROCESS, 0, 5, 5));

        //
        // Attempt to lower the priority.
        //

        LxtCheckResult(IoprioSetPriority(PRIO_PROCESS, 0, 6, 6));

        //
        // Attempt to raise the priority (should fail).
        //

        LxtCheckErrnoFailure(setpriority(PRIO_PROCESS, 0, 4), EACCES);

        //
        // Attempt to modify the parent's priority (should fail).
        //

        LxtCheckErrnoFailure(setpriority(PRIO_PROCESS, ParentPid, 5), EPERM);
        goto ErrorExit;
    }

    //
    // Wait for the child to exit.
    //

    LxtCheckErrno(LxtWaitPidPoll(ChildPid, LXT_RESULT_SUCCESS));

    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {

        //
        // Change the nice rlimit value.
        //

        Rlimit.rlim_cur = 19;
        Rlimit.rlim_max = 19;
        LxtCheckErrno(setrlimit(RLIMIT_NICE, &Rlimit));

        //
        // Change the UID to drop CAP_SYS_NICE.
        //

        LxtCheckErrno(setuid(1000));

        //
        // Setting the priority to 1 should succeed.
        //
        LxtCheckResult(IoprioSetPriority(PRIO_PROCESS, 0, 1, 1));

        //
        // Setting the priority to 0 should fail.
        //

        LxtCheckErrnoFailure(setpriority(PRIO_PROCESS, 0, 0), EACCES);
        goto ErrorExit;
    }

    //
    // Wait for the child to exit.
    //

    LxtCheckErrno(LxtWaitPidPoll(ChildPid, LXT_RESULT_SUCCESS));
    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    if (ChildPid == 0)
    {
        _exit(Result);
    }

    if (OriginalPriority > 0)
    {
        setpriority(PRIO_PROCESS, 0, OriginalPriority);
    }

    return Result;
}
