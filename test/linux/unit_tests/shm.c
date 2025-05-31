/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    shm.c

Abstract:

    This file is a test for the system V shared memory family of system calls.

--*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <grp.h>
#include <netinet/in.h>
#include <netdb.h>
#include <time.h>
#include <linux/random.h>

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

#include "lxtcommon.h"
#include "unittests.h"

#define LXT_NAME "shm"

#define SHM_ACCESS_UID 1004
#define SHM_ACCESS_GID 1004

//
// Globals.
//

bool g_RunningOnNative = false;
bool g_VerboseShm = false;

int ShmAtAccess(PLXT_ARGS Args);

int ShmAtDtSyscall(PLXT_ARGS Args);

int ShmCtlSyscall(PLXT_ARGS Args);

int ShmGetAccess(PLXT_ARGS Args);

int ShmGetSyscall(PLXT_ARGS Args);

int ShmPidNamespace(PLXT_ARGS Args);

void ShmPrintInfo(struct shmid_ds* Stat);

void ShmPrintInfoAttach(struct shmid_ds* Stat);

static const LXT_VARIATION g_LxtVariations[] = {
    {"shmget syscall", ShmGetSyscall},
    {"shmget access", ShmGetAccess},
    {"shmctl syscall", ShmCtlSyscall},
    {"shmat / shmdt syscalls", ShmAtDtSyscall},
    {"shmat access", ShmAtAccess},
    {"shm pid namespace", ShmPidNamespace}};

int ShmTestEntry(int Argc, char* Argv[])
{

    LXT_ARGS Args;
    int Result;

    LxtCheckResult(LxtInitialize(Argc, Argv, &Args, LXT_NAME));
    LXT_SYNCHRONIZATION_POINT_INIT();
    LxtCheckResult(LxtRunVariations(&Args, g_LxtVariations, LXT_COUNT_OF(g_LxtVariations)));

ErrorExit:
    LxtUninitialize();
    return 0;
}

int ShmAtAccess(PLXT_ARGS Args)

{

    unsigned char* Address;
    struct __user_cap_data_struct CapData[2];
    struct __user_cap_header_struct CapHeader;
    int ChildPid;
    int Id;
    void* MapResult;
    int Result;

    Address = NULL;
    ChildPid = -1;
    Id = -1;

    //
    // Create a shared memory region that should be unmappable by a process
    // without the CAP_IPC_OWNER capability.
    //

    LxtCheckErrno(Id = LxtShmGet(IPC_PRIVATE, PAGE_SIZE, 0));
    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        LxtCheckErrno(prctl(PR_SET_KEEPCAPS, 1));
        LxtCheckErrno(setgid(SHM_ACCESS_GID));
        LxtCheckErrno(setuid(SHM_ACCESS_UID));
        memset(&CapData, 0, sizeof(CapData));
        memset(&CapHeader, 0, sizeof(CapHeader));
        CapHeader.version = _LINUX_CAPABILITY_VERSION_3;
        CapData[CAP_TO_INDEX(CAP_IPC_OWNER)].permitted |= CAP_TO_MASK(CAP_IPC_OWNER);
        CapData[0].effective = CapData[0].permitted;
        CapData[1].effective = CapData[1].permitted;
        LxtCheckErrno(LxtCapSet(&CapHeader, CapData));
        LxtCheckMapErrno(Address = LxtShmAt(Id, NULL, 0));
        LxtCheckErrno(LxtShmDt(Address));
        Address = NULL;

        //
        // Drop the CAP_IPC_OWNER capability and attempt to map again (should fail).
        //

        memset(&CapData, 0, sizeof(CapData));
        memset(&CapHeader, 0, sizeof(CapHeader));
        CapHeader.version = _LINUX_CAPABILITY_VERSION_3;
        LxtCheckErrno(LxtCapSet(&CapHeader, CapData));
        Address = LxtShmAt(Id, NULL, 0);
        if (Address != MAP_FAILED)
        {
            Result = LXT_RESULT_FAILURE;
            LxtLogError("Unexpectedly able to shmat");
            goto ErrorExit;
        }

        goto ErrorExit;
    }

    //
    // Wait for the child to exit.
    //

    LxtCheckErrno(LxtWaitPidPoll(ChildPid, LXT_RESULT_SUCCESS));
    LxtCheckErrno(LxtShmCtl(Id, IPC_RMID, NULL));
    Id = -1;

    //
    // Create a read only memory region and verify that it is only mappable as
    // read only by the owner.
    //

    LxtCheckErrno(Id = LxtShmGet(IPC_PRIVATE, PAGE_SIZE, 0400));
    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        LxtCheckErrno(prctl(PR_SET_KEEPCAPS, 1));
        memset(&CapData, 0, sizeof(CapData));
        memset(&CapHeader, 0, sizeof(CapHeader));
        CapHeader.version = _LINUX_CAPABILITY_VERSION_3;
        CapData[CAP_TO_INDEX(CAP_IPC_OWNER)].permitted |= CAP_TO_MASK(CAP_IPC_OWNER);
        CapData[0].effective = CapData[0].permitted;
        CapData[1].effective = CapData[1].permitted;
        LxtCheckErrno(LxtCapSet(&CapHeader, CapData));
        LxtCheckMapErrno(Address = LxtShmAt(Id, NULL, 0));
        LxtCheckErrno(LxtShmDt(Address));
        Address = NULL;

        //
        // Drop the CAP_IPC_OWNER capability and attempt to with the readonly
        // flag.
        //

        memset(&CapData, 0, sizeof(CapData));
        memset(&CapHeader, 0, sizeof(CapHeader));
        CapHeader.version = _LINUX_CAPABILITY_VERSION_3;
        LxtCheckErrno(LxtCapSet(&CapHeader, CapData));
        LxtCheckMapErrno(Address = LxtShmAt(Id, NULL, SHM_RDONLY));
        LxtCheckErrno(LxtShmDt(Address));
        Address = NULL;

        //
        // Attempt to map as read / write (should fail).
        //

        Address = LxtShmAt(Id, NULL, 0);
        if (Address != MAP_FAILED)
        {
            Result = LXT_RESULT_FAILURE;
            LxtLogError("Unexpectedly able to shmat");
            goto ErrorExit;
        }

        //
        // Attempt to map as execute (should fail).
        //

        Address = LxtShmAt(Id, NULL, SHM_EXEC);
        if (Address != MAP_FAILED)
        {
            Result = LXT_RESULT_FAILURE;
            LxtLogError("Unexpectedly able to shmat");
            goto ErrorExit;
        }

        Id = -1;
        goto ErrorExit;
    }

    //
    // Wait for the child to exit.
    //

    LxtCheckErrno(LxtWaitPidPoll(ChildPid, LXT_RESULT_SUCCESS));
    LxtCheckErrno(LxtShmCtl(Id, IPC_RMID, NULL));
    Id = -1;

    //
    // Create a group read only memory region and verify that it is only
    // mappable by members of the same group.
    //

    LxtCheckErrno(Id = LxtShmGet(IPC_PRIVATE, PAGE_SIZE, 0040));
    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        LxtCheckErrno(prctl(PR_SET_KEEPCAPS, 1));
        LxtCheckErrno(setuid(SHM_ACCESS_UID));
        memset(&CapData, 0, sizeof(CapData));
        memset(&CapHeader, 0, sizeof(CapHeader));
        CapHeader.version = _LINUX_CAPABILITY_VERSION_3;
        CapData[CAP_TO_INDEX(CAP_IPC_OWNER)].permitted |= CAP_TO_MASK(CAP_IPC_OWNER);
        CapData[0].effective = CapData[0].permitted;
        CapData[1].effective = CapData[1].permitted;
        LxtCheckErrno(LxtCapSet(&CapHeader, CapData));
        LxtCheckMapErrno(Address = LxtShmAt(Id, NULL, 0));
        LxtCheckErrno(LxtShmDt(Address));
        Address = NULL;

        //
        // Drop the CAP_IPC_OWNER capability and attempt to with the readonly
        // flag.
        //

        memset(&CapData, 0, sizeof(CapData));
        memset(&CapHeader, 0, sizeof(CapHeader));
        CapHeader.version = _LINUX_CAPABILITY_VERSION_3;
        LxtCheckErrno(LxtCapSet(&CapHeader, CapData));
        LxtCheckMapErrno(Address = LxtShmAt(Id, NULL, SHM_RDONLY));
        LxtCheckErrno(LxtShmDt(Address));
        Address = NULL;

        //
        // Attempt to map as read / write (should fail).
        //

        Address = LxtShmAt(Id, NULL, 0);
        if (Address != MAP_FAILED)
        {
            Result = LXT_RESULT_FAILURE;
            LxtLogError("Unexpectedly able to shmat");
            goto ErrorExit;
        }

        //
        // Attempt to map as execute (should fail).
        //

        Address = LxtShmAt(Id, NULL, SHM_EXEC);
        if (Address != MAP_FAILED)
        {
            Result = LXT_RESULT_FAILURE;
            LxtLogError("Unexpectedly able to shmat");
            goto ErrorExit;
        }

        goto ErrorExit;
    }

    //
    // Wait for the child to exit.
    //

    LxtCheckErrno(LxtWaitPidPoll(ChildPid, LXT_RESULT_SUCCESS));
    LxtCheckErrno(LxtShmCtl(Id, IPC_RMID, NULL));
    Id = -1;

    //
    // Create another read only memory region and verify that it is
    // mappable.
    //

    LxtCheckErrno(Id = LxtShmGet(IPC_PRIVATE, PAGE_SIZE, 0004));
    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        LxtCheckErrno(prctl(PR_SET_KEEPCAPS, 1));
        LxtCheckErrno(setgid(SHM_ACCESS_GID));
        LxtCheckErrno(setuid(SHM_ACCESS_UID));
        memset(&CapData, 0, sizeof(CapData));
        memset(&CapHeader, 0, sizeof(CapHeader));
        CapHeader.version = _LINUX_CAPABILITY_VERSION_3;
        CapData[CAP_TO_INDEX(CAP_IPC_OWNER)].permitted |= CAP_TO_MASK(CAP_IPC_OWNER);
        CapData[CAP_TO_INDEX(CAP_SETGID)].permitted |= CAP_TO_MASK(CAP_SETGID);
        CapData[0].effective = CapData[0].permitted;
        CapData[1].effective = CapData[1].permitted;
        LxtCheckErrno(LxtCapSet(&CapHeader, CapData));
        LxtCheckMapErrno(Address = LxtShmAt(Id, NULL, 0));
        LxtCheckErrno(LxtShmDt(Address));
        Address = NULL;

        //
        // Remove all group membership, drop the CAP_IPC_OWNER capability, and
        // attempt to with the readonly flag.
        //

        LxtCheckErrno(Result = setgroups(0, NULL));
        memset(&CapData, 0, sizeof(CapData));
        memset(&CapHeader, 0, sizeof(CapHeader));
        CapHeader.version = _LINUX_CAPABILITY_VERSION_3;
        LxtCheckErrno(LxtCapSet(&CapHeader, CapData));
        LxtCheckMapErrno(Address = LxtShmAt(Id, NULL, SHM_RDONLY));
        LxtCheckErrno(LxtShmDt(Address));
        Address = NULL;

        //
        // Attempt to map as read / write (should fail).
        //

        Address = LxtShmAt(Id, NULL, 0);
        if (Address != MAP_FAILED)
        {
            Result = LXT_RESULT_FAILURE;
            LxtLogError("Unexpectedly able to shmat");
            goto ErrorExit;
        }

        //
        // Attempt to map as execute (should fail).
        //

        Address = LxtShmAt(Id, NULL, SHM_EXEC);
        if (Address != MAP_FAILED)
        {
            Result = LXT_RESULT_FAILURE;
            LxtLogError("Unexpectedly able to shmat");
            goto ErrorExit;
        }

        goto ErrorExit;
    }

    //
    // Wait for the child to exit.
    //

    LxtCheckErrno(LxtWaitPidPoll(ChildPid, LXT_RESULT_SUCCESS));
    LxtCheckErrno(LxtShmCtl(Id, IPC_RMID, NULL));
    Id = -1;

    //
    // Create a shared memory region that is write only This should be
    // unmappable by processes without the CAP_IPC_OWNER capability.
    //

    LxtCheckErrno(Id = LxtShmGet(IPC_PRIVATE, PAGE_SIZE, 0222));
    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        LxtCheckErrno(prctl(PR_SET_KEEPCAPS, 1));
        memset(&CapData, 0, sizeof(CapData));
        memset(&CapHeader, 0, sizeof(CapHeader));
        CapHeader.version = _LINUX_CAPABILITY_VERSION_3;
        CapData[CAP_TO_INDEX(CAP_IPC_OWNER)].permitted |= CAP_TO_MASK(CAP_IPC_OWNER);
        CapData[0].effective = CapData[0].permitted;
        CapData[1].effective = CapData[1].permitted;
        LxtCheckErrno(LxtCapSet(&CapHeader, CapData));
        LxtCheckMapErrno(Address = LxtShmAt(Id, NULL, 0));
        LxtCheckErrno(LxtShmDt(Address));
        Address = NULL;

        //
        // Drop the CAP_IPC_OWNER capability and attempt to map again (should fail).
        //

        memset(&CapData, 0, sizeof(CapData));
        memset(&CapHeader, 0, sizeof(CapHeader));
        CapHeader.version = _LINUX_CAPABILITY_VERSION_3;
        LxtCheckErrno(LxtCapSet(&CapHeader, CapData));
        Address = LxtShmAt(Id, NULL, SHM_RDONLY);
        if (Address != MAP_FAILED)
        {
            Result = LXT_RESULT_FAILURE;
            LxtLogError("Unexpectedly able to shmat");
            goto ErrorExit;
        }

        Address = LxtShmAt(Id, NULL, 0);
        if (Address != MAP_FAILED)
        {
            Result = LXT_RESULT_FAILURE;
            LxtLogError("Unexpectedly able to shmat");
            goto ErrorExit;
        }

        //
        // Attempt to map as execute (should fail).
        //

        Address = LxtShmAt(Id, NULL, SHM_EXEC);
        if (Address != MAP_FAILED)
        {
            Result = LXT_RESULT_FAILURE;
            LxtLogError("Unexpectedly able to shmat");
            goto ErrorExit;
        }

        goto ErrorExit;
    }

    //
    // Wait for the child to exit.
    //

    LxtCheckErrno(LxtWaitPidPoll(ChildPid, LXT_RESULT_SUCCESS));
    LxtCheckErrno(LxtShmCtl(Id, IPC_RMID, NULL));
    Id = -1;

    //
    // Create a shared memory region that can only be read or written by the
    // owner.
    //

    LxtCheckErrno(Id = LxtShmGet(IPC_PRIVATE, PAGE_SIZE, 0700));
    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {

        //
        // Drop the CAP_IPC_OWNER capability and attempt to map (should fail).
        //

        LxtCheckErrno(prctl(PR_SET_KEEPCAPS, 1));
        LxtCheckErrno(setgid(SHM_ACCESS_GID));
        memset(&CapData, 0, sizeof(CapData));
        memset(&CapHeader, 0, sizeof(CapHeader));
        CapData[CAP_TO_INDEX(CAP_SETUID)].permitted |= CAP_TO_MASK(CAP_SETUID);
        CapHeader.version = _LINUX_CAPABILITY_VERSION_3;
        CapData[0].effective = CapData[0].permitted;
        CapData[1].effective = CapData[1].permitted;
        LxtCheckErrno(LxtCapSet(&CapHeader, CapData));
        LxtCheckMapErrno(Address = LxtShmAt(Id, NULL, 0));
        LxtCheckErrno(LxtShmDt(Address));
        Address = NULL;

        //
        // Change the UID and verify the mapping fails.
        //

        LxtCheckErrno(setuid(SHM_ACCESS_UID));
        Address = LxtShmAt(Id, NULL, SHM_RDONLY);
        if (Address != MAP_FAILED)
        {
            Result = LXT_RESULT_FAILURE;
            LxtLogError("Unexpectedly able to shmat");
            goto ErrorExit;
        }

        Address = LxtShmAt(Id, NULL, 0);
        if (Address != MAP_FAILED)
        {
            Result = LXT_RESULT_FAILURE;
            LxtLogError("Unexpectedly able to shmat");
            goto ErrorExit;
        }

        goto ErrorExit;
    }

    //
    // Wait for the child to exit.
    //

    LxtCheckErrno(LxtWaitPidPoll(ChildPid, LXT_RESULT_SUCCESS));
    LxtCheckErrno(LxtShmCtl(Id, IPC_RMID, NULL));
    Id = -1;

    //
    // Create a shared memory region that is only mappable by other.
    //

    LxtCheckErrno(Id = LxtShmGet(IPC_PRIVATE, PAGE_SIZE, 0007));
    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {

        //
        // Verify the region is mappable with CAP_IPC_OWNER.
        //

        LxtCheckErrno(prctl(PR_SET_KEEPCAPS, 1));
        memset(&CapData, 0, sizeof(CapData));
        memset(&CapHeader, 0, sizeof(CapHeader));
        CapHeader.version = _LINUX_CAPABILITY_VERSION_3;
        CapData[CAP_TO_INDEX(CAP_SETGID)].permitted |= CAP_TO_MASK(CAP_SETGID);
        CapData[CAP_TO_INDEX(CAP_SETUID)].permitted |= CAP_TO_MASK(CAP_SETUID);
        CapData[CAP_TO_INDEX(CAP_IPC_OWNER)].permitted |= CAP_TO_MASK(CAP_IPC_OWNER);
        CapData[0].effective = CapData[0].permitted;
        CapData[1].effective = CapData[1].permitted;
        LxtCheckErrno(LxtCapSet(&CapHeader, CapData));
        LxtCheckMapErrno(Address = LxtShmAt(Id, NULL, 0));
        LxtCheckErrno(LxtShmDt(Address));
        Address = NULL;

        //
        // Drop the CAP_IPC_OWNER capability and attempt to map again this
        // should fail because the caller is still has a matching UID.
        //

        memset(&CapData, 0, sizeof(CapData));
        memset(&CapHeader, 0, sizeof(CapHeader));
        CapHeader.version = _LINUX_CAPABILITY_VERSION_3;
        CapData[CAP_TO_INDEX(CAP_SETGID)].permitted |= CAP_TO_MASK(CAP_SETGID);
        CapData[CAP_TO_INDEX(CAP_SETUID)].permitted |= CAP_TO_MASK(CAP_SETUID);
        CapData[0].effective = CapData[0].permitted;
        CapData[1].effective = CapData[1].permitted;
        LxtCheckErrno(LxtCapSet(&CapHeader, CapData));
        Address = LxtShmAt(Id, NULL, SHM_RDONLY);
        if (Address != MAP_FAILED)
        {
            Result = LXT_RESULT_FAILURE;
            LxtLogError("Unexpectedly able to shmat");
            goto ErrorExit;
        }

        //
        // Change the UID and attempt to map, this should still fail because
        // the caller has group ownership.
        //

        LxtCheckErrno(setuid(SHM_ACCESS_UID));
        LxtCheckErrno(LxtCapSet(&CapHeader, CapData));
        Address = LxtShmAt(Id, NULL, SHM_RDONLY);
        if (Address != MAP_FAILED)
        {
            Result = LXT_RESULT_FAILURE;
            LxtLogError("Unexpectedly able to shmat");
            goto ErrorExit;
        }

        //
        // Change the caller GID and attempt to map, this should still fail
        // because the caller has a supplementary group membership.
        //

        LxtCheckErrno(setgid(SHM_ACCESS_GID));
        Address = LxtShmAt(Id, NULL, SHM_RDONLY);
        if (Address != MAP_FAILED)
        {
            Result = LXT_RESULT_FAILURE;
            LxtLogError("Unexpectedly able to shmat");
            goto ErrorExit;
        }

        //
        // Drop supplementary group membership, finally this should succeed.
        //

        LxtCheckErrno(Result = setgroups(0, NULL));
        LxtCheckMapErrno(Address = LxtShmAt(Id, NULL, 0));
        LxtCheckErrno(LxtShmDt(Address));
        Address = NULL;
        LxtCheckMapErrno(Address = LxtShmAt(Id, NULL, SHM_RDONLY));
        LxtCheckErrno(LxtShmDt(Address));
        Address = NULL;
        LxtCheckMapErrno(Address = LxtShmAt(Id, NULL, SHM_EXEC));
        LxtCheckErrno(LxtShmDt(Address));
        Address = NULL;
        LxtCheckMapErrno(Address = LxtShmAt(Id, NULL, SHM_RDONLY | SHM_EXEC));
        LxtCheckErrno(LxtShmDt(Address));
        Address = NULL;
        goto ErrorExit;
    }

    //
    // Create a shared memory region that is only mappable as read / execute.
    //

    LxtCheckErrno(Id = LxtShmGet(IPC_PRIVATE, PAGE_SIZE, 0555));
    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {

        //
        // Drop the CAP_IPC_OWNER capability and try to map read / write and
        // read / write / execute (should fail).
        //

        memset(&CapData, 0, sizeof(CapData));
        memset(&CapHeader, 0, sizeof(CapHeader));
        CapHeader.version = _LINUX_CAPABILITY_VERSION_3;
        CapData[0].effective = CapData[0].permitted;
        CapData[1].effective = CapData[1].permitted;
        LxtCheckErrno(LxtCapSet(&CapHeader, CapData));
        Address = LxtShmAt(Id, NULL, 0);
        if (Address != MAP_FAILED)
        {
            Result = LXT_RESULT_FAILURE;
            LxtLogError("Unexpectedly able to shmat");
            goto ErrorExit;
        }

        Address = LxtShmAt(Id, NULL, SHM_EXEC);
        if (Address != MAP_FAILED)
        {
            Result = LXT_RESULT_FAILURE;
            LxtLogError("Unexpectedly able to shmat");
            goto ErrorExit;
        }

        //
        // Map the region as readonly, read / execute.
        //

        LxtCheckMapErrno(Address = LxtShmAt(Id, NULL, SHM_RDONLY));
        LxtCheckErrno(LxtShmDt(Address));
        Address = NULL;
        LxtCheckMapErrno(Address = LxtShmAt(Id, NULL, SHM_RDONLY | SHM_EXEC));
        LxtCheckErrno(LxtShmDt(Address));
        Address = NULL;
        goto ErrorExit;
    }

    //
    // Wait for the child to exit.
    //

    LxtCheckErrno(LxtWaitPidPoll(ChildPid, LXT_RESULT_SUCCESS));
    LxtCheckErrno(LxtShmCtl(Id, IPC_RMID, NULL));
    Id = -1;

