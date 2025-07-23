/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    resourcelimit.c

Abstract:

    This file contains unit test for set and get resource limits.

--*/

#include "lxtcommon.h"
#include "unittests.h"
#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>

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

#define LXT_NAME "resourcelimits"
#define LXT_RESOURCE_LIMIT_TEST_FILE "rlimit_testfile"
#define LXT_RESOURCE_LIMIT_UID 1024
#define LXT_RESOURCE_LIMIT_GID 1024
#define LXT_NOFILE (10)
#define LXT_NR_OPEN (1024 * 1024)

int ResourceLimitTest(PLXT_ARGS Args);

int ResourceLimitNoFile(PLXT_ARGS Args);

int PrlimitTest(PLXT_ARGS Args);

//
// Global constants
//

static const LXT_VARIATION g_LxtVariations[] = {
    {"Resource Limit Test", ResourceLimitTest}, {"RLIMIT_NOFILE", ResourceLimitNoFile}, {"prlimit64 test", PrlimitTest}};

int ResourceLimitsTestEntry(int Argc, char* Argv[])

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

int ResourceLimitTest(PLXT_ARGS Args)

/*++
--*/

{

    int Index;
    struct rlimit ResourceLimit;
    int Result;
    int Status;

    //
    // Get all resource limits.
    //

    LxtLogInfo("Getting all resource limits:");
    for (Index = 0; Index < 16; Index += 1)
    {
        LxtCheckErrno(getrlimit(Index, &ResourceLimit));
        LxtLogInfo("Resource# %d: current %llu, max %llu", Index, ResourceLimit.rlim_cur, ResourceLimit.rlim_max)
    }

    //
    // Set/Get invalid resources.
    //
    LxtCheckErrnoFailure(setrlimit(17, &ResourceLimit), EINVAL);
    LxtCheckErrnoFailure(getrlimit(17, &ResourceLimit), EINVAL);
    LxtCheckErrnoFailure(setrlimit(-1, &ResourceLimit), EINVAL);
    LxtCheckErrnoFailure(getrlimit(-1, &ResourceLimit), EINVAL);
    LxtCheckErrnoFailure(setrlimit(16, NULL), EINVAL);
    LxtCheckErrnoFailure(setrlimit(16, (struct rlimit*)-1), EFAULT);
    LxtCheckErrnoFailure(getrlimit(16, NULL), EINVAL);
    LxtCheckErrnoFailure(getrlimit(16, (struct rlimit*)-1), EINVAL);
    LxtCheckErrnoFailure(setrlimit(LXT_NR_OPEN, NULL), EINVAL);
    LxtCheckErrnoFailure(setrlimit(LXT_NR_OPEN, (struct rlimit*)-1), EFAULT);
    LxtCheckErrnoFailure(getrlimit(LXT_NR_OPEN, NULL), EINVAL);
    LxtCheckErrnoFailure(getrlimit(LXT_NR_OPEN, (struct rlimit*)-1), EINVAL);

    //
    // Set invalid resource limits.
    //

    ResourceLimit.rlim_cur = 2;
    ResourceLimit.rlim_max = 1;
    LxtLogInfo("Setting resource limit with soft limit being greater than hard limit");
    LxtCheckErrnoFailure(setrlimit(RLIMIT_NPROC, &ResourceLimit), EINVAL);

    //
    // Set NoFile limit past the WSL max, to NR_OPEN, and past NR_OPEN
    //

    ResourceLimit.rlim_cur = 2049;
    ResourceLimit.rlim_max = 2050;
    LxtCheckErrno(setrlimit(RLIMIT_NOFILE, &ResourceLimit));

    ResourceLimit.rlim_cur = LXT_NR_OPEN - 1;
    ResourceLimit.rlim_max = LXT_NR_OPEN;
    LxtCheckErrno(setrlimit(RLIMIT_NOFILE, &ResourceLimit));

    ResourceLimit.rlim_cur = LXT_NR_OPEN;
    ResourceLimit.rlim_max = LXT_NR_OPEN;
    LxtCheckErrno(setrlimit(RLIMIT_NOFILE, &ResourceLimit));

    ResourceLimit.rlim_cur = LXT_NR_OPEN + 1;
    ResourceLimit.rlim_max = LXT_NR_OPEN + 1;
    LxtCheckErrnoFailure(setrlimit(RLIMIT_NOFILE, &ResourceLimit), EPERM);

    //
    // Test RLIMIT_NPROC.
    //

    ResourceLimit.rlim_cur = 7823;
    ResourceLimit.rlim_max = 7824;
    LxtCheckErrno(setrlimit(RLIMIT_NPROC, &ResourceLimit));

    ResourceLimit.rlim_cur = 0x7ffffffffffffffe;
    ResourceLimit.rlim_max = 0x7fffffffffffffff;
    LxtCheckErrno(setrlimit(RLIMIT_NPROC, &ResourceLimit));
    LxtCheckErrno(getrlimit(RLIMIT_NPROC, &ResourceLimit));
    LxtCheckEqual(ResourceLimit.rlim_cur, 0x7ffffffffffffffe, "%Iu");
    LxtCheckEqual(ResourceLimit.rlim_max, 0x7fffffffffffffff, "%Iu");

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    return Result;
}

