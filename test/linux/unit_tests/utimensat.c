/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    utimensat.c

Abstract:

    This file contains test routines for the utimensat and utimes system
    calls.

--*/

#include "lxtcommon.h"
#include "unittests.h"
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/time.h>

#if !defined(__amd64__) && !defined(__aarch64__)
#include <sys/capability.h>
#endif

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <libmount/libmount.h>
#include "lxtfs.h"
#include "lxtmount.h"

#define LXT_NAME "Utimensat"

LXT_VARIATION_HANDLER TestBasicFunctions;
LXT_VARIATION_HANDLER TestUtimes;
LXT_VARIATION_HANDLER TestInvalid;
LXT_VARIATION_HANDLER TestPermissions;

//
// Global constants
//

const char ChildFileName[] = "testfile";
const char ChildFileFullPath[] = "/data/test_utimensat/testfile";
const char DirPath[] = "/data/test_utimensat";
const char LinkFileName[] = "testlink";
const char LinkFullPath[] = "/data/test_utimensat/testlink";

static const LXT_VARIATION g_LxtVariations[] = {
    {"Test basic functionality", TestBasicFunctions},
    {"Test utimes", TestUtimes},
    {"Test invalid parameters", TestInvalid},
    {"Test permissions", TestPermissions},
};

typedef struct timespec TIMESPEC, *PTIMESPEC;

enum
{
    PermissionsRoot,
    PermissionsOmit,
    PermissionsNow,
    PermissionsSet,
    PermissionsNowOmit,
    PermissionsOmitNow,
    PermissionsBeyondMax
};

typedef struct _PERMISSIONS_TEST_CASE
{
    TIMESPEC SetTime[2];
} PERMISSIONS_TEST_CASE, *PPERMISSIONS_TEST_CASE;

PERMISSIONS_TEST_CASE PermissionsTest[] = {
    {{{1234567, 98765432}, {4444444, 5555555}}},
    {{{1234567, UTIME_OMIT}, {4444444, UTIME_OMIT}}},
    {{{1234567, UTIME_NOW}, {4444444, UTIME_NOW}}},
    {{{1234567, 11111111}, {4444444, 22222222}}},
    {{{1234567, UTIME_NOW}, {4444444, UTIME_OMIT}}},
    {{{1234567, UTIME_OMIT}, {4444444, UTIME_NOW}}}};

typedef struct _PERMISSIONS_FILE
{
    const char* Filename;
    uid_t Owner;
    mode_t Mode;
    int Changeable[PermissionsBeyondMax];
} PERMISSIONS_FILE, *PPERMISSIONS_FILE;

enum
{
    OwnerMatchingThread = 1000,
    OwnerNotMatchingThread
};

PERMISSIONS_FILE PermissionsFiles[] = {
    {"OwnedAndWritable", OwnerMatchingThread, 0666, {0, 0, 0, 0, 0, 0}},
    {"OwnedReadonly", OwnerMatchingThread, 0444, {0, 0, 0, 0, 0, 0}},
    {"OwnedWriteonly", OwnerMatchingThread, 0222, {0, 0, 0, 0, 0, 0}},
    {"UnownedAndWritable", OwnerNotMatchingThread, 0666, {0, 0, 0, EPERM, EPERM, EPERM}},
    {"UnownedAndReadonly", OwnerNotMatchingThread, 0444, {0, 0, EACCES, EPERM, EPERM, EPERM}},
    {"UnownedAndWriteonly", OwnerNotMatchingThread, 0222, {0, 0, 0, EPERM, EPERM, EPERM}}};

int TestBasicFunctions(PLXT_ARGS Args)

/*++

Routine Description:

    This routine executes basic test functions, including setting timestamps
    to a range of values including UTIME_NOW or UTIME_OMIT, on a range of
    different ways to specify the target file, including relative, absolute,
    descriptor, via symbolic link, on a symbolic link, and validating that
    the expected outcome occurs.

Arguments:

    Args - Supplies a pointer to test arguments.

Return Value:

    0 if all variations complete successfully, -1 if they do not.

--*/

{

    int Flags;
    int Result;

    //
    // If running on wslfs, timestamps use NT precision.
    //

    Flags = 0;
    if (g_LxtFsInfo.Flags.DrvFsBehavior != 0)
    {
        LxtLogInfo("Using NT precision timestamps.");
        Flags = FS_UTIME_NT_PRECISION;
    }

    Result = LxtFsUtimeBasicCommon(DirPath, Flags);

ErrorExit:
    return Result;
}

int TestUtimes(PLXT_ARGS Args)