ErrorExit:
    if (Address != NULL)
    {
        LxtShmDt(Address);
    }

    if (ChildPid == 0)
    {
        _exit(Result);
    }

    //
    // N.B. The identifier should not be removed by any child processes.
    //

    if (Id != -1)
    {
        LxtShmCtl(Id, IPC_RMID, NULL);
    }

    return Result;
}

int ShmAtDtSyscall(PLXT_ARGS Args)

{

    unsigned char* Address;
    unsigned char* Address2;
    int ChildPid;
    int Id;
    key_t Key;
    void* MapResult;
    struct shmid_ds ParentStat;
    unsigned char* RemappedMemory;
    size_t Result;
    struct shmid_ds Stat;

    Address = NULL;
    Address2 = NULL;
    ChildPid = -1;
    Id = -1;

    //
    // (1) Create a shared memory region.
    //

    LxtCheckErrno(Id = LxtShmGet(IPC_PRIVATE, PAGE_SIZE * 3, 0));
    LxtLogInfo("Id = %d", Id);
    LxtCheckErrno(LxtShmCtl(Id, IPC_STAT, &ParentStat));
    LxtCheckEqual(PAGE_SIZE * 3, ParentStat.shm_segsz, "%Iu");
    LxtCheckEqual(0, ParentStat.shm_atime, "%Iu");
    LxtCheckEqual(0, ParentStat.shm_dtime, "%Iu");
    LxtCheckNotEqual(0, ParentStat.shm_ctime, "%Iu");
    LxtCheckEqual(ParentStat.shm_nattch, 0, "%Iu");

    //
    // Map the shared memory region.
    //

    LxtCheckMapErrno(Address = LxtShmAt(Id, NULL, 0));
    LxtLogInfo("Address = %p", Address);
    LxtCheckErrno(LxtShmCtl(Id, IPC_STAT, &ParentStat));
    ShmPrintInfoAttach(&ParentStat);
    LxtCheckNotEqual(0, ParentStat.shm_atime, "%Iu");
    LxtCheckEqual(0, ParentStat.shm_dtime, "%Iu");
    LxtCheckEqual(ParentStat.shm_nattch, 1, "%Iu");
    LxtCheckEqual(getpid(), ParentStat.shm_lpid, "%Iu");

    //
    // Sleep for 2 seconds then fork and verify that attach statics are
    // updated correctly. The attach count and attach time should be updated
    // but the last attach pid should not change.
    //

    sleep(2);
    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        LxtCheckErrno(LxtShmCtl(Id, IPC_STAT, &Stat));
        LxtCheckEqual(Stat.shm_nattch, 2, "%Iu");
        LxtCheckEqual(ParentStat.shm_lpid, Stat.shm_lpid, "%Iu");
        LxtCheckNotEqual(ParentStat.shm_atime, Stat.shm_atime, "%Iu");
        goto ErrorExit;
    }

    //
    // Wait for the child to exit.
    //

    LxtCheckErrno(LxtWaitPidPoll(ChildPid, LXT_RESULT_SUCCESS));
    LxtCheckErrno(LxtShmCtl(Id, IPC_STAT, &Stat));
    LxtCheckEqual(Stat.shm_nattch, 1, "%Iu");
    LxtCheckNotEqual(0, Stat.shm_dtime, "%Iu");

    //
    // Attempt to map the region in an area that already is mapped.
    //

    Address2 = LxtShmAt(Id, Address, 0);
    if (Address2 != MAP_FAILED)
    {
        Result = LXT_RESULT_FAILURE;
        LxtLogError("shmat on a used region should fail without SHM_REMAP flag %p %d", Address2, errno);

        goto ErrorExit;
    }

    Address2 = LxtShmAt(Id, Address + PAGE_SIZE, 0);
    if (Address2 != MAP_FAILED)
    {
        Result = LXT_RESULT_FAILURE;
        LxtLogError("shmat on a used region should fail without SHM_REMAP flag %p %d", Address2, errno);

        goto ErrorExit;
    }

    Address2 = LxtShmAt(Id, Address + (PAGE_SIZE * 2), 0);
    if (Address2 != MAP_FAILED)
    {
        Result = LXT_RESULT_FAILURE;
        LxtLogError("shmat on a used region should fail without SHM_REMAP flag %p %d", Address2, errno);

        goto ErrorExit;
    }

    if (g_RunningOnNative == false)
    {
        LxtLogInfo("WARNING: these variations are expected to fail on native Ubuntu");
        Address2 = LxtShmAt(Id, Address, SHM_REMAP);
        if (Address2 != MAP_FAILED)
        {
            Result = LXT_RESULT_FAILURE;
            LxtLogError("shmat with SHM_REMAP replacing entire region");
            goto ErrorExit;
        }

        Address2 = LxtShmAt(Id, Address + PAGE_SIZE, SHM_REMAP);
        if (Address2 != MAP_FAILED)
        {
            Result = LXT_RESULT_FAILURE;
            LxtLogError("shmat with SHM_REMAP replacing last two pages.");
            goto ErrorExit;
        }

        //
        // Unmap the first page in the range.
        //

        LxtCheckErrnoFailure(munmap(Address, PAGE_SIZE), EINVAL);

        //
        // Unmap the middle page of the three-page range.
        //

        LxtCheckErrnoFailure(munmap(Address + PAGE_SIZE, PAGE_SIZE), EINVAL);

        //
        // Unmap the last page in the range.
        //

        LxtCheckErrnoFailure(munmap(Address + (2 * PAGE_SIZE), PAGE_SIZE), EINVAL);

        //
        // Use the remap system call to resize the region.
        //

        RemappedMemory = LxtMremap(Address, PAGE_SIZE * 3, PAGE_SIZE * 4, MREMAP_MAYMOVE, NULL);

        if (RemappedMemory != MAP_FAILED)
        {
            Result = LXT_RESULT_FAILURE;
            LxtLogError("mremap moving the region.");
            goto ErrorExit;
        }

        goto ErrorExit;
    }

    //
    // Use the SHM_REMAP flag to replace the entire region.
    //

    LxtCheckMapErrno(Address2 = LxtShmAt(Id, Address, SHM_REMAP));
    LxtCheckEqual(Address, Address2, "%p");
    LxtCheckErrno(LxtShmCtl(Id, IPC_STAT, &Stat));
    LxtCheckEqual(Stat.shm_nattch, 1, "%Iu");
    LxtCheckErrno(LxtShmDt(Address));
    LxtCheckErrno(LxtShmCtl(Id, IPC_STAT, &Stat));
    LxtCheckEqual(Stat.shm_nattch, 0, "%Iu");
    LxtCheckErrnoFailure(LxtShmDt(Address), EINVAL);

    //
    // Use the SHM_REMAP flag to replace the last two pages of the original
    // region.
    //

    LxtCheckMapErrno(Address = LxtShmAt(Id, Address, 0));
    LxtCheckMapErrno(Address2 = LxtShmAt(Id, Address + PAGE_SIZE, SHM_REMAP));
    LxtCheckEqual(Address + PAGE_SIZE, Address2, "%p");
    LxtCheckErrno(LxtShmCtl(Id, IPC_STAT, &Stat));
    LxtCheckEqual(Stat.shm_nattch, 2, "%Iu");
    LxtCheckErrno(LxtShmDt(Address));
    LxtCheckErrno(LxtShmCtl(Id, IPC_STAT, &Stat));
    LxtCheckEqual(Stat.shm_nattch, 1, "%Iu");
    LxtCheckErrno(LxtShmDt(Address2));
    LxtCheckErrno(LxtShmCtl(Id, IPC_STAT, &Stat));
    LxtCheckEqual(Stat.shm_nattch, 0, "%Iu");
    LxtCheckErrnoFailure(LxtShmDt(Address), EINVAL);
    LxtCheckErrnoFailure(LxtShmDt(Address2), EINVAL);
    Address = NULL;
    Address2 = NULL;

    //
    // Unmap the middle page of the three-page range to split the region,
    // this should increment the attach count.
    //

    LxtCheckMapErrno(Address = LxtShmAt(Id, NULL, 0));
    LxtCheckErrno(munmap(Address + PAGE_SIZE, PAGE_SIZE));
    LxtCheckErrno(LxtShmCtl(Id, IPC_STAT, &Stat));
    ShmPrintInfoAttach(&Stat);
    LxtCheckEqual(Stat.shm_nattch, 2, "%Iu");
    LxtCheckNotEqual(0, Stat.shm_dtime, "%Iu");

    //
    // Unmap the last page in the range.
    //

    LxtCheckErrno(munmap(Address + (2 * PAGE_SIZE), PAGE_SIZE));
    LxtCheckErrno(LxtShmCtl(Id, IPC_STAT, &Stat));
    ShmPrintInfoAttach(&Stat);
    LxtCheckEqual(Stat.shm_nattch, 1, "%Iu");

    //
    // Use detach to clear the range.
    //

    LxtCheckErrno(LxtShmDt(Address));
    Address = NULL;
    LxtCheckErrno(LxtShmCtl(Id, IPC_STAT, &Stat));
    ShmPrintInfoAttach(&Stat);
    LxtCheckEqual(Stat.shm_nattch, 0, "%Iu");

    //
    // (2) Map the region again. Unmap the middle page of the three-page range.
    //

    LxtCheckMapErrno(Address = LxtShmAt(Id, NULL, 0));
    LxtLogInfo("Address = %p", Address);
    LxtCheckErrno(LxtShmCtl(Id, IPC_STAT, &Stat));
    ShmPrintInfoAttach(&Stat);
    LxtCheckEqual(Stat.shm_nattch, 1, "%Iu");
    LxtCheckErrno(munmap(Address + PAGE_SIZE, PAGE_SIZE));
    LxtCheckErrno(LxtShmCtl(Id, IPC_STAT, &Stat));
    ShmPrintInfoAttach(&Stat);
    LxtCheckEqual(Stat.shm_nattch, 2, "%Iu");

    //
    // Use shmdt to remove both remaining mapped regions, this should unmap
    // both attached regions.
    //

    LxtCheckErrno(LxtShmDt(Address));
    Address = NULL;
    LxtCheckErrno(LxtShmCtl(Id, IPC_STAT, &Stat));
    ShmPrintInfoAttach(&Stat);
    LxtCheckEqual(Stat.shm_nattch, 0, "%Iu");

    //
    // (3) Use the remap system call to resize the region.
    //

    LxtCheckMapErrno(Address = LxtShmAt(Id, NULL, 0));
    LxtLogInfo("Address = %p", Address);
    LxtCheckErrno(LxtShmCtl(Id, IPC_STAT, &Stat));
    ShmPrintInfoAttach(&Stat);
    LxtCheckEqual(Stat.shm_nattch, 1, "%Iu");
    LxtCheckMapErrno(RemappedMemory = LxtMremap(Address, PAGE_SIZE * 3, PAGE_SIZE * 4, MREMAP_MAYMOVE, NULL));

    LxtLogInfo("RemappedMemory = %p", RemappedMemory);

    //
    // If the address changed, attempt to remap the old address.
    //

    if (Address != RemappedMemory)
    {
        LxtCheckErrnoFailure(LxtShmDt(Address), EINVAL);
        Address = RemappedMemory;
    }

    LxtCheckErrno(LxtShmCtl(Id, IPC_STAT, &Stat));
    ShmPrintInfoAttach(&Stat);
    LxtCheckEqual(Stat.shm_nattch, 1, "%Iu");

    //
    // Unmap the middle two pages in the range.
    //

    LxtCheckErrno(munmap(Address + PAGE_SIZE, (2 * PAGE_SIZE)));
    LxtCheckErrno(LxtShmCtl(Id, IPC_STAT, &Stat));
    ShmPrintInfoAttach(&Stat);
    LxtCheckEqual(Stat.shm_nattch, 2, "%Iu");

    //
    // Unmap the first page in the range.
    //

    LxtCheckErrno(munmap(Address, PAGE_SIZE));
    LxtCheckErrno(LxtShmCtl(Id, IPC_STAT, &Stat));
    ShmPrintInfoAttach(&Stat);
    LxtCheckEqual(Stat.shm_nattch, 1, "%Iu");

    //
    // Use shmdt to remove the remaining region (the last page in the range).
    //

    LxtCheckErrno(LxtShmDt(Address));
    Address = NULL;
    LxtCheckErrno(LxtShmCtl(Id, IPC_STAT, &Stat));
    ShmPrintInfoAttach(&Stat);
    LxtCheckEqual(Stat.shm_nattch, 0, "%Iu");

    //
    // (4) Map the region again. Use the mremap system call to shrink the
    // region and validate that the global shared memory region remains the
    // same size.
    //

    LxtCheckMapErrno(Address = LxtShmAt(Id, NULL, 0));
    LxtLogInfo("Address = %p", Address);
    LxtCheckErrno(LxtShmCtl(Id, IPC_STAT, &Stat));
    ShmPrintInfoAttach(&Stat);
    LxtCheckEqual(Stat.shm_nattch, 1, "%Iu");
    LxtCheckEqual(Stat.shm_segsz, (PAGE_SIZE * 3), "%Iu");
    LxtCheckMapErrno(RemappedMemory = LxtMremap(Address, PAGE_SIZE * 3, PAGE_SIZE, 0, NULL));

    LxtLogInfo("RemappedMemory = %p", RemappedMemory);
    LxtCheckEqual(Address, RemappedMemory, "%p");
    LxtCheckErrno(LxtShmCtl(Id, IPC_STAT, &Stat));
    ShmPrintInfoAttach(&Stat);
    LxtCheckEqual(Stat.shm_nattch, 1, "%Iu");
    LxtCheckEqual(Stat.shm_segsz, PAGE_SIZE * 3, "%Iu");

    //
    // Use shmdt to remove the region.
    //

    LxtCheckErrno(LxtShmDt(Address));
    Address = NULL;
    LxtCheckErrno(LxtShmCtl(Id, IPC_STAT, &Stat));
    ShmPrintInfoAttach(&Stat);
    LxtCheckEqual(Stat.shm_nattch, 0, "%Iu");

    //
    // (5) Map the region twice.
    //

    LxtCheckMapErrno(Address = LxtShmAt(Id, NULL, 0));
    LxtLogInfo("Address = %p", Address);
    LxtCheckErrno(LxtShmCtl(Id, IPC_STAT, &Stat));
    ShmPrintInfoAttach(&Stat);
    LxtCheckEqual(Stat.shm_nattch, 1, "%Iu");

    LxtCheckMapErrno(Address2 = LxtShmAt(Id, NULL, 0));
    LxtLogInfo("Address2 = %p", Address2);
    LxtCheckErrno(LxtShmCtl(Id, IPC_STAT, &Stat));
    ShmPrintInfoAttach(&Stat);
    LxtCheckEqual(Stat.shm_nattch, 2, "%Iu");

    //
    // Ensure the shared memory region were mapped to different locations and
    // detach both.
    //

    LxtCheckNotEqual(Address, Address2, "%p");
    LxtCheckErrno(LxtShmDt(Address2));
    Address2 = NULL;
    LxtCheckErrno(LxtShmCtl(Id, IPC_STAT, &Stat));
    ShmPrintInfoAttach(&Stat);
    LxtCheckEqual(Stat.shm_nattch, 1, "%Iu");
    LxtCheckErrno(LxtShmDt(Address));
    Address = NULL;
    LxtCheckErrno(LxtShmCtl(Id, IPC_STAT, &Stat));
    ShmPrintInfoAttach(&Stat);
    LxtCheckEqual(Stat.shm_nattch, 0, "%Iu");

    //
    // (6) Map the region, delete the region, and validate that the region is
    // still able to be mapped.
    //

    LxtCheckMapErrno(Address = LxtShmAt(Id, NULL, 0));
    LxtCheckErrno(LxtShmCtl(Id, IPC_RMID, NULL));

    //
    // Delete the region again (should succeed).
    //

    LxtCheckErrno(LxtShmCtl(Id, IPC_RMID, NULL));
    LxtCheckErrno(LxtShmCtl(Id, IPC_STAT, &Stat));
    ShmPrintInfoAttach(&Stat);
    LxtCheckEqual(Stat.shm_nattch, 1, "%Iu");
    Address[0] = 'a';
    LxtCheckMapErrno(Address2 = LxtShmAt(Id, NULL, 0));
    LxtCheckErrno(LxtShmCtl(Id, IPC_STAT, &Stat));
    Address2[0] = 'a';
    ShmPrintInfoAttach(&Stat);
    LxtCheckEqual(Stat.shm_nattch, 2, "%Iu");

    //
    // Detach both mapped regions.
    //

    LxtCheckErrno(LxtShmDt(Address));
    Address = NULL;
    LxtCheckErrno(LxtShmCtl(Id, IPC_STAT, &Stat));
    ShmPrintInfoAttach(&Stat);
    LxtCheckEqual(Stat.shm_nattch, 1, "%Iu");
    LxtCheckErrno(LxtShmDt(Address2));
    Address2 = NULL;

    //
    // The region should be deleted at this point to the shmctl should fail.
    //

    LxtCheckErrnoFailure(LxtShmCtl(Id, IPC_STAT, &Stat), EINVAL);
    Id = -1;

    //
    // (7) Delete the shared memory region and attempt to attach it afterwards.
    //

    LxtCheckErrno(Id = LxtShmGet(IPC_PRIVATE, PAGE_SIZE * 3, 0));
    LxtLogInfo("Id = %d", Id);
    LxtCheckErrno(LxtShmCtl(Id, IPC_STAT, &Stat));
    LxtCheckEqual(Stat.shm_nattch, 0, "%Iu");

    LxtCheckErrno(LxtShmCtl(Id, IPC_RMID, NULL));
    Address = LxtShmAt(Id, NULL, 0);
    if ((Address != MAP_FAILED) && (errno != EINVAL))
    {
        Result = LXT_RESULT_FAILURE;
        LxtLogError("unexpectedly able to attach deleted memory region %p, %d", Address, errno);

        goto ErrorExit;
    }

    //
    // Attempt to stat the deleted region.
    //

    LxtCheckErrnoFailure(LxtShmCtl(Id, IPC_STAT, &Stat), EINVAL);
    Id = -1;

    //
    // (8) Use mremap to move the last page to a new location.
    //

    LxtCheckErrno(Id = LxtShmGet(IPC_PRIVATE, PAGE_SIZE * 3, 0));
    LxtLogInfo("Id = %d", Id);
    LxtCheckMapErrno(Address = LxtShmAt(Id, NULL, 0));
    LxtLogInfo("Address = %p", Address);
    LxtCheckMapErrno(Address2 = LxtMremap(Address + (2 * PAGE_SIZE), PAGE_SIZE, PAGE_SIZE * 4, MREMAP_MAYMOVE, NULL));

    LxtLogInfo("Address2 = %p", Address2);
    LxtCheckNotEqual(Address + (2 * PAGE_SIZE), Address2, "%p");
    LxtCheckErrno(LxtShmCtl(Id, IPC_STAT, &Stat));
    LxtCheckEqual(Stat.shm_nattch, 2, "%Iu");

    //
    // Detach the original address and validate the second region remains.
    //

    LxtCheckErrno(LxtShmDt(Address));
    Address = NULL;
    LxtCheckErrno(LxtShmCtl(Id, IPC_STAT, &Stat));
    LxtCheckEqual(Stat.shm_nattch, 1, "%Iu");

    //
    // Ensure that shmdt does not work for the new address.
    //

    LxtCheckErrnoFailure(LxtShmDt(Address2), EINVAL);
    LxtCheckErrno(LxtShmCtl(Id, IPC_STAT, &Stat));
    LxtCheckEqual(Stat.shm_nattch, 1, "%Iu");

    //
    // Call shmdt on what would have been the start of the new region.
    //
    // N.B. This functions like a new mapping of the memory where the first two
    //      pages have been unmapped.
    //

    LxtCheckErrno(LxtShmDt(Address2 - (2 * PAGE_SIZE)));
    Address2 = NULL;
    LxtCheckErrno(LxtShmCtl(Id, IPC_STAT, &Stat));
    LxtCheckEqual(Stat.shm_nattch, 0, "%Iu");