int ResourceLimitNoFile(PLXT_ARGS Args)

{

    int ChildPid;
    int FileDescriptorCount;
    int* FileDescriptors;
    int Index;
    int InitialFileDescriptorCount;
    struct rlimit InitialResourceLimit;
    struct rlimit ResourceLimit;
    int Result;
    struct stat Stat;

    ChildPid = -1;
    FileDescriptors = NULL;
    LxtCheckErrno(getrlimit(RLIMIT_NOFILE, &InitialResourceLimit));
    LxtLogInfo("Initial rlim_cur %Iu rlim_max %Iu", InitialResourceLimit.rlim_cur, InitialResourceLimit.rlim_max);

    ResourceLimit = InitialResourceLimit;

    //
    // Lower the current file descriptor limit.
    //

    ResourceLimit.rlim_cur = LXT_NOFILE;
    LxtCheckErrno(setrlimit(RLIMIT_NOFILE, &ResourceLimit));
    LxtCheckErrno(getrlimit(RLIMIT_NOFILE, &ResourceLimit));
    LxtLogInfo("rlim_cur %Iu rlim_max %Iu", ResourceLimit.rlim_cur, ResourceLimit.rlim_max);

    //
    // Determine how many file descriptors are already open.
    //

    InitialFileDescriptorCount = 0;
    for (Index = 0; Index < ResourceLimit.rlim_cur; Index += 1)
    {
        if (fstat(Index, &Stat) == 0)
        {

            //
            // Ensure that the file descriptor number matches the index. This
            // is strictly to make the validation later in this test more
            // straightforward.
            //

            LxtCheckEqual(Index, InitialFileDescriptorCount, "%d");
            InitialFileDescriptorCount += 1;
        }
    }

    //
    // Determine how many file descriptors are already open and allocate an
    // array large enough to open the maximum number of file descriptors.
    //

    LxtLogInfo("%d currently open file descriptors", InitialFileDescriptorCount);
    FileDescriptorCount = ResourceLimit.rlim_cur - InitialFileDescriptorCount;
    FileDescriptors = malloc(sizeof(int) * FileDescriptorCount);
    if (FileDescriptors == NULL)
    {
        LxtLogError("alloc failed");
        Result = LXT_RESULT_FAILURE;
        goto ErrorExit;
    }

    memset(FileDescriptors, -1, sizeof(FileDescriptors));
    LxtCheckErrno(FileDescriptors[0] = creat(LXT_RESOURCE_LIMIT_TEST_FILE, 0655));

    //
    // Ensure that a file descriptor with a value greater than the current
    // rlimit cannot be created.
    //

    LxtCheckErrnoFailure(dup2(FileDescriptors[0], ResourceLimit.rlim_cur), EBADF);
    LxtCheckErrnoFailure(dup2(FileDescriptors[0], (ResourceLimit.rlim_cur + 1)), EBADF);

    //
    // Open enough file descriptors to completely fill the table.
    //

    for (Index = 1; Index < FileDescriptorCount; Index += 1)
    {
        LxtCheckErrno(FileDescriptors[Index] = open(LXT_RESOURCE_LIMIT_TEST_FILE, O_RDONLY));
    }

    LxtLogInfo("Opened %d file descriptors", Index);

    //
    // Ensure that opening one more file descriptor fails.
    //

    LxtCheckErrnoFailure(open(LXT_RESOURCE_LIMIT_TEST_FILE, O_RDONLY), EMFILE);

    //
    // Lower the limit to the initial file descriptor count and close all but
    // the highest numbered file descriptor.
    //

    ResourceLimit.rlim_cur = InitialFileDescriptorCount;
    LxtCheckErrno(setrlimit(RLIMIT_NOFILE, &ResourceLimit));
    LxtCheckErrno(getrlimit(RLIMIT_NOFILE, &ResourceLimit));
    LxtLogInfo("rlim_cur %Iu rlim_max %Iu", ResourceLimit.rlim_cur, ResourceLimit.rlim_max);

    for (Index = 0; Index < (FileDescriptorCount - 1); Index += 1)
    {
        LxtClose(FileDescriptors[Index]);
        FileDescriptors[Index] = -1;
    }

    //
    // Try to open file descriptors up to the max value.
    //

    for (Index = 0; Index < (FileDescriptorCount - 1); Index += 1)
    {
        LxtCheckErrnoFailure(FileDescriptors[Index] = open(LXT_RESOURCE_LIMIT_TEST_FILE, O_RDONLY), EMFILE);
    }

    //
    // Ensure that a child process inherits the same file descriptor limits.
    //

    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        for (Index = 0; Index < (FileDescriptorCount - 1); Index += 1)
        {
            LxtCheckErrnoFailure(FileDescriptors[Index] = open(LXT_RESOURCE_LIMIT_TEST_FILE, O_RDONLY), EMFILE);
        }

        goto ErrorExit;
    }

    //
    // Wait for the child to exit.
    //

    LxtCheckErrno(LxtWaitPidPoll(ChildPid, LXT_RESULT_SUCCESS));

    //
    // Increment the rlimit and open a single file descriptor.
    //

    ResourceLimit.rlim_cur += 1;
    LxtCheckErrno(setrlimit(RLIMIT_NOFILE, &ResourceLimit));
    LxtCheckErrno(getrlimit(RLIMIT_NOFILE, &ResourceLimit));
    LxtLogInfo("rlim_cur %Iu rlim_max %Iu", ResourceLimit.rlim_cur, ResourceLimit.rlim_max);

    LxtCheckErrno(FileDescriptors[0] = open(LXT_RESOURCE_LIMIT_TEST_FILE, O_RDONLY));
    LxtCheckEqual(InitialFileDescriptorCount, FileDescriptors[0], "%d");

    //
    // Reset the rlimit to the original value.
    //

    ResourceLimit.rlim_cur = LXT_NOFILE;
    LxtCheckErrno(setrlimit(RLIMIT_NOFILE, &ResourceLimit));
    LxtCheckErrno(getrlimit(RLIMIT_NOFILE, &ResourceLimit));
    LxtLogInfo("rlim_cur %Iu rlim_max %Iu", ResourceLimit.rlim_cur, ResourceLimit.rlim_max);

    //
    // Attempt to reopen the rest of the file descriptors now that the limit has
    // been increased.
    //

    for (Index = 1; Index < (FileDescriptorCount - 1); Index += 1)
    {
        LxtCheckErrno(FileDescriptors[Index] = open(LXT_RESOURCE_LIMIT_TEST_FILE, O_RDONLY));
        LxtCheckEqual((Index + InitialFileDescriptorCount), FileDescriptors[Index], "%d");
    }

    //
    // Attempt to open one more file descriptor (should fail).
    //

    LxtCheckErrnoFailure(open(LXT_RESOURCE_LIMIT_TEST_FILE, O_RDONLY), EMFILE);

    //
    // Set the current resource limit to the max.
    //

    ResourceLimit.rlim_cur = ResourceLimit.rlim_max;
    LxtCheckErrno(setrlimit(RLIMIT_NOFILE, &ResourceLimit));
    LxtCheckErrno(getrlimit(RLIMIT_NOFILE, &ResourceLimit));
    LxtLogInfo("rlim_cur %Iu rlim_max %Iu", ResourceLimit.rlim_cur, ResourceLimit.rlim_max);

    //
    // Make the file descriptor limit very large.
    //

    ResourceLimit.rlim_max = 0x100000;
    LxtCheckErrno(setrlimit(RLIMIT_NOFILE, &ResourceLimit));
    LxtCheckErrno(getrlimit(RLIMIT_NOFILE, &ResourceLimit));
    LxtLogInfo("rlim_cur %Iu rlim_max %Iu", ResourceLimit.rlim_cur, ResourceLimit.rlim_max);

    ResourceLimit.rlim_cur = ResourceLimit.rlim_max;
    LxtCheckErrno(setrlimit(RLIMIT_NOFILE, &ResourceLimit));
    LxtCheckErrno(getrlimit(RLIMIT_NOFILE, &ResourceLimit));
    LxtLogInfo("rlim_cur %Iu rlim_max %Iu", ResourceLimit.rlim_cur, ResourceLimit.rlim_max);

    //
    // Attempt to set the file descriptor limit larger than the maximum allowed.
    //

    ResourceLimit.rlim_cur = ResourceLimit.rlim_max;
    ResourceLimit.rlim_max += 1;
    LxtCheckErrnoFailure(setrlimit(RLIMIT_NOFILE, &ResourceLimit), EPERM);
    LxtCheckErrno(getrlimit(RLIMIT_NOFILE, &ResourceLimit));
    LxtLogInfo("rlim_cur %Iu rlim_max %Iu", ResourceLimit.rlim_cur, ResourceLimit.rlim_max);

    //
    // Restore the original rlimit.
    //

    LxtCheckErrno(setrlimit(RLIMIT_NOFILE, &InitialResourceLimit));
    LxtCheckErrno(getrlimit(RLIMIT_NOFILE, &ResourceLimit));
    LxtLogInfo("Restored rlim_cur %Iu rlim_max %Iu", ResourceLimit.rlim_cur, ResourceLimit.rlim_max);

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    if (FileDescriptors != NULL)
    {
        for (Index = 0; Index < FileDescriptorCount; Index += 1)
        {
            if (FileDescriptors[Index] >= 0)
            {
                LxtClose(FileDescriptors[Index]);
            }
        }

        free(FileDescriptors);
    }

    if (ChildPid == 0)
    {
        _exit(Result);
    }
    else
    {
        unlink(LXT_RESOURCE_LIMIT_TEST_FILE);
    }
}