/*++

Routine Description:

    This routine executes test functions specific to the utimes syscall.

Arguments:

    Args - Supplies a pointer to test arguments.

Return Value:

    0 if all variations complete successfully, -1 if they do not.

--*/

{

    int Fd;
    int Result;
    struct timeval SetTimeVal[2];

    LxtCheckErrno(Fd = creat(ChildFileName, 0777));
    memset(SetTimeVal, 0, sizeof(SetTimeVal));
    LxtCheckResult(utimes(ChildFileName, SetTimeVal));
    LxtCheckResult(utimes(ChildFileName, NULL));

    //
    // Invalid parameter variations.
    //

    memset(SetTimeVal, 0, sizeof(SetTimeVal));
    SetTimeVal[1].tv_usec = 999999 + 1;
    LxtCheckErrnoFailure(utimes(ChildFileName, SetTimeVal), EINVAL);
    memset(SetTimeVal, 0, sizeof(SetTimeVal));
    SetTimeVal[1].tv_usec = 999999 + 1;
    LxtCheckErrnoFailure(utimes(ChildFileName, SetTimeVal), EINVAL);

ErrorExit:
    if (Fd != -1)
    {
        LxtClose(Fd);
    }

    return Result;
}

int TestInvalid(PLXT_ARGS Args)

/*++

Routine Description:

    This routine executes test functions which are expected to fail, and
    validates that they fail with the correct error.  This includes invalid
    descriptors, flags, nonexistent files, or invalid timestamps.

Arguments:

    Args - Supplies a pointer to test arguments.

Return Value:

    0 if all variations complete successfully, -1 if they do not.

--*/

{

    int Fd;
    int Result;
    TIMESPEC SetTime[2];
    struct timeval SetTimeVal[2];

    Fd = -1;
    memset(SetTime, 0, sizeof(SetTime));
    memset(SetTimeVal, 0, sizeof(SetTimeVal));

    LxtCheckErrnoFailure(utimensat(12345, ChildFileName, SetTime, 0), EBADF);

    LxtCheckErrnoFailure(utimensat(0, ChildFileFullPath, SetTime, 0x70000000), EINVAL);

    LxtCheckErrnoFailure(utimensat(AT_FDCWD, "bogus", SetTime, 0), ENOENT);

    LxtCheckResult(Fd = open(ChildFileFullPath, O_RDWR));

    LxtCheckErrnoFailure(utimensat(Fd, "bogus", SetTime, 0), ENOTDIR);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnonnull"

    LxtCheckErrnoFailure(utimensat(Fd, NULL, SetTime, AT_SYMLINK_NOFOLLOW), EINVAL);

#pragma GCC diagnostic pop

    LxtClose(Fd);
    Fd = -1;

    SetTime[0].tv_nsec = 1000000000;
    LxtCheckErrnoFailure(utimensat(0, ChildFileFullPath, SetTime, 0), EINVAL);

    SetTimeVal[0].tv_usec = 1000000;
    LxtCheckErrnoFailure(utimes(ChildFileFullPath, SetTimeVal), EINVAL);

    SetTimeVal[0].tv_usec = UTIME_NOW;
    LxtCheckErrnoFailure(utimes(ChildFileFullPath, SetTimeVal), EINVAL);

    SetTimeVal[0].tv_usec = UTIME_OMIT;
    LxtCheckErrnoFailure(utimes(ChildFileFullPath, SetTimeVal), EINVAL);

    SetTimeVal[0].tv_usec = 0;
    LxtCheckErrnoFailure(utimes("bogus", SetTimeVal), ENOENT);
    Result = 0;

ErrorExit:
    if (Fd != -1)
    {
        LxtClose(Fd);
    }

    return Result;
}

int TestPermissions(PLXT_ARGS Args)

/*++

Routine Description:

    This routine executes test functions that should succeed or fail
    depending on inputs and user states, and validates that they fail with
    the expected code.  In particular, validates that callers without
    privilege or file ownership cannot set file timestamps, and those
    without privilege, file ownership or write access cannot set timestamps
    to current.

Arguments:

    Args - Supplies a pointer to test arguments.

Return Value:

    0 if all variations complete successfully, -1 if they do not.

--*/