ErrorExit:
    if (Address != NULL)
    {
        LxtShmDt(Address);
    }

    if (Address2 != NULL)
    {
        LxtShmDt(Address2);
    }

    if (ChildPid == 0)
    {
        _exit(Result);
    }

    if (Id != -1)
    {
        LxtShmCtl(Id, IPC_RMID, NULL);
    }

    return Result;
}

int ShmGetAccess(PLXT_ARGS Args)

{

    struct __user_cap_data_struct CapData[2];
    struct __user_cap_header_struct CapHeader;
    int ChildPid;
    int Id;
    key_t Key;
    int Mode;
    int Result;

    ChildPid = -1;
    Id = -1;

    LxtCheckErrno(LxtGetrandom(&Key, sizeof(Key), 0));
    LxtLogInfo("Key = %u", Key);

    //
    // Create a shared memory region with a mode of all zeros.
    //

    Mode = 0000;
    LxtCheckErrno(Id = LxtShmGet(Key, PAGE_SIZE, (IPC_CREAT | IPC_EXCL | Mode)));
    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {

        //
        // First attempt with the CAP_IPC_OWNER capability.
        //

        LxtCheckErrno(prctl(PR_SET_KEEPCAPS, 1));
        memset(&CapData, 0, sizeof(CapData));
        memset(&CapHeader, 0, sizeof(CapHeader));
        CapHeader.version = _LINUX_CAPABILITY_VERSION_3;
        CapData[CAP_TO_INDEX(CAP_SETGID)].permitted |= CAP_TO_MASK(CAP_SETGID);
        CapData[CAP_TO_INDEX(CAP_SETUID)].permitted |= CAP_TO_MASK(CAP_SETUID);
        CapData[CAP_TO_INDEX(CAP_IPC_OWNER)].permitted |= CAP_TO_MASK(CAP_IPC_OWNER);
        CapData[0].effective = CapData[0].permitted;
        CapData[1].effective = CapData[1].permitted;
        LxtCheckErrno(LxtCapSet(&CapHeader, CapData));
        LxtCheckEqual(Id, LxtShmGet(Key, PAGE_SIZE, 0777), "%Iu");

        //
        // Drop the CAP_IPC_OWNER capability.
        //

        memset(&CapData, 0, sizeof(CapData));
        memset(&CapHeader, 0, sizeof(CapHeader));
        CapHeader.version = _LINUX_CAPABILITY_VERSION_3;
        CapData[CAP_TO_INDEX(CAP_SETGID)].permitted |= CAP_TO_MASK(CAP_SETGID);
        CapData[CAP_TO_INDEX(CAP_SETUID)].permitted |= CAP_TO_MASK(CAP_SETUID);
        CapData[0].effective = CapData[0].permitted;
        CapData[1].effective = CapData[1].permitted;
        LxtCheckErrno(LxtCapSet(&CapHeader, CapData));
        LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, 0777), EACCES);
        LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, 0700), EACCES);
        LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, 0070), EACCES);
        LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, 0007), EACCES);
        LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, 0124), EACCES);
        LxtCheckEqual(Id, LxtShmGet(Key, PAGE_SIZE, 0000), "%Iu");

        //
        // Change the UID.
        //

        LxtCheckErrno(setuid(SHM_ACCESS_UID));
        LxtCheckErrno(LxtCapSet(&CapHeader, CapData));
        LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, 0777), EACCES);
        LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, 0700), EACCES);
        LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, 0070), EACCES);
        LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, 0007), EACCES);
        LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, 0124), EACCES);
        LxtCheckEqual(Id, LxtShmGet(Key, PAGE_SIZE, 0000), "%Iu");

        //
        // Change the GID.
        //

        LxtCheckErrno(setgid(SHM_ACCESS_GID));
        LxtCheckErrno(LxtCapSet(&CapHeader, CapData));
        LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, 0777), EACCES);
        LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, 0700), EACCES);
        LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, 0070), EACCES);
        LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, 0007), EACCES);
        LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, 0124), EACCES);
        LxtCheckEqual(Id, LxtShmGet(Key, PAGE_SIZE, 0000), "%Iu");

        //
        // Drop supplementary group membership.
        //

        LxtCheckErrno(Result = setgroups(0, NULL));
        LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, 0777), EACCES);
        LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, 0700), EACCES);
        LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, 0070), EACCES);
        LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, 0007), EACCES);
        LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, 0124), EACCES);
        LxtCheckEqual(Id, LxtShmGet(Key, PAGE_SIZE, 0000), "%Iu");
        goto ErrorExit;
    }

    //
    // Wait for the child to exit.
    //

    LxtCheckErrno(LxtWaitPidPoll(ChildPid, LXT_RESULT_SUCCESS));
    LxtCheckErrno(LxtShmCtl(Id, IPC_RMID, NULL));
    Id = -1;

    //
    // Create a shared memory region with a user read / write / execute mode.
    //

    Mode = 0700;
    LxtCheckErrno(Id = LxtShmGet(Key, PAGE_SIZE, (IPC_CREAT | IPC_EXCL | Mode)));
    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {

        //
        // First attempt with the CAP_IPC_OWNER capability.
        //

        LxtCheckErrno(prctl(PR_SET_KEEPCAPS, 1));
        memset(&CapData, 0, sizeof(CapData));
        memset(&CapHeader, 0, sizeof(CapHeader));
        CapHeader.version = _LINUX_CAPABILITY_VERSION_3;
        CapData[CAP_TO_INDEX(CAP_SETGID)].permitted |= CAP_TO_MASK(CAP_SETGID);
        CapData[CAP_TO_INDEX(CAP_SETUID)].permitted |= CAP_TO_MASK(CAP_SETUID);
        CapData[CAP_TO_INDEX(CAP_IPC_OWNER)].permitted |= CAP_TO_MASK(CAP_IPC_OWNER);
        CapData[0].effective = CapData[0].permitted;
        CapData[1].effective = CapData[1].permitted;
        LxtCheckErrno(LxtCapSet(&CapHeader, CapData));
        LxtCheckEqual(Id, LxtShmGet(Key, PAGE_SIZE, 0777), "%Iu");

        //
        // Drop the CAP_IPC_OWNER capability.
        //

        memset(&CapData, 0, sizeof(CapData));
        memset(&CapHeader, 0, sizeof(CapHeader));
        CapHeader.version = _LINUX_CAPABILITY_VERSION_3;
        CapData[CAP_TO_INDEX(CAP_SETGID)].permitted |= CAP_TO_MASK(CAP_SETGID);
        CapData[CAP_TO_INDEX(CAP_SETUID)].permitted |= CAP_TO_MASK(CAP_SETUID);
        CapData[0].effective = CapData[0].permitted;
        CapData[1].effective = CapData[1].permitted;
        LxtCheckErrno(LxtCapSet(&CapHeader, CapData));
        LxtCheckEqual(Id, LxtShmGet(Key, PAGE_SIZE, 0777), "%Iu");
        LxtCheckEqual(Id, LxtShmGet(Key, PAGE_SIZE, 0700), "%Iu");
        LxtCheckEqual(Id, LxtShmGet(Key, PAGE_SIZE, 0070), "%Iu");
        LxtCheckEqual(Id, LxtShmGet(Key, PAGE_SIZE, 0007), "%Iu");
        LxtCheckEqual(Id, LxtShmGet(Key, PAGE_SIZE, 0124), "%Iu");
        LxtCheckEqual(Id, LxtShmGet(Key, PAGE_SIZE, 0000), "%Iu");

        //
        // Change the UID.
        //

        LxtCheckErrno(setuid(SHM_ACCESS_UID));
        LxtCheckErrno(LxtCapSet(&CapHeader, CapData));
        LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, 0777), EACCES);
        LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, 0700), EACCES);
        LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, 0070), EACCES);
        LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, 0007), EACCES);
        LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, 0124), EACCES);
        LxtCheckEqual(Id, LxtShmGet(Key, PAGE_SIZE, 0000), "%Iu");

        //
        // Change the GID.
        //

        LxtCheckErrno(setgid(SHM_ACCESS_GID));
        LxtCheckErrno(LxtCapSet(&CapHeader, CapData));
        LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, 0777), EACCES);
        LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, 0700), EACCES);
        LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, 0070), EACCES);
        LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, 0007), EACCES);
        LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, 0124), EACCES);
        LxtCheckEqual(Id, LxtShmGet(Key, PAGE_SIZE, 0000), "%Iu");

        //
        // Drop supplementary group membership.
        //

        LxtCheckErrno(Result = setgroups(0, NULL));
        LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, 0777), EACCES);
        LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, 0700), EACCES);
        LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, 0070), EACCES);
        LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, 0007), EACCES);
        LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, 0124), EACCES);
        LxtCheckEqual(Id, LxtShmGet(Key, PAGE_SIZE, 0000), "%Iu");
        goto ErrorExit;
    }

    //
    // Wait for the child to exit.
    //

    LxtCheckErrno(LxtWaitPidPoll(ChildPid, LXT_RESULT_SUCCESS));
    LxtCheckErrno(LxtShmCtl(Id, IPC_RMID, NULL));
    Id = -1;

    //
    // Create a shared memory region with a group read / write / execute mode.
    //

    Mode = 0070;
    LxtCheckErrno(Id = LxtShmGet(Key, PAGE_SIZE, (IPC_CREAT | IPC_EXCL | Mode)));
    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {

        //
        // First attempt with the CAP_IPC_OWNER capability.
        //

        LxtCheckErrno(prctl(PR_SET_KEEPCAPS, 1));
        memset(&CapData, 0, sizeof(CapData));
        memset(&CapHeader, 0, sizeof(CapHeader));
        CapHeader.version = _LINUX_CAPABILITY_VERSION_3;
        CapData[CAP_TO_INDEX(CAP_SETGID)].permitted |= CAP_TO_MASK(CAP_SETGID);
        CapData[CAP_TO_INDEX(CAP_SETUID)].permitted |= CAP_TO_MASK(CAP_SETUID);
        CapData[CAP_TO_INDEX(CAP_IPC_OWNER)].permitted |= CAP_TO_MASK(CAP_IPC_OWNER);
        CapData[0].effective = CapData[0].permitted;
        CapData[1].effective = CapData[1].permitted;
        LxtCheckErrno(LxtCapSet(&CapHeader, CapData));
        LxtCheckEqual(Id, LxtShmGet(Key, PAGE_SIZE, 0777), "%Iu");

        //
        // Drop the CAP_IPC_OWNER capability.
        //

        memset(&CapData, 0, sizeof(CapData));
        memset(&CapHeader, 0, sizeof(CapHeader));
        CapHeader.version = _LINUX_CAPABILITY_VERSION_3;
        CapData[CAP_TO_INDEX(CAP_SETGID)].permitted |= CAP_TO_MASK(CAP_SETGID);
        CapData[CAP_TO_INDEX(CAP_SETUID)].permitted |= CAP_TO_MASK(CAP_SETUID);
        CapData[0].effective = CapData[0].permitted;
        CapData[1].effective = CapData[1].permitted;
        LxtCheckErrno(LxtCapSet(&CapHeader, CapData));
        LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, 0777), EACCES);
        LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, 0700), EACCES);
        LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, 0070), EACCES);
        LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, 0007), EACCES);
        LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, 0124), EACCES);
        LxtCheckEqual(Id, LxtShmGet(Key, PAGE_SIZE, 0000), "%Iu");

        //
        // Change the UID (group still matches so this should succeed).
        //

        LxtCheckErrno(setuid(SHM_ACCESS_UID));
        LxtCheckErrno(LxtCapSet(&CapHeader, CapData));
        LxtCheckEqual(Id, LxtShmGet(Key, PAGE_SIZE, 0777), "%Iu");
        LxtCheckEqual(Id, LxtShmGet(Key, PAGE_SIZE, 0700), "%Iu");
        LxtCheckEqual(Id, LxtShmGet(Key, PAGE_SIZE, 0070), "%Iu");
        LxtCheckEqual(Id, LxtShmGet(Key, PAGE_SIZE, 0007), "%Iu");
        LxtCheckEqual(Id, LxtShmGet(Key, PAGE_SIZE, 0124), "%Iu");
        LxtCheckEqual(Id, LxtShmGet(Key, PAGE_SIZE, 0000), "%Iu");

        //
        // Change the GID (callers still has supplementary group membership so
        // this should succeed).
        //

        LxtCheckErrno(setgid(SHM_ACCESS_GID));
        LxtCheckErrno(LxtCapSet(&CapHeader, CapData));
        LxtCheckEqual(Id, LxtShmGet(Key, PAGE_SIZE, 0777), "%Iu");
        LxtCheckEqual(Id, LxtShmGet(Key, PAGE_SIZE, 0700), "%Iu");
        LxtCheckEqual(Id, LxtShmGet(Key, PAGE_SIZE, 0070), "%Iu");
        LxtCheckEqual(Id, LxtShmGet(Key, PAGE_SIZE, 0007), "%Iu");
        LxtCheckEqual(Id, LxtShmGet(Key, PAGE_SIZE, 0124), "%Iu");
        LxtCheckEqual(Id, LxtShmGet(Key, PAGE_SIZE, 0000), "%Iu");

        //
        // Drop supplementary group membership.
        //

        LxtCheckErrno(Result = setgroups(0, NULL));
        LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, 0777), EACCES);
        LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, 0700), EACCES);
        LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, 0070), EACCES);
        LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, 0007), EACCES);
        LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, 0124), EACCES);
        LxtCheckEqual(Id, LxtShmGet(Key, PAGE_SIZE, 0000), "%Iu");
        goto ErrorExit;
    }

    //
    // Wait for the child to exit.
    //

    LxtCheckErrno(LxtWaitPidPoll(ChildPid, LXT_RESULT_SUCCESS));
    LxtCheckErrno(LxtShmCtl(Id, IPC_RMID, NULL));
    Id = -1;

    //
    // Create a shared memory region with a other read / write / execute mode.
    //

    Mode = 0007;
    LxtCheckErrno(Id = LxtShmGet(Key, PAGE_SIZE, (IPC_CREAT | IPC_EXCL | Mode)));
    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {

        //
        // First attempt with the CAP_IPC_OWNER capability.
        //

        LxtCheckErrno(prctl(PR_SET_KEEPCAPS, 1));
        memset(&CapData, 0, sizeof(CapData));
        memset(&CapHeader, 0, sizeof(CapHeader));
        CapHeader.version = _LINUX_CAPABILITY_VERSION_3;
        CapData[CAP_TO_INDEX(CAP_SETGID)].permitted |= CAP_TO_MASK(CAP_SETGID);
        CapData[CAP_TO_INDEX(CAP_SETUID)].permitted |= CAP_TO_MASK(CAP_SETUID);
        CapData[CAP_TO_INDEX(CAP_IPC_OWNER)].permitted |= CAP_TO_MASK(CAP_IPC_OWNER);
        CapData[0].effective = CapData[0].permitted;
        CapData[1].effective = CapData[1].permitted;
        LxtCheckErrno(LxtCapSet(&CapHeader, CapData));
        LxtCheckEqual(Id, LxtShmGet(Key, PAGE_SIZE, 0777), "%Iu");

        //
        // Drop the CAP_IPC_OWNER capability.
        //

        memset(&CapData, 0, sizeof(CapData));
        memset(&CapHeader, 0, sizeof(CapHeader));
        CapHeader.version = _LINUX_CAPABILITY_VERSION_3;
        CapData[CAP_TO_INDEX(CAP_SETGID)].permitted |= CAP_TO_MASK(CAP_SETGID);
        CapData[CAP_TO_INDEX(CAP_SETUID)].permitted |= CAP_TO_MASK(CAP_SETUID);
        CapData[0].effective = CapData[0].permitted;
        CapData[1].effective = CapData[1].permitted;
        LxtCheckErrno(LxtCapSet(&CapHeader, CapData));
        LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, 0777), EACCES);
        LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, 0700), EACCES);
        LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, 0070), EACCES);
        LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, 0007), EACCES);
        LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, 0124), EACCES);
        LxtCheckEqual(Id, LxtShmGet(Key, PAGE_SIZE, 0000), "%Iu");

        //
        // Change the UID.
        //

        LxtCheckErrno(setuid(SHM_ACCESS_UID));
        LxtCheckErrno(LxtCapSet(&CapHeader, CapData));
        LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, 0777), EACCES);
        LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, 0700), EACCES);
        LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, 0070), EACCES);
        LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, 0007), EACCES);
        LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, 0124), EACCES);
        LxtCheckEqual(Id, LxtShmGet(Key, PAGE_SIZE, 0000), "%Iu");

        //
        // Change the GID (callers still has supplementary group membership so
        // this should succeed).
        //

        LxtCheckErrno(setgid(SHM_ACCESS_GID));
        LxtCheckErrno(LxtCapSet(&CapHeader, CapData));
        LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, 0777), EACCES);
        LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, 0700), EACCES);
        LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, 0070), EACCES);
        LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, 0007), EACCES);
        LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, 0124), EACCES);
        LxtCheckEqual(Id, LxtShmGet(Key, PAGE_SIZE, 0000), "%Iu");

        //
        // Drop supplementary group membership (this should succeed).
        //

        LxtCheckErrno(Result = setgroups(0, NULL));
        LxtCheckEqual(Id, LxtShmGet(Key, PAGE_SIZE, 0777), "%Iu");
        LxtCheckEqual(Id, LxtShmGet(Key, PAGE_SIZE, 0700), "%Iu");
        LxtCheckEqual(Id, LxtShmGet(Key, PAGE_SIZE, 0070), "%Iu");
        LxtCheckEqual(Id, LxtShmGet(Key, PAGE_SIZE, 0007), "%Iu");
        LxtCheckEqual(Id, LxtShmGet(Key, PAGE_SIZE, 0124), "%Iu");
        LxtCheckEqual(Id, LxtShmGet(Key, PAGE_SIZE, 0000), "%Iu");
        goto ErrorExit;
    }

    //
    // Wait for the child to exit.
    //

    LxtCheckErrno(LxtWaitPidPoll(ChildPid, LXT_RESULT_SUCCESS));
    LxtCheckErrno(LxtShmCtl(Id, IPC_RMID, NULL));
    Id = -1;

    //
    // Create a shared memory region with a other read / write mode.
    //

    Mode = 0006;
    LxtCheckErrno(Id = LxtShmGet(Key, PAGE_SIZE, (IPC_CREAT | IPC_EXCL | Mode)));
    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {

        //
        // First attempt with the CAP_IPC_OWNER capability.
        //

        LxtCheckErrno(prctl(PR_SET_KEEPCAPS, 1));
        memset(&CapData, 0, sizeof(CapData));
        memset(&CapHeader, 0, sizeof(CapHeader));
        CapHeader.version = _LINUX_CAPABILITY_VERSION_3;
        CapData[CAP_TO_INDEX(CAP_SETGID)].permitted |= CAP_TO_MASK(CAP_SETGID);
        CapData[CAP_TO_INDEX(CAP_SETUID)].permitted |= CAP_TO_MASK(CAP_SETUID);
        CapData[CAP_TO_INDEX(CAP_IPC_OWNER)].permitted |= CAP_TO_MASK(CAP_IPC_OWNER);
        CapData[0].effective = CapData[0].permitted;
        CapData[1].effective = CapData[1].permitted;
        LxtCheckErrno(LxtCapSet(&CapHeader, CapData));
        LxtCheckEqual(Id, LxtShmGet(Key, PAGE_SIZE, 0777), "%Iu");

        //
        // Drop the CAP_IPC_OWNER capability.
        //

        memset(&CapData, 0, sizeof(CapData));
        memset(&CapHeader, 0, sizeof(CapHeader));
        CapHeader.version = _LINUX_CAPABILITY_VERSION_3;
        CapData[CAP_TO_INDEX(CAP_SETGID)].permitted |= CAP_TO_MASK(CAP_SETGID);
        CapData[CAP_TO_INDEX(CAP_SETUID)].permitted |= CAP_TO_MASK(CAP_SETUID);
        CapData[0].effective = CapData[0].permitted;
        CapData[1].effective = CapData[1].permitted;
        LxtCheckErrno(LxtCapSet(&CapHeader, CapData));
        LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, 0777), EACCES);
        LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, 0700), EACCES);
        LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, 0070), EACCES);
        LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, 0007), EACCES);
        LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, 0124), EACCES);
        LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, 0006), EACCES);
        LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, 0004), EACCES);
        LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, 0002), EACCES);
        LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, 0001), EACCES);
        LxtCheckEqual(Id, LxtShmGet(Key, PAGE_SIZE, 0000), "%Iu");

        //
        // Change the UID.
        //

        LxtCheckErrno(setuid(SHM_ACCESS_UID));
        LxtCheckErrno(LxtCapSet(&CapHeader, CapData));
        LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, 0777), EACCES);
        LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, 0700), EACCES);
        LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, 0070), EACCES);
        LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, 0007), EACCES);
        LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, 0124), EACCES);
        LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, 0006), EACCES);
        LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, 0004), EACCES);
        LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, 0002), EACCES);
        LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, 0001), EACCES);
        LxtCheckEqual(Id, LxtShmGet(Key, PAGE_SIZE, 0000), "%Iu");

        //
        // Change the GID (callers still has supplementary group membership so
        // this should succeed).
        //

        LxtCheckErrno(setgid(SHM_ACCESS_GID));
        LxtCheckErrno(LxtCapSet(&CapHeader, CapData));
        LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, 0777), EACCES);
        LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, 0700), EACCES);
        LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, 0070), EACCES);
        LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, 0007), EACCES);
        LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, 0124), EACCES);
        LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, 0006), EACCES);
        LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, 0004), EACCES);
        LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, 0002), EACCES);
        LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, 0001), EACCES);
        LxtCheckEqual(Id, LxtShmGet(Key, PAGE_SIZE, 0000), "%Iu");

        //
        // Drop supplementary group membership (this should succeed).
        //

        LxtCheckErrno(Result = setgroups(0, NULL));
        LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, 0777), EACCES);
        LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, 0700), EACCES);
        LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, 0070), EACCES);
        LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, 0007), EACCES);
        LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, 0124), EACCES);
        LxtCheckEqual(Id, LxtShmGet(Key, PAGE_SIZE, 0666), "%Iu");
        LxtCheckEqual(Id, LxtShmGet(Key, PAGE_SIZE, 0600), "%Iu");
        LxtCheckEqual(Id, LxtShmGet(Key, PAGE_SIZE, 0060), "%Iu");
        LxtCheckEqual(Id, LxtShmGet(Key, PAGE_SIZE, 0006), "%Iu");
        LxtCheckEqual(Id, LxtShmGet(Key, PAGE_SIZE, 0024), "%Iu");
        LxtCheckEqual(Id, LxtShmGet(Key, PAGE_SIZE, 0424), "%Iu");
        LxtCheckEqual(Id, LxtShmGet(Key, PAGE_SIZE, 0024), "%Iu");
        LxtCheckEqual(Id, LxtShmGet(Key, PAGE_SIZE, 0000), "%Iu");
        goto ErrorExit;
    }

    //
    // Wait for the child to exit.
    //

    LxtCheckErrno(LxtWaitPidPoll(ChildPid, LXT_RESULT_SUCCESS));
    LxtCheckErrno(LxtShmCtl(Id, IPC_RMID, NULL));
    Id = -1;