int PrlimitTest(PLXT_ARGS Args)

{

    struct __user_cap_data_struct CapData[2];
    struct __user_cap_header_struct CapHeader;
    int ChildPid;
    int CurrentPid;
    int Index;
    struct rlimit NewLimit;
    struct rlimit OldLimit;
    int ParentPid;
    int Result;
    int Status;

    ChildPid = -1;
    ParentPid = getpid();

    //
    // Get and set all resource limits.
    //

    for (Index = 0; Index < 16; Index += 1)
    {
        LxtCheckErrno(LxtPrlimit64(0, Index, NULL, &NewLimit));
        LxtCheckErrno(LxtPrlimit64(0, Index, &NewLimit, &OldLimit));
        LxtCheckEqual(OldLimit.rlim_max, NewLimit.rlim_max, "%Iu");
        LxtCheckEqual(OldLimit.rlim_cur, NewLimit.rlim_cur, "%Iu");
        LxtCheckErrno(LxtPrlimit64(ParentPid, Index, &NewLimit, &OldLimit));
    }

    //
    // Pid != 0 variations.
    //

    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        CurrentPid = getpid();
        LxtCheckErrno(LxtPrlimit64(ParentPid, RLIMIT_NOFILE, NULL, &NewLimit));
        LxtCheckErrno(LxtPrlimit64(ParentPid, RLIMIT_NOFILE, &NewLimit, &OldLimit));
        LxtCheckErrno(LxtPrlimit64(CurrentPid, RLIMIT_NOFILE, NULL, &NewLimit));
        LxtCheckErrno(LxtPrlimit64(CurrentPid, RLIMIT_NOFILE, &NewLimit, &OldLimit));
        goto ErrorExit;
    }

    //
    // Wait for the child to exit.
    //

    LxtCheckErrno(LxtWaitPidPoll(ChildPid, LXT_RESULT_SUCCESS));

    //
    // UID variations.
    //

    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        memset(&CapData, 0, sizeof(CapData));
        memset(&CapHeader, 0, sizeof(CapHeader));
        CapHeader.version = _LINUX_CAPABILITY_VERSION_3;
        CapData[CAP_TO_INDEX(CAP_SETUID)].permitted |= CAP_TO_MASK(CAP_SETUID);
        CapData[0].effective = CapData[0].permitted;
        CapData[1].effective = CapData[1].permitted;
        LxtCheckErrno(prctl(PR_SET_KEEPCAPS, 1));
        LxtCheckErrno(LxtSetresuid(-1, -1, LXT_RESOURCE_LIMIT_UID));
        LxtCheckErrno(LxtCapSet(&CapHeader, CapData));
        LxtCheckErrno(LxtPrlimit64(ParentPid, RLIMIT_NOFILE, NULL, &NewLimit));
        LxtCheckErrno(LxtPrlimit64(ParentPid, RLIMIT_NOFILE, &NewLimit, &OldLimit));

        LxtCheckErrno(LxtSetresuid(-1, LXT_RESOURCE_LIMIT_UID, -1));
        LxtCheckErrno(LxtPrlimit64(ParentPid, RLIMIT_NOFILE, NULL, &NewLimit));
        LxtCheckErrno(LxtPrlimit64(ParentPid, RLIMIT_NOFILE, &NewLimit, &OldLimit));

        LxtCheckErrno(LxtSetresuid(LXT_RESOURCE_LIMIT_UID, -1, -1));
        LxtCheckErrnoFailure(LxtPrlimit64(ParentPid, RLIMIT_NOFILE, NULL, &NewLimit), EPERM);
        LxtCheckErrnoFailure(LxtPrlimit64(ParentPid, RLIMIT_NOFILE, &NewLimit, &OldLimit), EPERM);

        CurrentPid = getpid();
        LxtCheckErrno(LxtPrlimit64(CurrentPid, RLIMIT_NOFILE, NULL, &NewLimit));
        LxtCheckErrno(LxtPrlimit64(CurrentPid, RLIMIT_NOFILE, &NewLimit, &OldLimit));
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
        // Change the UID and verify that querying the parent's resource limits
        // succeeds with the CAP_SYS_RESOURCE capability.
        //

        memset(&CapData, 0, sizeof(CapData));
        memset(&CapHeader, 0, sizeof(CapHeader));
        CapHeader.version = _LINUX_CAPABILITY_VERSION_3;
        CapData[CAP_TO_INDEX(CAP_SETUID)].permitted |= CAP_TO_MASK(CAP_SETUID);
        CapData[CAP_TO_INDEX(CAP_SYS_RESOURCE)].permitted |= CAP_TO_MASK(CAP_SYS_RESOURCE);
        CapData[0].effective = CapData[0].permitted;
        CapData[1].effective = CapData[1].permitted;
        LxtCheckErrno(prctl(PR_SET_KEEPCAPS, 1));
        LxtCheckErrno(LxtSetresuid(LXT_RESOURCE_LIMIT_UID, LXT_RESOURCE_LIMIT_UID, LXT_RESOURCE_LIMIT_UID));
        LxtCheckErrno(LxtCapSet(&CapHeader, CapData));
        LxtCheckErrno(LxtPrlimit64(ParentPid, RLIMIT_NOFILE, NULL, &NewLimit));
        LxtCheckErrno(LxtPrlimit64(ParentPid, RLIMIT_NOFILE, &NewLimit, &OldLimit));

        //
        // Drop the CAP_SYS_RESOURCE capability and verify querying the parents
        // resource limits fails.
        //

        memset(&CapData, 0, sizeof(CapData));
        memset(&CapHeader, 0, sizeof(CapHeader));
        CapHeader.version = _LINUX_CAPABILITY_VERSION_3;
        LxtCheckErrno(LxtCapSet(&CapHeader, CapData));
        LxtCheckErrnoFailure(LxtPrlimit64(ParentPid, RLIMIT_NOFILE, NULL, &NewLimit), EPERM);
        LxtCheckErrnoFailure(LxtPrlimit64(ParentPid, RLIMIT_NOFILE, &NewLimit, &OldLimit), EPERM);

        CurrentPid = getpid();
        LxtCheckErrno(LxtPrlimit64(CurrentPid, RLIMIT_NOFILE, NULL, &NewLimit));
        LxtCheckErrno(LxtPrlimit64(CurrentPid, RLIMIT_NOFILE, &NewLimit, &OldLimit));
        goto ErrorExit;
    }

    //
    // Wait for the child to exit.
    //

    LxtCheckErrno(LxtWaitPidPoll(ChildPid, LXT_RESULT_SUCCESS));

    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        goto ErrorExit;
    }

    LxtCheckErrno(LxtWaitPidPoll(ChildPid, LXT_RESULT_SUCCESS));

    //
    // GID variations.
    //

    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        memset(&CapData, 0, sizeof(CapData));
        memset(&CapHeader, 0, sizeof(CapHeader));
        CapHeader.version = _LINUX_CAPABILITY_VERSION_3;
        CapData[CAP_TO_INDEX(CAP_SETGID)].permitted |= CAP_TO_MASK(CAP_SETGID);
        CapData[0].effective = CapData[0].permitted;
        CapData[1].effective = CapData[1].permitted;
        LxtCheckErrno(prctl(PR_SET_KEEPCAPS, 1));
        LxtCheckErrno(LxtSetresgid(-1, -1, LXT_RESOURCE_LIMIT_GID));
        LxtCheckErrno(LxtCapSet(&CapHeader, CapData));
        LxtCheckErrno(LxtPrlimit64(ParentPid, RLIMIT_NOFILE, NULL, &NewLimit));
        LxtCheckErrno(LxtPrlimit64(ParentPid, RLIMIT_NOFILE, &NewLimit, &OldLimit));

        LxtCheckErrno(LxtSetresgid(-1, LXT_RESOURCE_LIMIT_GID, -1));
        LxtCheckErrno(LxtPrlimit64(ParentPid, RLIMIT_NOFILE, NULL, &NewLimit));
        LxtCheckErrno(LxtPrlimit64(ParentPid, RLIMIT_NOFILE, &NewLimit, &OldLimit));

        LxtCheckErrno(LxtSetresgid(LXT_RESOURCE_LIMIT_GID, -1, -1));
        LxtCheckErrnoFailure(LxtPrlimit64(ParentPid, RLIMIT_NOFILE, NULL, &NewLimit), EPERM);
        LxtCheckErrnoFailure(LxtPrlimit64(ParentPid, RLIMIT_NOFILE, &NewLimit, &OldLimit), EPERM);

        CurrentPid = getpid();
        LxtCheckErrno(LxtPrlimit64(CurrentPid, RLIMIT_NOFILE, NULL, &NewLimit));
        LxtCheckErrno(LxtPrlimit64(CurrentPid, RLIMIT_NOFILE, &NewLimit, &OldLimit));
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
        // Change the GID and verify that querying the parent's resource limits
        // succeeds with the CAP_SYS_RESOURCE capability.
        //

        memset(&CapData, 0, sizeof(CapData));
        memset(&CapHeader, 0, sizeof(CapHeader));
        CapHeader.version = _LINUX_CAPABILITY_VERSION_3;
        CapData[CAP_TO_INDEX(CAP_SETGID)].permitted |= CAP_TO_MASK(CAP_SETGID);
        CapData[CAP_TO_INDEX(CAP_SYS_RESOURCE)].permitted |= CAP_TO_MASK(CAP_SYS_RESOURCE);
        CapData[0].effective = CapData[0].permitted;
        CapData[1].effective = CapData[1].permitted;
        LxtCheckErrno(prctl(PR_SET_KEEPCAPS, 1));
        LxtCheckErrno(LxtSetresgid(LXT_RESOURCE_LIMIT_GID, LXT_RESOURCE_LIMIT_GID, LXT_RESOURCE_LIMIT_GID));
        LxtCheckErrno(LxtCapSet(&CapHeader, CapData));
        LxtCheckErrno(LxtPrlimit64(ParentPid, RLIMIT_NOFILE, NULL, &NewLimit));
        LxtCheckErrno(LxtPrlimit64(ParentPid, RLIMIT_NOFILE, &NewLimit, &OldLimit));

        //
        // Drop the CAP_SYS_RESOURCE capability and verify querying the parents
        // resource limits fails.
        //

        memset(&CapData, 0, sizeof(CapData));
        memset(&CapHeader, 0, sizeof(CapHeader));
        CapHeader.version = _LINUX_CAPABILITY_VERSION_3;
        LxtCheckErrno(LxtCapSet(&CapHeader, CapData));
        LxtCheckErrnoFailure(LxtPrlimit64(ParentPid, RLIMIT_NOFILE, NULL, &NewLimit), EPERM);
        LxtCheckErrnoFailure(LxtPrlimit64(ParentPid, RLIMIT_NOFILE, &NewLimit, &OldLimit), EPERM);

        CurrentPid = getpid();
        LxtCheckErrno(LxtPrlimit64(CurrentPid, RLIMIT_NOFILE, NULL, &NewLimit));
        LxtCheckErrno(LxtPrlimit64(CurrentPid, RLIMIT_NOFILE, &NewLimit, &OldLimit));
        goto ErrorExit;
    }

    //
    // Wait for the child to exit.
    //

    LxtCheckErrno(LxtWaitPidPoll(ChildPid, LXT_RESULT_SUCCESS));

    //
    // The new limit should still be set even if old limit buffer is invalid.
    //

    LxtCheckErrno(LxtPrlimit64(0, RLIMIT_NOFILE, NULL, &OldLimit));
    NewLimit = OldLimit;
    NewLimit.rlim_cur -= 1;
    LxtCheckErrnoFailure(LxtPrlimit64(0, RLIMIT_NOFILE, &NewLimit, -1), EFAULT);
    LxtCheckErrno(LxtPrlimit64(0, RLIMIT_NOFILE, NULL, &NewLimit));
    LxtCheckNotEqual(OldLimit.rlim_cur, NewLimit.rlim_cur, "%Iu");

    //
    // Verify that if the new limit is invalid, the old limit is not returned.
    //

    LxtCheckErrno(LxtPrlimit64(0, RLIMIT_NOFILE, NULL, &OldLimit));
    NewLimit = OldLimit;
    NewLimit.rlim_cur = NewLimit.rlim_max + 1;
    memset(&OldLimit, 0, sizeof(OldLimit));
    LxtCheckErrnoFailure(LxtPrlimit64(0, RLIMIT_NOFILE, &NewLimit, &OldLimit), EINVAL);
    LxtCheckEqual(OldLimit.rlim_max, 0, "%Iu");
    LxtCheckEqual(OldLimit.rlim_cur, 0, "%Iu");
    LxtCheckErrnoFailure(LxtPrlimit64(0, RLIMIT_NOFILE, -1, &OldLimit), EFAULT);
    LxtCheckEqual(OldLimit.rlim_max, 0, "%Iu");
    LxtCheckEqual(OldLimit.rlim_cur, 0, "%Iu");

    //
    // Negative variations.
    //

    LxtCheckErrno(LxtPrlimit64(0, RLIMIT_NPROC, NULL, NULL));
    LxtCheckErrnoFailure(LxtPrlimit64(0, 16, NULL, NULL), EINVAL);
    LxtCheckErrnoFailure(LxtPrlimit64(-1, 16, NULL, NULL), ESRCH);
    LxtCheckErrnoFailure(LxtPrlimit64(-1, RLIMIT_NPROC, NULL, NULL), ESRCH);
    LxtCheckErrnoFailure(LxtPrlimit64(-1, RLIMIT_NPROC, -1, NULL), EFAULT);
    LxtCheckErrnoFailure(LxtPrlimit64(-1, RLIMIT_NPROC, NULL, -1), ESRCH);
    LxtCheckErrnoFailure(LxtPrlimit64(-1, RLIMIT_NPROC, -1, -1), EFAULT);
    LxtCheckErrnoFailure(LxtPrlimit64(0, 16, NULL, &OldLimit), EINVAL);
    LxtCheckErrnoFailure(LxtPrlimit64(0, RLIMIT_NPROC, -1, NULL), EFAULT);
    LxtCheckErrnoFailure(LxtPrlimit64(0, RLIMIT_NPROC, NULL, -1), EFAULT);
    LxtCheckErrnoFailure(LxtPrlimit64(0, RLIMIT_NPROC, -1, -1), EFAULT);
    LxtCheckErrnoFailure(LxtPrlimit64(0, 16, -1, -1), EFAULT);
    LxtCheckErrnoFailure(LxtPrlimit64(-1, 16, NULL, &OldLimit), ESRCH);
    LxtCheckErrnoFailure(LxtPrlimit64(-1, RLIMIT_NPROC, -1, NULL), EFAULT);
    LxtCheckErrnoFailure(LxtPrlimit64(-1, RLIMIT_NPROC, NULL, -1), ESRCH);
    LxtCheckErrnoFailure(LxtPrlimit64(-1, RLIMIT_NPROC, -1, -1), EFAULT);
    LxtCheckErrnoFailure(LxtPrlimit64(-1, 16, -1, -1), EFAULT);

ErrorExit:
    if (ChildPid == 0)
    {
        _exit(Result);
    }

    return Result;
}