{

    pid_t ChildPid;
    int Fd;
    unsigned int FileNumber;
    int Result;
    unsigned int TestCase;

    Result = chdir(DirPath);
    if (Result < 0)
    {
        LxtLogError("Could not change directory: %d", Result);
        Result = -1;
        goto ErrorExit;
    }

    for (FileNumber = 0; FileNumber < LXT_COUNT_OF(PermissionsFiles); FileNumber += 1)
    {
        Fd = open(PermissionsFiles[FileNumber].Filename, O_CREAT | O_RDWR, PermissionsFiles[FileNumber].Mode);
        if ((Fd < 0) && (errno != EEXIST))
        {
            LxtLogError("Could not create file: %d", Result);
            Result = -1;
            goto ErrorExit;
        }

        Result = fchown(Fd, PermissionsFiles[FileNumber].Owner, PermissionsFiles[FileNumber].Owner);
        if (Result < 0)
        {
            LxtLogError("Could not change owner: %d", Result);
            Result = -1;
            goto ErrorExit;
        }

        Result = fchmod(Fd, PermissionsFiles[FileNumber].Mode);
        if (Result < 0)
        {
            LxtLogError("Could not change mode: %d", Result);
            Result = -1;
            goto ErrorExit;
        }

        close(Fd);
        Fd = 0;
    }

    //
    // While still root, check that we can perform all of the things it should
    // be able to do.
    //

    TestCase = PermissionsRoot;

    for (FileNumber = 0; FileNumber < LXT_COUNT_OF(PermissionsFiles); FileNumber += 1)
    {
        Result = utimensat(AT_FDCWD, PermissionsFiles[FileNumber].Filename, PermissionsTest[TestCase].SetTime, 0);
        if (PermissionsFiles[FileNumber].Changeable[TestCase] == 0)
        {
            if (Result < 0)
            {
                LxtLogError("Could not change time as expected, file %s case %i, Result %d", PermissionsFiles[FileNumber].Filename, TestCase, Result);
                Result = -1;
                goto ErrorExit;
            }
        }
        else
        {
            if ((Result == 0) || (errno != PermissionsFiles[FileNumber].Changeable[TestCase]))
            {

                LxtLogError(
                    "Could change time, or failed with the wrong code, file %s case %i, Result %d, errno %i, expected %i",
                    PermissionsFiles[FileNumber].Filename,
                    TestCase,
                    Result,
                    errno,
                    PermissionsFiles[FileNumber].Changeable[TestCase]);
                Result = -1;
                goto ErrorExit;
            }
        }
    }

    //
    // Set to user that should owns some of the files, and lacks privilege
    // to access others
    //

    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        Result = setuid(OwnerMatchingThread);
        if (Result < 0)
        {
            LxtLogError("Could not setuid: %d", Result);
            Result = -1;
            goto ErrorExit;
        }

        //
        // Advance to the next test (the first one was performed above), and continue
        //

        for (TestCase += 1; TestCase < PermissionsBeyondMax; TestCase += 1)
        {
            for (FileNumber = 0; FileNumber < LXT_COUNT_OF(PermissionsFiles); FileNumber += 1)
            {
                Result = utimensat(AT_FDCWD, PermissionsFiles[FileNumber].Filename, PermissionsTest[TestCase].SetTime, 0);
                if (PermissionsFiles[FileNumber].Changeable[TestCase] == 0)
                {
                    if (Result < 0)
                    {
                        LxtLogError("Could not change time as expected, file %s case %i, Result %d", PermissionsFiles[FileNumber].Filename, TestCase, Result);
                        Result = -1;
                        goto ErrorExit;
                    }
                }
                else
                {
                    if ((Result == 0) || (errno != PermissionsFiles[FileNumber].Changeable[TestCase]))
                    {

                        LxtLogError(
                            "Could change time, or failed with the wrong code, file %s case %i, Result %d, errno %i, expected %i",
                            PermissionsFiles[FileNumber].Filename,
                            TestCase,
                            Result,
                            errno,
                            PermissionsFiles[FileNumber].Changeable[TestCase]);
                        Result = -1;
                        goto ErrorExit;
                    }
                }
            }
        }

        exit(0);
    }

    LxtCheckResult(LxtWaitPidPoll(ChildPid, 0));
    Result = 0;

ErrorExit:
    for (FileNumber = 0; FileNumber < LXT_COUNT_OF(PermissionsFiles); FileNumber += 1)
    {
        unlink(PermissionsFiles[FileNumber].Filename);
    }

    return Result;
}

int UtimensatTestEntry(int Argc, char* Argv[])
{
    LXT_ARGS Args;
    int Fd;
    int Result;

    Fd = -1;
    LxtCheckResult(LxtInitialize(Argc, Argv, &Args, LXT_NAME));
    LxtCheckResult(LxtFsUtimeCreateTestFiles(DirPath, 0));
    LxtCheckResult(LxtRunVariations(&Args, g_LxtVariations, LXT_COUNT_OF(g_LxtVariations)));

ErrorExit:
    if (Fd >= 0)
    {
        close(Fd);
    }

    LxtFsUtimeCleanupTestFiles(DirPath);
    LxtUninitialize();
    return !LXT_SUCCESS(Result);
}