ErrorExit:
    if (ChildPid == 0)
    {
        _exit(Result);
    }

    //
    // N.B. The identifier should not be removed by any child processes.
    //

    if (Id != -1)
    {
        LxtShmCtl(Id, IPC_RMID, NULL);
    }

    return Result;
}

int ShmGetSyscall(PLXT_ARGS Args)

{

    struct __user_cap_data_struct CapData[2];
    struct __user_cap_header_struct CapHeader;
    int ChildPid;
    int Id;
    key_t Key;
    int Mode;
    size_t Result;
    struct shmid_ds Stat;
    time_t Time;

    ChildPid = -1;
    Id = -1;

    //
    // Create a key, verify that creating the key with the IPC_EXCL flag fails.
    //

    Mode = 0000;
    LxtLogInfo("Mode %o", Mode);
    LxtCheckErrno(LxtGetrandom(&Key, sizeof(Key), 0));
    LxtLogInfo("Key = %u", Key);
    LxtCheckErrno(Id = LxtShmGet(Key, PAGE_SIZE, (IPC_CREAT | IPC_EXCL | Mode)));
    LxtLogInfo("Id = %d", Id);
    LxtCheckErrno(LxtShmCtl(Id, IPC_STAT, &Stat));
    ShmPrintInfo(&Stat);
    LxtCheckEqual(Key, Stat.shm_perm.__key, "%Iu");
    LxtCheckEqual(PAGE_SIZE, Stat.shm_segsz, "%Iu");
    LxtCheckEqual(getpid(), Stat.shm_cpid, "%Iu");
    LxtCheckEqual(0, Stat.shm_lpid, "%Iu");
    LxtCheckEqual(0, Stat.shm_atime, "%Iu");
    LxtCheckEqual(0, Stat.shm_dtime, "%Iu");
    LxtCheckNotEqual(0, Stat.shm_ctime, "%Iu");
    LxtCheckEqual(Mode, Stat.shm_perm.mode, "%o");
    LxtCheckEqual(getuid(), Stat.shm_perm.cuid, "%d");
    LxtCheckEqual(getuid(), Stat.shm_perm.uid, "%d");
    LxtCheckEqual(getgid(), Stat.shm_perm.cgid, "%d");
    LxtCheckEqual(getgid(), Stat.shm_perm.gid, "%d");

    //
    // shmget with IPC_CREAT or IPC_EXCL when the region already exists.
    //

    LxtCheckEqual(Id, LxtShmGet(Key, PAGE_SIZE, IPC_CREAT), "%Iu");
    LxtCheckEqual(Id, LxtShmGet(Key, PAGE_SIZE, IPC_EXCL), "%Iu");
    LxtCheckEqual(Id, LxtShmGet(Key, PAGE_SIZE, 0), "%Iu");

    //
    // Create a child with a different uid and gid that does not have the
    // IPC_OWNER capability.
    //

    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        LxtCheckErrno(prctl(PR_SET_KEEPCAPS, 1));
        LxtCheckErrno(setgid(SHM_ACCESS_GID));
        LxtCheckErrno(setuid(SHM_ACCESS_UID));
        memset(&CapData, 0, sizeof(CapData));
        memset(&CapHeader, 0, sizeof(CapHeader));
        CapHeader.version = _LINUX_CAPABILITY_VERSION_3;
        CapData[CAP_TO_INDEX(CAP_SETGID)].permitted |= CAP_TO_MASK(CAP_SETGID);
        CapData[CAP_TO_INDEX(CAP_IPC_OWNER)].permitted |= CAP_TO_MASK(CAP_IPC_OWNER);
        CapData[0].effective = CapData[0].permitted;
        CapData[1].effective = CapData[1].permitted;
        LxtCheckErrno(LxtCapSet(&CapHeader, CapData));

        //
        // These should succeed because the child still has the IPC_OWNER cap.
        //

        LxtCheckEqual(Id, LxtShmGet(Key, PAGE_SIZE, IPC_CREAT), "%Iu");
        LxtCheckEqual(Id, LxtShmGet(Key, PAGE_SIZE, IPC_EXCL), "%Iu");
        LxtCheckEqual(Id, LxtShmGet(Key, PAGE_SIZE, 0777), "%Iu");
        LxtCheckEqual(Id, LxtShmGet(Key, PAGE_SIZE, 0666), "%Iu");
        LxtCheckEqual(Id, LxtShmGet(Key, PAGE_SIZE, 0600), "%Iu");
        LxtCheckEqual(Id, LxtShmGet(Key, PAGE_SIZE, 0060), "%Iu");
        LxtCheckEqual(Id, LxtShmGet(Key, PAGE_SIZE, 0006), "%Iu");
        LxtCheckEqual(Id, LxtShmGet(Key, PAGE_SIZE, 0), "%Iu");

        //
        // Drop all group membership and the CAP_IPC_OWNER capability and
        // attempt to call shmget with unmatching mode bits.
        //

        LxtCheckErrno(Result = setgroups(0, NULL));
        memset(&CapData, 0, sizeof(CapData));
        memset(&CapHeader, 0, sizeof(CapHeader));
        CapHeader.version = _LINUX_CAPABILITY_VERSION_3;
        LxtCheckErrno(LxtCapSet(&CapHeader, CapData));
        LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, 0777), EACCES);
        LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, 0666), EACCES);
        LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, 0600), EACCES);
        LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, 0060), EACCES);
        LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, 0006), EACCES);

        //
        // Use the same permission as before, these should succeed.
        //

        LxtCheckEqual(Id, LxtShmGet(Key, PAGE_SIZE, IPC_CREAT), "%Iu");
        LxtCheckEqual(Id, LxtShmGet(Key, PAGE_SIZE, IPC_EXCL), "%Iu");
        LxtCheckEqual(Id, LxtShmGet(Key, PAGE_SIZE, 0), "%Iu");
        goto ErrorExit;
    }

    //
    // Wait for the child to exit.
    //

    LxtCheckErrno(LxtWaitPidPoll(ChildPid, LXT_RESULT_SUCCESS));

    //
    // shmget with size = 0 should succeed.
    //

    LxtCheckEqual(Id, LxtShmGet(Key, 0, 0), "%Iu");

    //
    // Invalid parameter variations.
    //

    //
    // shmget with IPC_CREAT | IPC_EXCL when the region already exists, should
    // succeed with only IPC_EXCL.
    //

    LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, (IPC_CREAT | IPC_EXCL)), EEXIST);

    //
    // shmget with a known key and a size that does not match.
    //

    LxtCheckErrnoFailure(LxtShmGet(Key, (PAGE_SIZE * 2), 0), EINVAL);
    LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE + 1, 0), EINVAL);

    //
    // N.B. There appears to be no error checking for invalid flags, only the
    //      presence of valid flags.
    //
    // -1 includes the IPC_EXCL flag so this should return EEXIST.
    //

    LxtCheckErrnoFailure(LxtShmGet(Key, PAGE_SIZE, -1), EEXIST);
    LxtCheckEqual(Id, LxtShmGet(Key, PAGE_SIZE, (-1 & ~IPC_EXCL)), "%Iu");

    //
    // Delete the region and create a new one with a size of one byte.
    //

    LxtCheckErrno(LxtShmCtl(Id, IPC_RMID, &Stat));
    Id = -1;
    LxtCheckErrno(Id = LxtShmGet(IPC_PRIVATE, 1, 0));
    LxtCheckErrno(LxtShmCtl(Id, IPC_STAT, &Stat));
    LxtCheckEqual(1, Stat.shm_segsz, "%Iu");

    //
    // Delete the region and create a new region with a size of zero bytes
    // (should fail).
    //

    LxtCheckErrno(LxtShmCtl(Id, IPC_RMID, &Stat));
    Id = -1;
    LxtCheckErrnoFailure(Id = LxtShmGet(IPC_PRIVATE, 0, 0), EINVAL);

