/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    dup.c

Abstract:

    This file is a test for the dup, dup2 system call.

--*/

#include "lxtcommon.h"
#include "unittests.h"
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>

#if !defined(__amd64__) && !defined(__aarch64__)
#include <sys/limits.h>
#endif

#include <fcntl.h>

#define LXT_NAME "Dup"
#define FD_INVALID (-1)
#define FD_STDIN 0
#define FD_STDOUT 1
#define FD_ERR 2
#define MY_F_DUPFD_CLOEXEC 1030

int Dup0(PLXT_ARGS Args);

int Dup1(PLXT_ARGS Args);

int Dup2(PLXT_ARGS Args);

//
// Global constants
//

static const LXT_VARIATION g_LxtVariations[] = {{"Dup Basic", Dup0}, {"Dup Descriptor Flags", Dup1}, {"FCNTL Dup Error Cases", Dup2}};

int DupTestEntry(int Argc, char* Argv[])

/*++

Routine Description:

    This routine main entry point for the test for dup, dup2 system call.

Arguments:

    Argc - Supplies the number of command line arguments.

    Argv - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

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

int Dup0(PLXT_ARGS Args)

/*++

Routine Description:

    This routine validates that the dup and dup2 system call and the
    various parameter variances.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int Result;
    int DupStdIn;
    int DupStdOut;
    int DupFd2;

    LxtCheckErrnoFailure(dup(FD_INVALID), EBADF);
    LxtCheckErrnoFailure(dup(__SHRT_MAX__), EBADF);
    LxtCheckResult((DupStdIn = dup(FD_STDIN)));
    LxtCheckResult(close(FD_STDIN));
    LxtCheckResult((DupStdOut = dup(FD_STDOUT)));

    //
    // Since we just closed STDIN, duping STDOUT should take the position
    // of STDIN
    //

    if (DupStdOut != FD_STDIN)
    {
        LxtLogError("Dup, expected FD return value(%d), actual(%d).", FD_STDIN, DupStdOut);
        Result = LXT_RESULT_FAILURE;
        goto ErrorExit;
    }

    //
    // Now forcefully restore STDIN to its rightful position using dup2
    //

    LxtCheckResult((DupFd2 = dup2(DupStdIn, FD_STDIN)));
    if (DupFd2 != FD_STDIN)
    {
        LxtLogError("Dup, expected FD return value(%d), actual(%d).", FD_STDIN, DupFd2);
        Result = LXT_RESULT_FAILURE;
        goto ErrorExit;
    }

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    return Result;
}

int Dup1(PLXT_ARGS Args)

/*++

Routine Description:

    This routine validates that the variations of the dup system calls,
    and their behavior w.r.t sharing file descriptor flags.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int Result;
    int FdProcSelf;
    int FdProcSelfFlags;
    int FdDup;
    int FdDupFlags;
    int FdTemp;

    LxtCheckResult((FdProcSelf = open("/proc/self", O_RDONLY | O_CLOEXEC)));
    LxtCheckResult((FdDup = dup(FdProcSelf)));
    LxtCheckResult((FdProcSelfFlags = fcntl(FdProcSelf, F_GETFD)));
    LxtCheckResult((FdDupFlags = fcntl(FdDup, F_GETFD)));

    if ((FdProcSelfFlags & FD_CLOEXEC) == 0)
    {
        LxtLogError("/proc/self FD should have the 'FD_CLOEXEC' flag set, ", "but it is not. FD Flags(%d).", FdProcSelfFlags);
        Result = LXT_RESULT_FAILURE;
        goto ErrorExit;
    }

    //
    // Dup should not share file descriptor flags
    //

    if ((FdDupFlags & FD_CLOEXEC) != 0)
    {
        LxtLogError(
            "Dup should not share File Descriptor Flags between "
            "the old and new descriptor. New Descriptor Flags(%d).",
            FdDupFlags);
        Result = LXT_RESULT_FAILURE;
        goto ErrorExit;
    }

    LxtCheckResult(close(FdDup));
    LxtCheckResult((FdTemp = fcntl(FdProcSelf, F_DUPFD, FdDup)));
    if (FdTemp != FdDup)
    {
        LxtLogError(
            "fcntl(F_DUPFD) should return(%d), but "
            "it returned fd(%d).",
            FdDup,
            FdTemp);
        Result = LXT_RESULT_FAILURE;
        goto ErrorExit;
    }

    LxtCheckResult((FdDupFlags = fcntl(FdDup, F_GETFD)));

    //
    // fcntl(F_DUPFD) should not set the FD_CLOEXEC in the new descriptor
    //

    if ((FdDupFlags & FD_CLOEXEC) != 0)
    {
        LxtLogError(
            "fcntl(F_DUPFD) should not share File Descriptor Flags "
            "between the old and new descriptor. New Descriptor "
            "Flags(%d).",
            FdDupFlags);
        Result = LXT_RESULT_FAILURE;
        goto ErrorExit;
    }

    LxtCheckResult(close(FdDup));
    LxtCheckResult((FdTemp = fcntl(FdProcSelf, MY_F_DUPFD_CLOEXEC, FdDup)));
    if (FdTemp != FdDup)
    {
        LxtLogError(
            "fcntl(F_DUPFD_CLOEXEC) should return(%d), but "
            "it returned fd(%d).",
            FdDup,
            FdTemp);
        Result = LXT_RESULT_FAILURE;
        goto ErrorExit;
    }

    LxtCheckResult((FdDupFlags = fcntl(FdDup, F_GETFD)));

    //
    // fcntl(F_DUPFD_CLOEXEC) should set the FD_CLOEXEC in the new descriptor
    //

    if ((FdDupFlags & FD_CLOEXEC) == 0)
    {
        LxtLogError(
            "fcntl(F_DUPFD_CLOEXEC) should set the FD_CLOEXEC flag "
            "in the new descriptor. New Descriptor "
            "Flags(%d).",
            FdDupFlags);
        Result = LXT_RESULT_FAILURE;
        goto ErrorExit;
    }

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    return Result;
}

int Dup2(PLXT_ARGS Args)

/*++

Routine Description:

    This routine validates the error cases for FCNTL calls related to dup.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int Result;

    LxtCheckErrnoFailure(fcntl(FD_INVALID, F_DUPFD, 0), EBADF);
    LxtCheckErrnoFailure(fcntl(FD_INVALID, MY_F_DUPFD_CLOEXEC, 0), EBADF);
    LxtCheckErrnoFailure(fcntl(FD_STDIN, F_DUPFD, FD_INVALID), EINVAL);
    LxtCheckErrnoFailure(fcntl(FD_STDIN, MY_F_DUPFD_CLOEXEC, FD_INVALID), EINVAL);

    //
    // TODO: Add cases where the FD to dup into in FCNTL is > max allowed
    //

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    return Result;
}