ErrorExit:
    if (ChildPid == 0)
    {
        _exit(Result);
    }

    if (Id != -1)
    {
        LxtShmCtl(Id, IPC_RMID, &Stat);
    }

    return Result;
}

int ShmCtlSyscall(PLXT_ARGS Args)

{

    struct __user_cap_data_struct CapData[2];
    struct __user_cap_header_struct CapHeader;
    int ChildPid;
    int Id;
    struct shminfo IpcInfo;
    key_t Key;
    struct shmid_ds OldStat;
    int RandomId;
    size_t Result;
    struct shm_info ShmInfo;
    struct shmid_ds Stat = {0};

    Id = -1;

    //
    // Test permissions for the IPC_STAT.
    //

    LxtCheckErrno(Id = LxtShmGet(IPC_PRIVATE, PAGE_SIZE, 0));
    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {

        //
        // First attempt with the CAP_IPC_OWNER capability.
        //

        LxtCheckErrno(prctl(PR_SET_KEEPCAPS, 1));
        memset(&CapData, 0, sizeof(CapData));
        memset(&CapHeader, 0, sizeof(CapHeader));
        CapHeader.version = _LINUX_CAPABILITY_VERSION_3;
        CapData[CAP_TO_INDEX(CAP_SETGID)].permitted |= CAP_TO_MASK(CAP_SETGID);
        CapData[CAP_TO_INDEX(CAP_SETUID)].permitted |= CAP_TO_MASK(CAP_SETUID);
        CapData[CAP_TO_INDEX(CAP_IPC_OWNER)].permitted |= CAP_TO_MASK(CAP_IPC_OWNER);
        CapData[0].effective = CapData[0].permitted;
        CapData[1].effective = CapData[1].permitted;
        LxtCheckErrno(LxtCapSet(&CapHeader, CapData));
        LxtCheckErrno(LxtShmCtl(Id, IPC_STAT, &Stat));

        //
        // Drop the CAP_IPC_OWNER capability and verify that the region cannot
        // be queried.
        //

        memset(&CapData, 0, sizeof(CapData));
        memset(&CapHeader, 0, sizeof(CapHeader));
        CapHeader.version = _LINUX_CAPABILITY_VERSION_3;
        CapData[CAP_TO_INDEX(CAP_SETGID)].permitted |= CAP_TO_MASK(CAP_SETGID);
        CapData[CAP_TO_INDEX(CAP_SETUID)].permitted |= CAP_TO_MASK(CAP_SETUID);
        CapData[0].effective = CapData[0].permitted;
        CapData[1].effective = CapData[1].permitted;
        LxtCheckErrno(LxtCapSet(&CapHeader, CapData));
        LxtCheckErrnoFailure(LxtShmCtl(Id, IPC_STAT, &Stat), EACCES);

        //
        // Create a no access shared memory region and verify that it cannot be
        // queried without the CAP_IPC_OWNER (even by its owner).
        //

        LxtCheckErrno(Id = LxtShmGet(IPC_PRIVATE, PAGE_SIZE, 0));
        LxtCheckErrnoFailure(LxtShmCtl(Id, IPC_STAT, &Stat), EACCES);
        LxtCheckErrno(LxtShmCtl(Id, IPC_RMID, NULL));

        //
        // Create a write only shared memory region and verify that it cannot be
        // queried without the CAP_IPC_OWNER (even by its owner).
        //

        LxtCheckErrno(Id = LxtShmGet(IPC_PRIVATE, PAGE_SIZE, 0200));
        LxtCheckErrnoFailure(LxtShmCtl(Id, IPC_STAT, &Stat), EACCES);
        LxtCheckErrno(LxtShmCtl(Id, IPC_RMID, NULL));

        //
        // Create a read only shared memory region and verify that it can be
        // queried.
        //

        LxtCheckErrno(Id = LxtShmGet(IPC_PRIVATE, PAGE_SIZE, 0400));
        LxtCheckErrno(LxtShmCtl(Id, IPC_STAT, &Stat));
        LxtCheckErrno(LxtShmCtl(Id, IPC_RMID, NULL));
        goto ErrorExit;
    }

    //
    // Wait for the child to exit.
    //

    LxtCheckErrno(LxtWaitPidPoll(ChildPid, LXT_RESULT_SUCCESS));

    //
    // Test permissions for IPC_SET.
    //

    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {

        //
        // First attempt with the CAP_IPC_OWNER capability.
        //

        LxtCheckErrno(prctl(PR_SET_KEEPCAPS, 1));
        memset(&CapData, 0, sizeof(CapData));
        memset(&CapHeader, 0, sizeof(CapHeader));
        CapHeader.version = _LINUX_CAPABILITY_VERSION_3;
        CapData[CAP_TO_INDEX(CAP_SETGID)].permitted |= CAP_TO_MASK(CAP_SETGID);
        CapData[CAP_TO_INDEX(CAP_SETUID)].permitted |= CAP_TO_MASK(CAP_SETUID);
        CapData[CAP_TO_INDEX(CAP_IPC_OWNER)].permitted |= CAP_TO_MASK(CAP_IPC_OWNER);
        CapData[0].effective = CapData[0].permitted;
        CapData[1].effective = CapData[1].permitted;
        LxtCheckErrno(LxtCapSet(&CapHeader, CapData));
        LxtCheckErrno(LxtShmCtl(Id, IPC_STAT, &Stat));
        LxtCheckErrno(LxtShmCtl(Id, IPC_SET, &Stat));

        //
        // Drop the CAP_IPC_OWNER capability and verify that IPC_SET can still
        // be called by the owner.
        //

        memset(&CapData, 0, sizeof(CapData));
        memset(&CapHeader, 0, sizeof(CapHeader));
        CapHeader.version = _LINUX_CAPABILITY_VERSION_3;
        CapData[CAP_TO_INDEX(CAP_SETGID)].permitted |= CAP_TO_MASK(CAP_SETGID);
        CapData[CAP_TO_INDEX(CAP_SETUID)].permitted |= CAP_TO_MASK(CAP_SETUID);
        CapData[0].effective = CapData[0].permitted;
        CapData[1].effective = CapData[1].permitted;
        LxtCheckErrno(LxtCapSet(&CapHeader, CapData));
        LxtCheckErrno(LxtShmCtl(Id, IPC_SET, &Stat));

        //
        // Change the GID.
        //

        LxtCheckErrno(setgid(SHM_ACCESS_GID));
        LxtCheckErrno(LxtCapSet(&CapHeader, CapData));
        LxtCheckErrno(LxtShmCtl(Id, IPC_SET, &Stat));

        //
        // Drop supplementary group membership.
        //

        LxtCheckErrno(Result = setgroups(0, NULL));
        LxtCheckErrno(LxtShmCtl(Id, IPC_SET, &Stat));

        //
        // Change the UID (this should fail).
        //

        LxtCheckErrno(setuid(SHM_ACCESS_UID));
        LxtCheckErrno(LxtCapSet(&CapHeader, CapData));
        LxtCheckErrnoFailure(LxtShmCtl(Id, IPC_SET, NULL), EFAULT);
        LxtCheckErrnoFailure(LxtShmCtl(Id, IPC_SET, &Stat), EPERM);
        goto ErrorExit;
    }

    //
    // Wait for the child to exit.
    //

    LxtCheckErrno(LxtWaitPidPoll(ChildPid, LXT_RESULT_SUCCESS));

    //
    // Test permissions for IPC_SET.
    //

    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {

        //
        // First attempt with the CAP_IPC_OWNER capability.
        //

        LxtCheckErrno(prctl(PR_SET_KEEPCAPS, 1));
        memset(&CapData, 0, sizeof(CapData));
        memset(&CapHeader, 0, sizeof(CapHeader));
        CapHeader.version = _LINUX_CAPABILITY_VERSION_3;
        CapData[CAP_TO_INDEX(CAP_SETGID)].permitted |= CAP_TO_MASK(CAP_SETGID);
        CapData[CAP_TO_INDEX(CAP_SETUID)].permitted |= CAP_TO_MASK(CAP_SETUID);
        CapData[CAP_TO_INDEX(CAP_IPC_OWNER)].permitted |= CAP_TO_MASK(CAP_IPC_OWNER);
        CapData[0].effective = CapData[0].permitted;
        CapData[1].effective = CapData[1].permitted;
        LxtCheckErrno(LxtCapSet(&CapHeader, CapData));
        LxtCheckErrno(LxtShmCtl(Id, IPC_STAT, &Stat));
        LxtCheckErrno(LxtShmCtl(Id, IPC_SET, &Stat));

        //
        // Drop the CAP_IPC_OWNER capability and verify that IPC_SET can still
        // be called by the creator and owner.
        //

        memset(&CapData, 0, sizeof(CapData));
        memset(&CapHeader, 0, sizeof(CapHeader));
        CapHeader.version = _LINUX_CAPABILITY_VERSION_3;
        CapData[CAP_TO_INDEX(CAP_SETGID)].permitted |= CAP_TO_MASK(CAP_SETGID);
        CapData[CAP_TO_INDEX(CAP_SETUID)].permitted |= CAP_TO_MASK(CAP_SETUID);
        CapData[0].effective = CapData[0].permitted;
        CapData[1].effective = CapData[1].permitted;
        LxtCheckErrno(LxtCapSet(&CapHeader, CapData));
        LxtCheckErrno(LxtShmCtl(Id, IPC_SET, &Stat));

        //
        // Change the owner UID.
        //

        Stat.shm_perm.uid = SHM_ACCESS_UID;
        LxtCheckErrno(LxtShmCtl(Id, IPC_SET, &Stat));

        //
        // Change the GID.
        //

        LxtCheckErrno(setgid(SHM_ACCESS_GID));
        LxtCheckErrno(LxtCapSet(&CapHeader, CapData));
        LxtCheckErrno(LxtShmCtl(Id, IPC_SET, &Stat));

        //
        // Drop supplementary group membership.
        //

        LxtCheckErrno(Result = setgroups(0, NULL));
        LxtCheckErrno(LxtShmCtl(Id, IPC_SET, &Stat));

        //
        // Change the UID to match (this should succeed).
        //

        LxtCheckErrno(setuid(SHM_ACCESS_UID));
        LxtCheckErrno(LxtCapSet(&CapHeader, CapData));
        LxtCheckErrno(LxtShmCtl(Id, IPC_SET, &Stat));

        //
        // IPC_STAT should still fail.
        //

        LxtCheckErrnoFailure(LxtShmCtl(Id, IPC_STAT, &Stat), EACCES);
        goto ErrorExit;
    }

    //
    // Wait for the child to exit.
    //

    LxtCheckErrno(LxtWaitPidPoll(ChildPid, LXT_RESULT_SUCCESS));

    //
    // Reset the region's UID.
    //

    Stat.shm_perm.uid = getuid();
    LxtCheckErrno(LxtShmCtl(Id, IPC_SET, &Stat));

    //
    // Test permissions for SHM_LOCK / SHM_UNLOCK.
    //

    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {

        //
        // Drop the CAP_IPC_LOCK capability.
        //

        memset(&CapHeader, 0, sizeof(CapHeader));
        CapHeader.version = _LINUX_CAPABILITY_VERSION_3;
        LxtCheckErrno(LxtCapGet(&CapHeader, CapData)) LxtCheckErrno(prctl(PR_SET_KEEPCAPS, 1));
        CapData[CAP_TO_INDEX(CAP_IPC_LOCK)].permitted &= ~CAP_TO_MASK(CAP_IPC_LOCK);
        CapData[0].effective = CapData[0].permitted;
        CapData[1].effective = CapData[1].permitted;
        LxtCheckErrno(LxtCapSet(&CapHeader, CapData));

        //
        // Change the UID and verify SHM_LOCK and SHM_UNLOCK fail.
        //

        LxtCheckErrno(setuid(SHM_ACCESS_UID));
        LxtCheckErrno(LxtCapSet(&CapHeader, CapData));
        LxtCheckErrnoFailure(LxtShmCtl(Id, SHM_LOCK, NULL), EPERM);
        LxtCheckErrnoFailure(LxtShmCtl(Id, SHM_UNLOCK, NULL), EPERM);
        goto ErrorExit;
    }

    //
    // Wait for the child to exit.
    //

    LxtCheckErrno(LxtWaitPidPoll(ChildPid, LXT_RESULT_SUCCESS));

    //
    // Test permissions for IPC_RMID.
    //

    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {

        //
        // Drop the CAP_IPC_OWNER capability.
        //

        LxtCheckErrno(prctl(PR_SET_KEEPCAPS, 1));
        memset(&CapData, 0, sizeof(CapData));
        memset(&CapHeader, 0, sizeof(CapHeader));
        CapHeader.version = _LINUX_CAPABILITY_VERSION_3;
        CapData[CAP_TO_INDEX(CAP_SETGID)].permitted |= CAP_TO_MASK(CAP_SETGID);
        CapData[CAP_TO_INDEX(CAP_SETUID)].permitted |= CAP_TO_MASK(CAP_SETUID);
        CapData[0].effective = CapData[0].permitted;
        CapData[1].effective = CapData[1].permitted;
        LxtCheckErrno(LxtCapSet(&CapHeader, CapData));

        //
        // Change the UID and verify IPC_RMID fails.
        //

        LxtCheckErrno(setuid(SHM_ACCESS_UID));
        LxtCheckErrno(LxtCapSet(&CapHeader, CapData));
        LxtCheckErrnoFailure(LxtShmCtl(Id, IPC_RMID, NULL), EPERM);
        goto ErrorExit;
    }

    //
    // Wait for the child to exit.
    //

    LxtCheckErrno(LxtWaitPidPoll(ChildPid, LXT_RESULT_SUCCESS));

    //
    // Verify IPC_RMID can be called by the memory region's owner.
    //

    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {

        //
        // Drop the CAP_IPC_OWNER capability.
        //

        LxtCheckErrno(prctl(PR_SET_KEEPCAPS, 1));
        memset(&CapData, 0, sizeof(CapData));
        memset(&CapHeader, 0, sizeof(CapHeader));
        CapHeader.version = _LINUX_CAPABILITY_VERSION_3;
        CapData[CAP_TO_INDEX(CAP_SETGID)].permitted |= CAP_TO_MASK(CAP_SETGID);
        CapData[CAP_TO_INDEX(CAP_SETUID)].permitted |= CAP_TO_MASK(CAP_SETUID);
        CapData[0].effective = CapData[0].permitted;
        CapData[1].effective = CapData[1].permitted;
        LxtCheckErrno(LxtCapSet(&CapHeader, CapData));

        //
        // Change the owner UID.
        //

        Stat.shm_perm.uid = SHM_ACCESS_UID;
        LxtCheckErrno(LxtShmCtl(Id, IPC_SET, &Stat));

        //
        // Change the caller's UID to match.
        //

        LxtCheckErrno(setuid(SHM_ACCESS_UID));
        LxtCheckErrno(LxtCapSet(&CapHeader, CapData));
        LxtCheckErrno(LxtShmCtl(Id, IPC_RMID, NULL));
        goto ErrorExit;
    }

    //
    // Wait for the child to exit.
    //

    LxtCheckErrno(LxtWaitPidPoll(ChildPid, LXT_RESULT_SUCCESS));

    //
    // Create a new shared memory region since the previous was just deleted.
    //

    LxtCheckErrno(Id = LxtShmGet(IPC_PRIVATE, PAGE_SIZE, 0));

    //
    // Verify IPC_INFO.
    //

    LxtCheckErrno(LxtShmCtl(Id, IPC_INFO, &IpcInfo));
    LxtCheckErrno(LxtShmCtl(0, IPC_INFO, &IpcInfo));
    LxtLogInfo("shminfo.shmmax %Iu", IpcInfo.shmmax);
    LxtLogInfo("shminfo.shmmin %Iu", IpcInfo.shmmin);
    LxtLogInfo("shminfo.shmmni %Iu", IpcInfo.shmmni);
    LxtLogInfo("shminfo.shmseg %Iu", IpcInfo.shmseg);
    LxtLogInfo("shminfo.shmall %Iu", IpcInfo.shmall);
    LxtCheckEqual(IpcInfo.shmmin, 1, "%Iu");

    //
    // Verify SHM_INFO.
    //

    LxtCheckErrno(LxtShmCtl(Id, SHM_INFO, &ShmInfo));
    LxtCheckErrno(LxtShmCtl(0, SHM_INFO, &ShmInfo));
    LxtLogInfo("shm_info.used_ids %Iu", ShmInfo.used_ids);
    LxtLogInfo("shm_info.shm_tot %Iu", ShmInfo.shm_tot);
    LxtLogInfo("shm_info.shm_rss %Iu", ShmInfo.shm_rss);
    LxtLogInfo("shm_info.shm_swp %Iu", ShmInfo.shm_swp);
    LxtLogInfo("shm_info.swap_attempts %Iu", ShmInfo.swap_attempts);
    LxtLogInfo("shm_info.swap_successes %Iu", ShmInfo.swap_successes);
    LxtCheckNotEqual(ShmInfo.used_ids, 0, "%Iu");

    //
    // Verify SHM_LOCK and SHM_UNLOCK. The locked state is boolean (there is
    // no count for locked / unlocked).
    //

    LxtCheckErrno(LxtShmCtl(Id, SHM_LOCK, NULL));
    LxtCheckErrno(LxtShmCtl(Id, IPC_STAT, &Stat));
    LxtCheckEqual(SHM_LOCKED, Stat.shm_perm.mode & SHM_LOCKED, "%o");
    LxtCheckErrno(LxtShmCtl(Id, SHM_LOCK, NULL));
    LxtCheckErrno(LxtShmCtl(Id, IPC_STAT, &Stat));
    LxtCheckEqual(SHM_LOCKED, Stat.shm_perm.mode & SHM_LOCKED, "%o");
    LxtCheckErrno(LxtShmCtl(Id, SHM_UNLOCK, NULL));
    LxtCheckErrno(LxtShmCtl(Id, IPC_STAT, &Stat));
    LxtCheckEqual(0, Stat.shm_perm.mode & SHM_LOCKED, "%o");
    LxtCheckErrno(LxtShmCtl(Id, SHM_UNLOCK, NULL));
    LxtCheckErrno(LxtShmCtl(Id, IPC_STAT, &Stat));
    LxtCheckEqual(0, Stat.shm_perm.mode & SHM_LOCKED, "%o");

    //
    // Invalid parameter variations.
    //

    //
    // Ensure IPC_SET cannot set invalid mode bits (they are silently ignored).
    //

    LxtCheckErrno(LxtShmCtl(Id, IPC_STAT, &Stat));
    Stat.shm_perm.mode = -1;
    LxtCheckErrno(LxtShmCtl(Id, IPC_SET, &Stat));
    LxtCheckErrno(LxtShmCtl(Id, IPC_STAT, &Stat));
    LxtCheckEqual(Stat.shm_perm.mode, 0777, "%o");

    //
    // Ensure the uid and gid cannot be set to -1.
    //

    LxtCheckErrno(LxtShmCtl(Id, IPC_STAT, &OldStat));
    Stat = OldStat;
    Stat.shm_perm.uid = -1;
    LxtCheckErrnoFailure(LxtShmCtl(Id, IPC_SET, &Stat), EINVAL);
    Stat = OldStat;
    Stat.shm_perm.gid = -1;
    LxtCheckErrnoFailure(LxtShmCtl(Id, IPC_SET, &Stat), EINVAL);
    LxtCheckErrno(LxtShmCtl(Id, IPC_STAT, &Stat));
    LxtCheckEqual(Stat.shm_perm.uid, OldStat.shm_perm.uid, "%d");
    LxtCheckEqual(Stat.shm_perm.gid, OldStat.shm_perm.gid, "%d");

    LxtCheckErrnoFailure(LxtShmCtl(-1, IPC_STAT, NULL), EINVAL);
    LxtCheckErrnoFailure(LxtShmCtl(Id, IPC_STAT, NULL), EFAULT);
    LxtCheckErrnoFailure(LxtShmCtl(Id, IPC_STAT, -1), EFAULT);
    LxtCheckErrnoFailure(LxtShmCtl(-1, IPC_SET, NULL), EINVAL);
    LxtCheckErrnoFailure(LxtShmCtl(Id, IPC_SET, NULL), EFAULT);
    LxtCheckErrnoFailure(LxtShmCtl(Id, IPC_SET, -1), EFAULT);
    LxtCheckErrnoFailure(LxtShmCtl(-1, IPC_INFO, NULL), EINVAL);
    LxtCheckErrnoFailure(LxtShmCtl(Id, IPC_INFO, NULL), EFAULT);
    LxtCheckErrnoFailure(LxtShmCtl(Id, IPC_INFO, -1), EFAULT);
    LxtCheckErrnoFailure(LxtShmCtl(-1, SHM_INFO, NULL), EINVAL);
    LxtCheckErrnoFailure(LxtShmCtl(Id, SHM_INFO, NULL), EFAULT);
    LxtCheckErrnoFailure(LxtShmCtl(Id, SHM_INFO, -1), EFAULT);
    LxtCheckErrnoFailure(LxtShmCtl(-1, SHM_LOCK, NULL), EINVAL);
    LxtCheckErrnoFailure(LxtShmCtl(-1, SHM_UNLOCK, NULL), EINVAL);

    //
    // Generate an ID that does not refer to a valid memory region and attempt
    // operations on the nonexistent region.
    //

    do
    {
        LxtCheckErrno(LxtGetrandom(&RandomId, sizeof(RandomId), 0));
        Result = LxtShmCtl(RandomId, IPC_STAT, &Stat);
    } while ((Result == 0) && (errno != EINVAL));

    LxtCheckErrnoFailure(LxtShmCtl(RandomId, IPC_RMID, NULL), EINVAL);
    LxtCheckErrnoFailure(LxtShmCtl(RandomId, IPC_STAT, &Stat), EINVAL);
    LxtCheckErrnoFailure(LxtShmCtl(RandomId, IPC_SET, &Stat), EINVAL);
    LxtCheckErrnoFailure(LxtShmCtl(RandomId, SHM_LOCK, NULL), EINVAL);
    LxtCheckErrnoFailure(LxtShmCtl(RandomId, SHM_UNLOCK, NULL), EINVAL);

ErrorExit:
    if (ChildPid == 0)
    {
        _exit(Result);
    }

    if (Id != -1)
    {
        LxtShmCtl(Id, IPC_RMID, NULL);
    }

    return Result;
}

int ShmPidNamespaceWork(void)

/*++

Routine Description:

    This routine tests the behavior of System V shared memory across IPC
    namespaces. A child threadgroup is forked into a new IPC namespace,


    and the parent and
    child communicate across a unix socket connection. Each side queries the
    credentials of the other side via SO_PEERCRED and ancillary messages and
    validates that the appropriate credentials are returned.

Arguments:

    None.

Return Value:

    0 on success, -1 on failure.

--*/

{

    void* Address;
    void* Address2;
    pid_t ChildPid = 0;
    int Id;
    void* MapResult;
    pid_t ParentPid;
    struct shmid_ds ParentStat;
    int Result;
    struct shmid_ds Stat;
    int Status;

    Address = NULL;
    Address2 = NULL;
    Id = -1;

    LXT_SYNCHRONIZATION_POINT_START();

    //
    // Create and map a shared memory region.
    //

    LxtCheckErrno(Id = LxtShmGet(IPC_PRIVATE, PAGE_SIZE, 0));
    LxtCheckMapErrno(Address = LxtShmAt(Id, NULL, 0));
    LxtCheckErrno(LxtShmCtl(Id, IPC_STAT, &ParentStat));

    //
    // Unshare the PID namespace used for children.
    //

    LxtLogInfo("Unsharing CLONE_NEWPID");
    LxtCheckErrno(unshare(CLONE_NEWPID));

    //
    // Fork a child that will exist in a new IPC namespace.
    //

    ParentPid = getpid();
    LxtLogInfo("ParentPid %d", ParentPid);
    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {

        LxtLogInfo("Child's view of ChildPid %d", getpid());

        //
        // Attach the shared segment.
        //

        LxtCheckMapErrno(Address2 = LxtShmAt(Id, NULL, 0));
        LxtCheckErrno(LxtShmCtl(Id, IPC_STAT, &Stat));
        LxtCheckEqual(getpid(), Stat.shm_lpid, "%d");

        LXT_SYNCHRONIZATION_POINT();

        //
        // Wait for the parent to query credentials.
        //

        LXT_SYNCHRONIZATION_POINT();

        LxtCheckErrno(LxtShmCtl(Id, IPC_STAT, &Stat));
        LxtCheckEqual(ParentPid, Stat.shm_lpid, "%d");
    }
    else
    {

        LxtLogInfo("Parent's view of ChildPid %d", ChildPid);

        //
        // Wait for the child to attach.
        //

        LXT_SYNCHRONIZATION_POINT();

        //
        // Query the last attach pid (should NOT match ChildPid) and create
        // a new mapping.
        //

        LxtCheckErrno(LxtShmCtl(Id, IPC_STAT, &Stat));
        LxtCheckNotEqual(ChildPid, Stat.shm_lpid, "%d");
        LxtCheckMapErrno(Address2 = LxtShmAt(Id, NULL, 0));
        LxtCheckErrno(LxtShmCtl(Id, IPC_STAT, &Stat));
        LxtCheckEqual(getpid(), Stat.shm_lpid, "%d");

        LXT_SYNCHRONIZATION_POINT();
    }

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    LXT_SYNCHRONIZATION_POINT_END();
    if (Address != NULL)
    {
        LxtShmDt(Address);
    }

    if (Address2 != NULL)
    {
        LxtShmDt(Address2);
    }

    if (ChildPid == 0)
    {
        _exit(Result);
    }

    if (Id != -1)
    {
        LxtShmCtl(Id, IPC_RMID, NULL);
    }

    return Result;
}

int ShmPidNamespace(PLXT_ARGS Args)

/*++

Routine Description:

    This routine tests the behavior of System V shared memory across IPC
    namespaces.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    0 on success, -1 on failure.

--*/

{

    pid_t ChildPid;
    int Result;

    //
    // Fork into a new parent so that the existing threadgroup does not have its
    // IPC namespaces altered for later tests.
    //

    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        _exit(ShmPidNamespaceWork());
    }

    LxtCheckResult(LxtWaitPidPoll(ChildPid, 0));
    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    return Result;
}

void ShmPrintInfo(struct shmid_ds* Stat)

{

    if (g_VerboseShm == false)
    {
        return;
    }

    LxtLogInfo("shm_perm.__key %u", Stat->shm_perm.__key);
    LxtLogInfo("shm_perm.uid %u", Stat->shm_perm.uid);
    LxtLogInfo("shm_perm.gid %u", Stat->shm_perm.gid);
    LxtLogInfo("shm_perm.cuid %u", Stat->shm_perm.cuid);
    LxtLogInfo("shm_perm.cgid %u", Stat->shm_perm.cgid);
    LxtLogInfo("shm_perm.mode %o", Stat->shm_perm.mode);
    LxtLogInfo("shm_perm.__seq %d", Stat->shm_perm.__seq);
    LxtLogInfo("shm_segsz %Iu", Stat->shm_segsz);
    LxtLogInfo("shm_atime %Iu", Stat->shm_atime);
    LxtLogInfo("shm_dtime %Iu", Stat->shm_dtime);
    LxtLogInfo("shm_ctime %Iu", Stat->shm_ctime);
    LxtLogInfo("shm_cpid %Iu", Stat->shm_cpid);
    LxtLogInfo("shm_lpid %Iu", Stat->shm_lpid);
    LxtLogInfo("shm_nattch %Iu", Stat->shm_nattch);
    return;
}

void ShmPrintInfoAttach(struct shmid_ds* Stat)

{

    if (g_VerboseShm == false)
    {
        return;
    }

    LxtLogInfo("shm_segsz %Iu", Stat->shm_segsz);
    LxtLogInfo("shm_atime %Iu", Stat->shm_atime);
    LxtLogInfo("shm_dtime %Iu", Stat->shm_dtime);
    LxtLogInfo("shm_ctime %Iu", Stat->shm_ctime);
    LxtLogInfo("shm_cpid %Iu", Stat->shm_cpid);
    LxtLogInfo("shm_lpid %Iu", Stat->shm_lpid);
    LxtLogInfo("shm_nattch %Iu", Stat->shm_nattch);
    return;
}
