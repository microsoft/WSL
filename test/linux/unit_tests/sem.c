/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    sem.c

Abstract:

    This file is a test for the system V semaphore family of system calls.

--*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/eventfd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/sem.h>
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

#define LXT_NAME "sem"

#define SEM_ACCESS_UID 1004
#define SEM_ACCESS_GID 1004
#define SEM_COUNT (10)

//
// Globals.
//

bool g_VerboseSem = true;

int SemCtlSyscall(PLXT_ARGS Args);

int SemGetSyscall(PLXT_ARGS Args);

int SemOpFlags(PLXT_ARGS Args);

int SemOpSyscall(PLXT_ARGS Args);

void SemPrintInfo(struct semid_ds* Stat);

static const LXT_VARIATION g_LxtVariations[] = {
    {"semget syscall", SemGetSyscall}, {"semctl syscall", SemCtlSyscall}, {"semop syscall", SemOpSyscall}, {"semop flags", SemOpFlags}};

int SemTestEntry(int Argc, char* Argv[])
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

int SemCtlSyscall(PLXT_ARGS Args)

{

    struct __user_cap_data_struct CapData[2];
    struct __user_cap_header_struct CapHeader;
    int ChildPid;
    gid_t Gid;
    int Id;
    int Index;
    struct semid_ds OldStat;
    int Result;
    struct seminfo SemInfo;
    struct semid_ds Stat;
    uid_t Uid;
    unsigned short Values[SEM_COUNT];

    ChildPid = -1;
    Uid = getuid();
    Gid = getgid();
    LxtCheckErrno(Id = LxtSemGet(IPC_PRIVATE, SEM_COUNT, (IPC_CREAT | IPC_EXCL)));
    LxtCheckErrno(LxtSemCtl(Id, 0, SEM_STAT, &Stat));
    LxtCheckEqual(SEM_COUNT, Stat.sem_nsems, "%Iu");
    LxtCheckEqual(Uid, Stat.sem_perm.uid, "%d");
    LxtCheckEqual(Gid, Stat.sem_perm.gid, "%d");
    LxtCheckEqual(Uid, Stat.sem_perm.cuid, "%d");
    LxtCheckEqual(Gid, Stat.sem_perm.cgid, "%d");
    LxtCheckNotEqual(0, Stat.sem_ctime, "%Iu");
    LxtCheckEqual(0, Stat.sem_otime, "%Iu");

    LxtCheckErrno(LxtSemCtl(Id, 0, IPC_STAT, &Stat));
    LxtCheckErrno(LxtSemCtl(Id, 0, IPC_SET, &Stat));
    LxtCheckErrno(LxtSemCtl(Id, 0, IPC_INFO, &SemInfo));
    LxtCheckErrno(LxtSemCtl(Id, 0, IPC_INFO, &SemInfo));
    LxtCheckErrno(LxtSemCtl(0, 0, IPC_INFO, &SemInfo));
    LxtCheckErrno(LxtSemCtl(1, 0, IPC_INFO, &SemInfo));
    LxtCheckEqual(0, LxtSemCtl(Id, 0, GETPID, NULL), "%d");
    LxtCheckErrnoFailure(LxtSemCtl(Id, SEM_COUNT, GETPID, NULL), EINVAL);
    LxtCheckEqual(0, LxtSemCtl(Id, 0, GETVAL, NULL), "%d");
    LxtCheckErrnoFailure(LxtSemCtl(Id, SEM_COUNT, GETVAL, NULL), EINVAL);
    memset(&Values, 0, sizeof(Values));
    LxtCheckErrno(LxtSemCtl(Id, 0, GETALL, &Values));
    for (Index = 0; Index < SEM_COUNT; Index += 1)
    {
        LxtCheckEqual(Values[Index], LxtSemCtl(Id, Index, GETVAL, NULL), "%d");
    }

    LxtCheckEqual(0, LxtSemCtl(Id, 0, GETNCNT, NULL), "%d");
    LxtCheckEqual(0, LxtSemCtl(Id, 0, GETZCNT, NULL), "%d");

    //
    // Check GETPID and GETVAL again after doing a setval on a single semaphore.
    //

    Values[0] = 1;
    LxtCheckErrno(LxtSemCtl(Id, 0, SETVAL, Values[0]));
    LxtCheckErrnoFailure(LxtSemCtl(Id, SEM_COUNT, SETVAL, Values[0]), EINVAL);
    LxtCheckEqual(getpid(), LxtSemCtl(Id, 0, GETPID, NULL), "%d");
    LxtCheckEqual(Values[0], LxtSemCtl(Id, 0, GETVAL, NULL), "%d");
    LxtCheckErrno(LxtSemCtl(Id, 0, GETALL, &Values));
    for (Index = 0; Index < SEM_COUNT; Index += 1)
    {
        LxtCheckEqual(Values[Index], LxtSemCtl(Id, Index, GETVAL, NULL), "%d");
    }

    //
    // Verify the pid and value of the other semaphores has not changed.
    //

    for (Index = 1; Index < SEM_COUNT; Index += 1)
    {
        LxtCheckEqual(0, LxtSemCtl(Id, Index, GETPID, NULL), "%d");
        LxtCheckEqual(0, LxtSemCtl(Id, Index, GETVAL, NULL), "%d");
    }

    //
    // SETALL command.
    //

    for (Index = 0; Index < SEM_COUNT; Index += 1)
    {
        Values[0] = Index;
    }

    //
    // Ensure that each semaphore's value has been updated. Interestingly the
    // last pid value is not updated by the SETALL command.
    //

    LxtCheckErrno(LxtSemCtl(Id, 0, SETALL, &Values));
    for (Index = 0; Index < SEM_COUNT; Index += 1)
    {
        if (Index == 0)
        {
            LxtCheckEqual(getpid(), LxtSemCtl(Id, Index, GETPID, NULL), "%d");
        }
        else
        {
            LxtCheckEqual(0, LxtSemCtl(Id, Index, GETPID, NULL), "%d");
        }

        LxtCheckEqual(Values[Index], LxtSemCtl(Id, Index, GETVAL, NULL), "%d");
    }

    memset(Values, 0, sizeof(Values));
    Values[1] = -1;
    LxtCheckErrnoFailure(LxtSemCtl(Id, 0, SETALL, &Values), ERANGE);

    //
    // Create a child without the CAP_IPC_OWNER capability.
    //

    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {

        //
        // Drop the CAP_IPC_OWNER capability.
        //

        memset(&CapHeader, 0, sizeof(CapHeader));
        CapHeader.version = _LINUX_CAPABILITY_VERSION_3;
        LxtCheckErrno(LxtCapGet(&CapHeader, CapData)) LxtCheckErrno(prctl(PR_SET_KEEPCAPS, 1));
        CapData[CAP_TO_INDEX(CAP_IPC_OWNER)].permitted &= ~CAP_TO_MASK(CAP_IPC_OWNER);
        CapData[0].effective = CapData[0].permitted;
        CapData[1].effective = CapData[1].permitted;
        LxtCheckErrno(LxtCapSet(&CapHeader, CapData));

        //
        // Verify commands that requires the IPC_OWNER capability now fail.
        //

        LxtCheckErrnoFailure(LxtSemCtl(Id, 0, SEM_STAT, &Stat), EACCES);
        LxtCheckErrnoFailure(LxtSemCtl(Id, 0, IPC_STAT, &Stat), EACCES);

        //
        // Change the UID and verify commands fail.
        //

        LxtCheckErrno(setuid(SEM_ACCESS_UID));
        LxtCheckErrnoFailure(LxtSemCtl(Id, 0, IPC_SET, &Stat), EPERM);
        LxtCheckErrnoFailure(LxtSemCtl(Id, 0, IPC_RMID, NULL), EPERM);

        LxtCheckErrnoFailure(LxtSemCtl(Id, 0, IPC_STAT, &Stat), EACCES);
        LxtCheckErrnoFailure(LxtSemCtl(Id, 0, SEM_STAT, &Stat), EACCES);

        LxtCheckErrno(LxtSemCtl(Id, 0, IPC_INFO, &SemInfo));
        LxtCheckErrno(LxtSemCtl(0, 0, IPC_INFO, &SemInfo));
        Result = LXT_RESULT_SUCCESS;
        goto ErrorExit;
    }

    //
    // Wait for the child to exit.
    //

    LxtCheckErrno(LxtWaitPidPoll(ChildPid, LXT_RESULT_SUCCESS));

    //
    // Invalid parameter variations.
    //

    //
    // Ensure IPC_SET cannot set invalid mode bits (they are silently ignored).
    //

    LxtCheckErrno(LxtSemCtl(Id, 0, IPC_STAT, &Stat));
    Stat.sem_perm.mode = -1;
    LxtCheckErrno(LxtSemCtl(Id, 0, IPC_SET, &Stat));
    LxtCheckErrno(LxtSemCtl(Id, 0, IPC_STAT, &Stat));
    LxtCheckEqual(Stat.sem_perm.mode, 0777, "%o");

    //
    // Ensure the uid and gid cannot be set to -1.
    //

    LxtCheckErrno(LxtSemCtl(Id, 0, IPC_STAT, &OldStat));
    Stat = OldStat;
    Stat.sem_perm.uid = -1;
    LxtCheckErrnoFailure(LxtSemCtl(Id, 0, IPC_SET, &Stat), EINVAL);
    Stat = OldStat;
    Stat.sem_perm.gid = -1;
    LxtCheckErrnoFailure(LxtSemCtl(Id, 0, IPC_SET, &Stat), EINVAL);
    LxtCheckErrno(LxtSemCtl(Id, 0, IPC_STAT, &Stat));
    LxtCheckEqual(Stat.sem_perm.uid, OldStat.sem_perm.uid, "%d");
    LxtCheckEqual(Stat.sem_perm.gid, OldStat.sem_perm.gid, "%d");

    LxtCheckErrnoFailure(LxtSemCtl(Id, -1, GETPID, NULL), EINVAL);
    LxtCheckErrnoFailure(LxtSemCtl(Id, SEM_COUNT, GETPID, NULL), EINVAL);
    LxtCheckErrnoFailure(LxtSemCtl(Id, -1, GETVAL, NULL), EINVAL);
    LxtCheckErrnoFailure(LxtSemCtl(Id, SEM_COUNT, GETVAL, NULL), EINVAL);
    LxtCheckErrnoFailure(LxtSemCtl(Id, -1, SETVAL, 0), EINVAL);
    LxtCheckErrnoFailure(LxtSemCtl(Id, SEM_COUNT, SETVAL, 0), EINVAL);
    LxtCheckErrnoFailure(LxtSemCtl(Id, -1, GETNCNT, NULL), EINVAL);
    LxtCheckErrnoFailure(LxtSemCtl(Id, SEM_COUNT, GETNCNT, NULL), EINVAL);
    LxtCheckErrnoFailure(LxtSemCtl(Id, -1, GETZCNT, NULL), EINVAL);
    LxtCheckErrnoFailure(LxtSemCtl(Id, SEM_COUNT, GETZCNT, NULL), EINVAL);

    LxtCheckErrnoFailure(LxtSemCtl(-1, 0, SEM_STAT, &Stat), EINVAL);
    LxtCheckErrnoFailure(LxtSemCtl(-1, 0, IPC_STAT, &Stat), EINVAL);
    LxtCheckErrnoFailure(LxtSemCtl(-1, 0, IPC_SET, &Stat), EINVAL);
    LxtCheckErrnoFailure(LxtSemCtl(Id, 0, IPC_INFO, NULL), EFAULT);
    LxtCheckErrnoFailure(LxtSemCtl(Id, 0, IPC_INFO, -1), EFAULT);
    LxtCheckErrnoFailure(LxtSemCtl(0, 0, IPC_INFO, NULL), EFAULT);
    LxtCheckErrnoFailure(LxtSemCtl(0, 0, IPC_INFO, -1), EFAULT);
    LxtCheckErrnoFailure(LxtSemCtl(-1, 0, GETPID, NULL), EINVAL);
    LxtCheckErrnoFailure(LxtSemCtl(Id, SEM_COUNT, GETPID, NULL), EINVAL);
    LxtCheckErrnoFailure(LxtSemCtl(-1, 0, GETVAL, NULL), EINVAL);
    LxtCheckErrnoFailure(LxtSemCtl(Id, SEM_COUNT, GETVAL, NULL), EINVAL);
    LxtCheckErrnoFailure(LxtSemCtl(Id, 0, GETALL, NULL), EFAULT);
    LxtCheckErrnoFailure(LxtSemCtl(Id, 0, GETALL, -1), EFAULT);
    LxtCheckErrnoFailure(LxtSemCtl(-1, 0, GETNCNT, NULL), EINVAL);
    LxtCheckErrnoFailure(LxtSemCtl(-1, 0, GETZCNT, NULL), EINVAL);
    LxtCheckErrnoFailure(LxtSemCtl(Id, 0, SETALL, NULL), EFAULT);
    LxtCheckErrnoFailure(LxtSemCtl(Id, 0, SETALL, -1), EFAULT);

ErrorExit:
    if (ChildPid == 0)
    {
        _exit(Result);
    }

    if (Id != -1)
    {
        LxtSemCtl(Id, 0, IPC_RMID, NULL);
    }

    return Result;
}

int SemGetSyscall(PLXT_ARGS Args)

{

    struct __user_cap_data_struct CapData[2];
    struct __user_cap_header_struct CapHeader;
    int ChildPid;
    int Id;
    key_t Key;
    int Mode;
    size_t Result;
    struct semid_ds Stat;
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
    LxtCheckErrno(Id = LxtSemGet(Key, SEM_COUNT, (IPC_CREAT | IPC_EXCL | Mode)));
    LxtLogInfo("Id = %d", Id);
    LxtCheckErrno(LxtSemCtl(Id, 0, IPC_STAT, &Stat));
    SemPrintInfo(&Stat);
    LxtCheckEqual(Key, Stat.sem_perm.__key, "%Iu");
    LxtCheckEqual(SEM_COUNT, Stat.sem_nsems, "%Iu");
    LxtCheckEqual(0, Stat.sem_otime, "%Iu");
    LxtCheckNotEqual(0, Stat.sem_ctime, "%Iu");
    LxtCheckEqual(Mode, Stat.sem_perm.mode, "%o");
    LxtCheckEqual(getuid(), Stat.sem_perm.cuid, "%d");
    LxtCheckEqual(getuid(), Stat.sem_perm.uid, "%d");
    LxtCheckEqual(getgid(), Stat.sem_perm.cgid, "%d");
    LxtCheckEqual(getgid(), Stat.sem_perm.gid, "%d");

    //
    // semget with IPC_CREAT or IPC_EXCL when the region already exists.
    //

    LxtCheckEqual(Id, LxtSemGet(Key, SEM_COUNT, IPC_CREAT), "%Iu");
    LxtCheckEqual(Id, LxtSemGet(Key, SEM_COUNT, IPC_EXCL), "%Iu");
    LxtCheckEqual(Id, LxtSemGet(Key, SEM_COUNT, 0), "%Iu");

    //
    // semget with count = 0 should succeed.
    //

    LxtCheckEqual(Id, LxtSemGet(Key, 0, 0), "%Iu");

    //
    // Create a child with a different uid and gid that does not have the
    // IPC_OWNER capability.
    //

    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        LxtCheckErrno(prctl(PR_SET_KEEPCAPS, 1));
        LxtCheckErrno(setgid(SEM_ACCESS_GID));
        LxtCheckErrno(setuid(SEM_ACCESS_UID));
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

        LxtCheckEqual(Id, LxtSemGet(Key, SEM_COUNT, IPC_CREAT), "%Iu");
        LxtCheckEqual(Id, LxtSemGet(Key, SEM_COUNT, IPC_EXCL), "%Iu");
        LxtCheckEqual(Id, LxtSemGet(Key, SEM_COUNT, 0777), "%Iu");
        LxtCheckEqual(Id, LxtSemGet(Key, SEM_COUNT, 0666), "%Iu");
        LxtCheckEqual(Id, LxtSemGet(Key, SEM_COUNT, 0600), "%Iu");
        LxtCheckEqual(Id, LxtSemGet(Key, SEM_COUNT, 0060), "%Iu");
        LxtCheckEqual(Id, LxtSemGet(Key, SEM_COUNT, 0006), "%Iu");
        LxtCheckEqual(Id, LxtSemGet(Key, SEM_COUNT, 0), "%Iu");

        //
        // Drop all group membership and the CAP_IPC_OWNER capability and
        // attempt to call semget with unmatching mode bits.
        //

        LxtCheckErrno(Result = setgroups(0, NULL));
        memset(&CapData, 0, sizeof(CapData));
        memset(&CapHeader, 0, sizeof(CapHeader));
        CapHeader.version = _LINUX_CAPABILITY_VERSION_3;
        LxtCheckErrno(LxtCapSet(&CapHeader, CapData));
        LxtCheckErrnoFailure(LxtSemGet(Key, SEM_COUNT, 0777), EACCES);
        LxtCheckErrnoFailure(LxtSemGet(Key, SEM_COUNT, 0666), EACCES);
        LxtCheckErrnoFailure(LxtSemGet(Key, SEM_COUNT, 0600), EACCES);
        LxtCheckErrnoFailure(LxtSemGet(Key, SEM_COUNT, 0060), EACCES);
        LxtCheckErrnoFailure(LxtSemGet(Key, SEM_COUNT, 0006), EACCES);

        //
        // Use the same permission as before, these should succeed.
        //

        LxtCheckEqual(Id, LxtSemGet(Key, SEM_COUNT, IPC_CREAT), "%Iu");
        LxtCheckEqual(Id, LxtSemGet(Key, SEM_COUNT, IPC_EXCL), "%Iu");
        LxtCheckEqual(Id, LxtSemGet(Key, SEM_COUNT, 0), "%Iu");
        goto ErrorExit;
    }

    //
    // Wait for the child to exit.
    //

    LxtCheckErrno(LxtWaitPidPoll(ChildPid, LXT_RESULT_SUCCESS));

    //
    // Invalid parameter variations.
    //

    //
    // semget with IPC_CREAT | IPC_EXCL when the region already exists, should
    // succeed with only IPC_EXCL.
    //

    LxtCheckErrnoFailure(LxtSemGet(Key, SEM_COUNT, (IPC_CREAT | IPC_EXCL)), EEXIST);

    //
    // semget with a known key and a size that does not match.
    //

    LxtCheckErrnoFailure(LxtSemGet(Key, (SEM_COUNT * 2), 0), EINVAL);
    LxtCheckErrnoFailure(LxtSemGet(Key, SEM_COUNT + 1, 0), EINVAL);

    //
    // N.B. There appears to be no error checking for invalid flags, only the
    //      presence of valid flags.
    //
    // -1 includes the IPC_EXCL flag so this should return EEXIST.
    //

    LxtCheckErrnoFailure(LxtSemGet(Key, SEM_COUNT, -1), EEXIST);
    LxtCheckEqual(Id, LxtSemGet(Key, SEM_COUNT, (-1 & ~IPC_EXCL)), "%Iu");

    //
    // Delete the region and create a new one with a size of one byte.
    //

    LxtCheckErrno(LxtSemCtl(Id, 0, IPC_RMID, &Stat));
    LxtCheckErrnoFailure(LxtSemCtl(Id, 0, IPC_RMID, NULL), EINVAL);
    Id = -1;
    LxtCheckErrno(Id = LxtSemGet(IPC_PRIVATE, 1, 0));
    LxtCheckErrno(LxtSemCtl(Id, 0, IPC_STAT, &Stat));
    LxtCheckEqual(1, Stat.sem_nsems, "%Iu");

    //
    // Delete the region and create a new region with a size of zero bytes
    // (should fail).
    //

    LxtCheckErrno(LxtSemCtl(Id, 0, IPC_RMID, &Stat));
    Id = -1;
    LxtCheckErrnoFailure(Id = LxtSemGet(IPC_PRIVATE, 0, 0), EINVAL);

ErrorExit:
    if (ChildPid == 0)
    {
        _exit(Result);
    }

    if (Id != -1)
    {
        LxtSemCtl(Id, 0, IPC_RMID, NULL);
    }

    return Result;
}

int SemCloneChild(void* Param)
{

    int Id;
    int Result;

    Id = *((int*)Param);
    LxtCheckEqual(1, LxtSemCtl(Id, 0, GETVAL, NULL), "%d");
    LxtCheckEqual(0, LxtSemCtl(Id, 1, GETVAL, NULL), "%d");
    LxtCheckErrno(unshare(CLONE_SYSVSEM));

    //
    // Verify the values did not change.
    //

    LxtCheckEqual(1, LxtSemCtl(Id, 0, GETVAL, NULL), "%d");
    LxtCheckEqual(0, LxtSemCtl(Id, 1, GETVAL, NULL), "%d");
    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    exit(Result);
}

int SemCloneThread(void* Param)
{

    long long Data;
    int Event;
    int Result;

    Event = *((int*)Param);
    LxtCheckErrno(read(Event, &Data, sizeof(Data)));

    //
    // Just exit the thread, not the thread group, on success.
    //

    syscall(SYS_exit, 0);

ErrorExit:
    exit(Result);
}

int SemOpFlags(PLXT_ARGS Args)
{
    int ChildPid;
    LXT_CLONE_ARGS CloneArgs;
    long long EventData;
    int Flags;
    int Id;
    struct sembuf Operations[SEM_COUNT];
    size_t Result;
    char* SharedStack;
    int SharedEvent;
    pid_t SharedTid;
    struct semid_ds Stat;
    int StackSize;
    int Status;
    char* UnsharedStack;
    int UnsharedEvent;
    pid_t UnsharedTid;
    unsigned short Values[SEM_COUNT];

    ChildPid = -1;
    EventData = 1;
    Id = -1;
    SharedEvent = -1;
    SharedStack = NULL;
    UnsharedEvent = -1;
    UnsharedStack = NULL;
    memset(Operations, 0, sizeof(Operations));

    LXT_SYNCHRONIZATION_POINT_START();

    //
    // Create a semaphore set.
    //

    LxtCheckErrno(Id = LxtSemGet(IPC_PRIVATE, SEM_COUNT, (IPC_CREAT | IPC_EXCL)));

    //
    // Test the nowait flag.
    //

    Operations[0].sem_num = 0;
    Operations[0].sem_op = -1;
    Operations[0].sem_flg = IPC_NOWAIT;
    LxtCheckErrnoFailure(LxtSemOp(Id, Operations, 1), EAGAIN);

    //
    // Increment the first semaphore.
    //

    Operations[0].sem_num = 0;
    Operations[0].sem_op = 1;
    Operations[0].sem_flg = 0;
    LxtCheckErrno(LxtSemOp(Id, Operations, 1));

    //
    // Create a child.
    //

    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {

        //
        // Decrement the first semaphore and increment the second semaphore,
        // both with the undo flag set.
        //

        Operations[0].sem_num = 0;
        Operations[0].sem_op = -1;
        Operations[0].sem_flg = SEM_UNDO;
        Operations[1].sem_num = 1;
        Operations[1].sem_op = 1;
        Operations[1].sem_flg = SEM_UNDO;
        LxtCheckErrno(LxtSemOp(Id, Operations, 2));
        goto ErrorExit;
    }

    //
    // Wait for the child to exit.
    //

    LxtCheckErrno(LxtWaitPidPoll(ChildPid, LXT_RESULT_SUCCESS));

    //
    // Ensure the child's operations were undone.
    //

    LxtCheckEqual(1, LxtSemCtl(Id, 0, GETVAL, NULL), "%d");
    LxtCheckEqual(0, LxtSemCtl(Id, 1, GETVAL, NULL), "%d");

    //
    // Ensure the wait can still be satisfied.
    //

    Operations[0].sem_num = 0;
    Operations[0].sem_op = -1;
    Operations[0].sem_flg = 0;
    LxtCheckErrno(LxtSemOp(Id, Operations, 1));
    LxtCheckEqual(0, LxtSemCtl(Id, 0, GETVAL, NULL), "%d");

    //
    // Create a child.
    //

    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {

        //
        // Set the first semaphore to the max with the undo flag specified and
        // lower the count without the undo flag specified.
        //

        Operations[0].sem_num = 0;
        Operations[0].sem_op = 0x7fff;
        Operations[0].sem_flg = SEM_UNDO;
        Operations[1].sem_num = 0;
        Operations[1].sem_op = -0x7fff;
        Operations[1].sem_flg = 0;
        LxtCheckErrno(LxtSemOp(Id, Operations, 2));
        LxtCheckEqual(0, LxtSemCtl(Id, 0, GETVAL, NULL), "%d");

        LXT_SYNCHRONIZATION_POINT();
        LXT_SYNCHRONIZATION_POINT();

        Operations[0].sem_num = 0;
        Operations[0].sem_op = 1;
        Operations[0].sem_flg = 0;
        LxtCheckErrno(LxtSemOp(Id, Operations, 1));
        LxtCheckEqual(1, LxtSemCtl(Id, 0, GETVAL, NULL), "%d");
        LXT_SYNCHRONIZATION_POINT();
        LXT_SYNCHRONIZATION_POINT();
        goto ErrorExit;
    }

    //
    // Wait for child to perform first operation.
    //

    LXT_SYNCHRONIZATION_POINT();
    LxtCheckEqual(0, LxtSemCtl(Id, 0, GETVAL, NULL), "%d");
    LXT_SYNCHRONIZATION_POINT();

    //
    // Wait for child to perform second operation.
    //

    LXT_SYNCHRONIZATION_POINT();
    LxtCheckEqual(1, LxtSemCtl(Id, 0, GETVAL, NULL), "%d");
    LXT_SYNCHRONIZATION_POINT();

    //
    // Wait for the child to exit and ensure the count does not drop below
    // zero.
    //

    LxtCheckErrno(LxtWaitPidPoll(ChildPid, LXT_RESULT_SUCCESS));
    LxtCheckEqual(0, LxtSemCtl(Id, 0, GETVAL, NULL), "%d");

    //
    // Create a child.
    //

    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {

        //
        // Set the first semaphore to the max without the undo flag specified and
        // lower the count with the undo flag specified.
        //

        Operations[0].sem_num = 0;
        Operations[0].sem_op = 0x7fff;
        Operations[0].sem_flg = 0;
        Operations[1].sem_num = 0;
        Operations[1].sem_op = -0x7fff;
        Operations[1].sem_flg = SEM_UNDO;
        LxtCheckErrno(LxtSemOp(Id, Operations, 2));
        LxtCheckEqual(0, LxtSemCtl(Id, 0, GETVAL, NULL), "%d");

        LXT_SYNCHRONIZATION_POINT();
        LXT_SYNCHRONIZATION_POINT();

        Operations[0].sem_num = 0;
        Operations[0].sem_op = 1;
        Operations[0].sem_flg = 0;
        LxtCheckErrno(LxtSemOp(Id, Operations, 1));
        LxtCheckEqual(1, LxtSemCtl(Id, 0, GETVAL, NULL), "%d");
        LXT_SYNCHRONIZATION_POINT();
        LXT_SYNCHRONIZATION_POINT();
        goto ErrorExit;
    }

    //
    // Wait for child to perform first operation.
    //

    LXT_SYNCHRONIZATION_POINT();
    LxtCheckEqual(0, LxtSemCtl(Id, 0, GETVAL, NULL), "%d");
    LXT_SYNCHRONIZATION_POINT();

    //
    // Wait for child to perform second operation.
    //

    LXT_SYNCHRONIZATION_POINT();
    LxtCheckEqual(1, LxtSemCtl(Id, 0, GETVAL, NULL), "%d");
    LXT_SYNCHRONIZATION_POINT();

    //
    // Wait for the child to exit and ensure the count does not exceed the max
    // semaphore value.
    //

    LxtCheckErrno(LxtWaitPidPoll(ChildPid, LXT_RESULT_SUCCESS));
    LxtCheckEqual(0x7fff, LxtSemCtl(Id, 0, GETVAL, NULL), "%d");
    LxtCheckErrno(LxtSemCtl(Id, 0, SETVAL, 0));

    //
    // Validate semctl SETVAL clears undo adjustments.
    //

    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {

        //
        // Set the first semaphore to the max with the undo flag specified and
        // lower the count without the undo flag specified.
        //

        Operations[0].sem_num = 0;
        Operations[0].sem_op = 0x7fff;
        Operations[0].sem_flg = 0;
        Operations[1].sem_num = 0;
        Operations[1].sem_op = -0x7fff;
        Operations[1].sem_flg = SEM_UNDO;
        LxtCheckErrno(LxtSemOp(Id, Operations, 2));
        LxtCheckEqual(0, LxtSemCtl(Id, 0, GETVAL, NULL), "%d");

        LXT_SYNCHRONIZATION_POINT();
        LXT_SYNCHRONIZATION_POINT();

        Operations[0].sem_num = 0;
        Operations[0].sem_op = 1;
        Operations[0].sem_flg = 0;
        LxtCheckErrno(LxtSemOp(Id, Operations, 1));
        LxtCheckEqual(1, LxtSemCtl(Id, 0, GETVAL, NULL), "%d");
        LXT_SYNCHRONIZATION_POINT();
        LXT_SYNCHRONIZATION_POINT();
        goto ErrorExit;
    }

    //
    // Wait for child to perform first operation.
    //

    LXT_SYNCHRONIZATION_POINT();
    LxtCheckEqual(0, LxtSemCtl(Id, 0, GETVAL, NULL), "%d");
    LXT_SYNCHRONIZATION_POINT();

    //
    // Wait for child to perform second operation and set the semaphore value
    // to zero. This should remove the pending semaphore adjustment.
    //

    LXT_SYNCHRONIZATION_POINT();
    LxtCheckEqual(1, LxtSemCtl(Id, 0, GETVAL, NULL), "%d");
    LxtCheckErrno(LxtSemCtl(Id, 0, SETVAL, 0));
    LXT_SYNCHRONIZATION_POINT();

    //
    // Wait for the child to exit and ensure the adjustment was not applied.
    //

    LxtCheckErrno(LxtWaitPidPoll(ChildPid, LXT_RESULT_SUCCESS));
    LxtCheckEqual(0, LxtSemCtl(Id, 0, GETVAL, NULL), "%d");

    //
    // Create a child, verify when the child unshares the semaphore adjustments
    // are cleared.
    //

    memset(Values, 0, sizeof(Values));
    LxtCheckErrno(LxtSemCtl(Id, 0, SETALL, &Values));
    LxtCheckErrno(LxtSemCtl(Id, 1, SETVAL, 1));
    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {

        //
        // Increment one semaphore and decrement another both with the undo
        // flag set.
        //

        Operations[0].sem_num = 0;
        Operations[0].sem_op = 1;
        Operations[0].sem_flg = SEM_UNDO;
        Operations[1].sem_num = 1;
        Operations[1].sem_op = -1;
        Operations[1].sem_flg = SEM_UNDO;
        LxtCheckErrno(LxtSemOp(Id, Operations, 2));
        LxtCheckEqual(1, LxtSemCtl(Id, 0, GETVAL, NULL), "%d");
        LxtCheckEqual(0, LxtSemCtl(Id, 1, GETVAL, NULL), "%d");
        LxtCheckErrno(unshare(CLONE_SYSVSEM));

        //
        // Ensure the state was undone.
        //

        LxtCheckEqual(0, LxtSemCtl(Id, 0, GETVAL, NULL), "%d");
        LxtCheckEqual(1, LxtSemCtl(Id, 1, GETVAL, NULL), "%d");
        LXT_SYNCHRONIZATION_POINT();
        LXT_SYNCHRONIZATION_POINT();
        goto ErrorExit;
    }

    //
    // Wait for child to unshare.
    //

    LXT_SYNCHRONIZATION_POINT();

    //
    // Ensure the child's operations were undone.
    //

    LxtCheckEqual(0, LxtSemCtl(Id, 0, GETVAL, NULL), "%d");
    LxtCheckEqual(1, LxtSemCtl(Id, 1, GETVAL, NULL), "%d");
    LXT_SYNCHRONIZATION_POINT();

    //
    // Wait for the child to exit.
    //

    LxtCheckErrno(LxtWaitPidPoll(ChildPid, LXT_RESULT_SUCCESS));

    //
    // Reset semaphore state.
    //

    memset(Values, 0, sizeof(Values));
    LxtCheckErrno(LxtSemCtl(Id, 0, SETALL, &Values));
    LxtCheckErrno(LxtSemCtl(Id, 1, SETVAL, 1));
    Operations[0].sem_num = 0;
    Operations[0].sem_op = 1;
    Operations[0].sem_flg = SEM_UNDO;
    Operations[1].sem_num = 1;
    Operations[1].sem_op = -1;
    Operations[1].sem_flg = SEM_UNDO;
    LxtCheckErrno(LxtSemOp(Id, Operations, 2));

    //
    // Clone a child to share the same SystemV semaphore adjustment structure.
    //

    LxtCheckResult(LxtClone(SemCloneChild, &Id, CLONE_SYSVSEM | SIGCHLD, &CloneArgs));

    //
    // Wait for child to exit.
    //

    LxtCheckResult(LxtWaitPidPoll(CloneArgs.CloneId, 0));

    //
    // Values should not have changed yet.
    //

    LxtCheckEqual(1, LxtSemCtl(Id, 0, GETVAL, NULL), "%d");
    LxtCheckEqual(0, LxtSemCtl(Id, 1, GETVAL, NULL), "%d");

    //
    // Create two threads, one sharing the semaphore adjustment structure
    // and one not.
    //

    Flags = CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD | CLONE_PARENT_SETTID | CLONE_CHILD_CLEARTID;

    StackSize = 1024 * 1024;

    LxtCheckErrno(SharedEvent = eventfd(0, EFD_SEMAPHORE));
    SharedStack = malloc(StackSize);
    LxtCheckResult(clone(SemCloneThread, SharedStack + StackSize, Flags | CLONE_SYSVSEM, &SharedEvent, &SharedTid, NULL, &SharedTid));

    LxtCheckErrno(UnsharedEvent = eventfd(0, EFD_SEMAPHORE));
    UnsharedStack = malloc(StackSize);
    LxtCheckResult(clone(SemCloneThread, UnsharedStack + StackSize, Flags, &UnsharedEvent, &UnsharedTid, NULL, &UnsharedTid));

    //
    // Unshare; since there is still a thread sharing, adjustments should
    // not occur.
    //

    LxtCheckErrno(unshare(CLONE_SYSVSEM));
    LxtCheckEqual(1, LxtSemCtl(Id, 0, GETVAL, NULL), "%d");
    LxtCheckEqual(0, LxtSemCtl(Id, 1, GETVAL, NULL), "%d");

    //
    // Signal the sharing thread and wait for it to exit; adjustments should
    // occur shortly thereafter.
    //

    LxtCheckErrno(write(SharedEvent, &EventData, sizeof(EventData)));
    LxtCheckErrno(LxtJoinThread(&SharedTid));
    usleep(100000);
    LxtCheckEqual(0, LxtSemCtl(Id, 0, GETVAL, NULL), "%d");
    LxtCheckEqual(1, LxtSemCtl(Id, 1, GETVAL, NULL), "%d");

    //
    // Signal the unshared thread to clean things up.
    //

    LxtCheckErrno(write(UnsharedEvent, &EventData, sizeof(EventData)));
    LxtCheckErrno(LxtJoinThread(&UnsharedTid));

ErrorExit:
    LXT_SYNCHRONIZATION_POINT_END();
    if (ChildPid == 0)
    {
        _exit(Result);
    }

    if (Id != -1)
    {
        LxtSemCtl(Id, 0, IPC_RMID, NULL);
    }

    if (SharedEvent != -1)
    {
        close(SharedEvent);
    }

    if (UnsharedEvent != -1)
    {
        close(SharedEvent);
    }

    free(SharedStack);
    free(UnsharedStack);
    return Result;
}

int SemOpSyscall(PLXT_ARGS Args)

{
    struct __user_cap_data_struct CapData[2];
    struct __user_cap_header_struct CapHeader;
    int ChildPid;
    int Id;
    int Index;
    int Mode;
    struct sembuf Operations[SEM_COUNT];
    size_t Result;
    struct semid_ds Stat;
    int Status;
    time_t Time;
    struct timespec Timeout;
    unsigned short Values[SEM_COUNT];

    ChildPid = -1;
    Id = -1;
    memset(Operations, 0, sizeof(Operations));
    memset(Values, 0, sizeof(Values));

    LXT_SYNCHRONIZATION_POINT_START();

    //
    // Create a semaphore with zero mode bits.
    //

    Mode = 0000;
    LxtLogInfo("Mode %o", Mode);
    LxtCheckErrno(Id = LxtSemGet(IPC_PRIVATE, SEM_COUNT, (IPC_CREAT | IPC_EXCL | Mode)));
    LxtLogInfo("Id = %d", Id);

    //
    // Create a child with a different uid and gid that does not have the
    // IPC_OWNER capability.
    //

    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        LxtCheckErrno(prctl(PR_SET_KEEPCAPS, 1));
        LxtCheckErrno(setgid(SEM_ACCESS_GID));
        LxtCheckErrno(setuid(SEM_ACCESS_UID));
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

        memset(Operations, 0, sizeof(Operations));
        LxtCheckErrno(LxtSemOp(Id, Operations, SEM_COUNT));

        //
        // Drop all group membership and the CAP_IPC_OWNER capability and
        // attempt to call semget with unmatching mode bits.
        //

        LxtCheckErrno(Result = setgroups(0, NULL));
        memset(&CapData, 0, sizeof(CapData));
        memset(&CapHeader, 0, sizeof(CapHeader));
        CapHeader.version = _LINUX_CAPABILITY_VERSION_3;
        LxtCheckErrno(LxtCapSet(&CapHeader, CapData));

        //
        // Attempt to issue operations, these should fail.
        //

        LxtCheckErrnoFailure(LxtSemOp(Id, Operations, SEM_COUNT), EACCES);
        goto ErrorExit;
    }

    //
    // Wait for the child to exit.
    //

    LxtCheckErrno(LxtWaitPidPoll(ChildPid, LXT_RESULT_SUCCESS));

    //
    // Create a new readable semaphore.
    //

    LxtCheckErrno(LxtSemCtl(Id, 0, IPC_RMID, NULL));
    Mode = 0004;
    LxtLogInfo("Mode %o", Mode);
    LxtCheckErrno(Id = LxtSemGet(IPC_PRIVATE, SEM_COUNT, (IPC_CREAT | IPC_EXCL | Mode)));
    LxtLogInfo("Id = %d", Id);

    //
    // Create a child with a different uid and gid that does not have the
    // IPC_OWNER capability.
    //

    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        LxtCheckErrno(prctl(PR_SET_KEEPCAPS, 1));
        LxtCheckErrno(setgid(SEM_ACCESS_GID));
        LxtCheckErrno(setuid(SEM_ACCESS_UID));
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

        memset(Operations, 0, sizeof(Operations));
        LxtCheckErrno(LxtSemOp(Id, Operations, SEM_COUNT));

        //
        // Drop all group membership and the CAP_IPC_OWNER capability and
        // attempt to call semget with unmatching mode bits.
        //

        LxtCheckErrno(Result = setgroups(0, NULL));
        memset(&CapData, 0, sizeof(CapData));
        memset(&CapHeader, 0, sizeof(CapHeader));
        CapHeader.version = _LINUX_CAPABILITY_VERSION_3;
        LxtCheckErrno(LxtCapSet(&CapHeader, CapData));

        //
        // Attempt to issue a "wait for zero" operation, this should succeed
        // and return immediately because the value is zero.
        //

        LxtCheckErrno(LxtSemOp(Id, Operations, SEM_COUNT));

        //
        // Attempt to increment the semaphore, this should fail.
        //

        Operations[1].sem_num = 0;
        Operations[1].sem_op = 1;
        LxtCheckErrnoFailure(LxtSemOp(Id, &Operations[1], 1), EACCES);

        //
        // Attempt to decrement the semaphore, this should fail.
        //

        Operations[2].sem_num = 0;
        Operations[2].sem_op = -1;
        LxtCheckErrnoFailure(LxtSemOp(Id, &Operations[2], 1), EACCES);

        //
        // Attempt the increment and wait operations after a wait for zero that
        // succeeds.
        //

        LxtCheckErrnoFailure(LxtSemOp(Id, Operations, 3), EACCES);
        goto ErrorExit;
    }

    //
    // Wait for the child to exit.
    //

    LxtCheckErrno(LxtWaitPidPoll(ChildPid, LXT_RESULT_SUCCESS));

    //
    // Create a new writable semaphore.
    //

    LxtCheckErrno(LxtSemCtl(Id, 0, IPC_RMID, NULL));
    Mode = 0002;
    LxtLogInfo("Mode %o", Mode);
    LxtCheckErrno(Id = LxtSemGet(IPC_PRIVATE, SEM_COUNT, (IPC_CREAT | IPC_EXCL | Mode)));
    LxtLogInfo("Id = %d", Id);

    //
    // Create a child with a different uid and gid that does not have the
    // IPC_OWNER capability.
    //

    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        LxtCheckErrno(prctl(PR_SET_KEEPCAPS, 1));
        LxtCheckErrno(setgid(SEM_ACCESS_GID));
        LxtCheckErrno(setuid(SEM_ACCESS_UID));
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

        memset(Operations, 0, sizeof(Operations));
        LxtCheckErrno(LxtSemOp(Id, Operations, SEM_COUNT));

        //
        // Drop all group membership and the CAP_IPC_OWNER capability and
        // attempt to call semget with unmatching mode bits.
        //

        LxtCheckErrno(Result = setgroups(0, NULL));
        memset(&CapData, 0, sizeof(CapData));
        memset(&CapHeader, 0, sizeof(CapHeader));
        CapHeader.version = _LINUX_CAPABILITY_VERSION_3;
        LxtCheckErrno(LxtCapSet(&CapHeader, CapData));

        //
        // Attempt to issue a "wait for zero" operation, this should fail.
        //

        memset(Operations, 0, sizeof(Operations));
        LxtCheckErrnoFailure(LxtSemOp(Id, Operations, SEM_COUNT), EACCES);

        //
        // Attempt to increment the semaphore, this should succeed.
        //

        Operations[0].sem_num = 0;
        Operations[0].sem_op = 1;
        LxtCheckErrno(LxtSemOp(Id, Operations, 1));

        //
        // Attempt to decrement the semaphore, this should succeed.
        //

        Operations[0].sem_num = 0;
        Operations[0].sem_op = -1;
        LxtCheckErrno(LxtSemOp(Id, Operations, 1));

        //
        // Fill the operations buffer with a combination of valid operations
        // and operations that the caller does not have permission to do. The
        // parent will verify the semaphore values are adjusted correctly.
        //

        memset(Operations, 0, sizeof(Operations));
        Operations[0].sem_num = 0;
        Operations[0].sem_op = 1;

        Operations[1].sem_num = 1;
        Operations[1].sem_op = 1;

        Operations[2].sem_num = 2;
        Operations[2].sem_op = 0;

        Operations[3].sem_num = 3;
        Operations[3].sem_op = 1;

        Operations[4].sem_num = 2;
        Operations[4].sem_op = 0;

        LxtCheckErrno(LxtSemOp(Id, Operations, 3));
        LxtCheckErrno(LxtSemOp(Id, &Operations[1], 2));
        LxtCheckErrno(LxtSemOp(Id, &Operations[1], 3));
        LxtCheckErrno(LxtSemOp(Id, &Operations[2], 2));
        LxtCheckErrno(LxtSemOp(Id, &Operations[2], 3));
        LxtCheckErrnoFailure(LxtSemOp(Id, &Operations[2], 1), EACCES);
        LXT_SYNCHRONIZATION_POINT(); // (1)

        //
        // Wait for parent to query.
        //

        LXT_SYNCHRONIZATION_POINT(); // (2)

        //
        // Test how overflow is handled. It looks like there is a per-semaphore
        // rolling count that is checked before any operations are performed.
        //

        memset(Operations, 0, sizeof(Operations));
        Operations[0].sem_op = 32767;
        Operations[1].sem_op = 1;
        LxtCheckErrno(LxtSemOp(Id, Operations, 1));
        LxtCheckErrnoFailure(LxtSemOp(Id, &Operations[1], 1), ERANGE);
        LXT_SYNCHRONIZATION_POINT(); // (3)

        //
        // Wait for parent to query.
        //

        LXT_SYNCHRONIZATION_POINT(); // (4)
        LxtCheckErrnoFailure(LxtSemOp(Id, Operations, 2), ERANGE);
        LXT_SYNCHRONIZATION_POINT(); // (5)

        LXT_SYNCHRONIZATION_POINT(); // (6)
        memset(Operations, 0, sizeof(Operations));
        Operations[0].sem_op = 32767;
        Operations[1].sem_op = -1;
        Operations[2].sem_op = 2;
        Operations[3].sem_op = -1;
        LxtCheckErrnoFailure(LxtSemOp(Id, Operations, 4), ERANGE);
        LXT_SYNCHRONIZATION_POINT(); // (7)

        memset(Operations, 0, sizeof(Operations));
        Operations[0].sem_op = -1;
        Operations[1].sem_op = 32767;
        Operations[2].sem_op = 1;
        LXT_SYNCHRONIZATION_POINT(); // (8)
        LxtLogInfo("child semop");
        LxtCheckErrnoFailure(LxtSemOp(Id, Operations, 4), ERANGE);
        LxtLogInfo("child return");
        LXT_SYNCHRONIZATION_POINT(); // (9)
        goto ErrorExit;
    }

    //
    // Wait for the child to do the first semop and query the values.
    //

    LXT_SYNCHRONIZATION_POINT(); // (1)
    LxtCheckErrno(LxtSemCtl(Id, 0, GETALL, &Values));
    LxtCheckEqual(1, Values[0], "%u");
    LxtCheckEqual(3, Values[1], "%u");
    LxtCheckEqual(3, Values[3], "%u");
    memset(Values, 0, sizeof(Values));
    LxtCheckErrno(LxtSemCtl(Id, 0, SETALL, &Values));
    LXT_SYNCHRONIZATION_POINT(); // (2)

    LXT_SYNCHRONIZATION_POINT(); // (3)
    LxtCheckErrno(LxtSemCtl(Id, 0, GETALL, &Values));
    LxtCheckEqual(32767, Values[0], "%u");
    memset(Values, 0, sizeof(Values));
    LxtCheckErrno(LxtSemCtl(Id, 0, SETALL, &Values));
    LXT_SYNCHRONIZATION_POINT(); // (4)

    LXT_SYNCHRONIZATION_POINT(); // (5)
    LxtCheckErrno(LxtSemCtl(Id, 0, GETALL, &Values));
    LxtCheckEqual(0, Values[0], "%u");

    LXT_SYNCHRONIZATION_POINT(); // (6)
    LxtCheckErrno(LxtSemCtl(Id, 0, GETALL, &Values));
    LxtCheckEqual(0, Values[0], "%u");
    memset(Values, 0, sizeof(Values));
    LxtCheckErrno(LxtSemCtl(Id, 0, SETALL, &Values));
    LXT_SYNCHRONIZATION_POINT(); // (7)

    LXT_SYNCHRONIZATION_POINT(); // (8)
    Operations[0].sem_num = 0;
    Operations[0].sem_op = 1;
    sleep(1);
    LxtCheckErrno(LxtSemOp(Id, Operations, 1));
    LXT_SYNCHRONIZATION_POINT(); // (9)
    LxtCheckErrno(LxtSemCtl(Id, 0, GETALL, &Values));
    LxtCheckEqual(1, Values[0], "%u");

    //
    // Wait for the child to exit.
    //

    LxtCheckErrno(LxtWaitPidPoll(ChildPid, LXT_RESULT_SUCCESS));

    //
    // Invalid parameter variations.
    //

    LxtCheckErrnoFailure(LxtSemOp(Id, NULL, 0), EINVAL);
    LxtCheckErrnoFailure(LxtSemOp(Id, NULL, 501), E2BIG);
    LxtCheckErrnoFailure(LxtSemOp(Id, NULL, 1), EFAULT);
    LxtCheckErrnoFailure(LxtSemOp(Id, -1, 1), EFAULT);
    LxtCheckErrnoFailure(LxtSemOp(-1, NULL, 0), EINVAL);
    LxtCheckErrnoFailure(LxtSemOp(-1, NULL, 1), EINVAL);

    LxtCheckErrnoFailure(LxtSemTimedOp(Id, NULL, 0, NULL), EINVAL);
    LxtCheckErrnoFailure(LxtSemTimedOp(Id, NULL, 501, NULL), E2BIG);
    LxtCheckErrnoFailure(LxtSemTimedOp(Id, NULL, 1, NULL), EFAULT);
    LxtCheckErrnoFailure(LxtSemTimedOp(Id, -1, 1, NULL), EFAULT);
    LxtCheckErrnoFailure(LxtSemTimedOp(Id, Operations, 1, -1), EFAULT);
    LxtCheckErrnoFailure(LxtSemTimedOp(-1, NULL, 0, NULL), EINVAL);
    LxtCheckErrnoFailure(LxtSemTimedOp(-1, NULL, 1, -1), EINVAL);
    Timeout.tv_sec = 0;
    Timeout.tv_nsec = 999999999 + 1;
    LxtCheckErrnoFailure(LxtSemTimedOp(Id, Operations, 1, &Timeout), EINVAL);
    Timeout.tv_sec = -1;
    Timeout.tv_nsec = 0;
    LxtCheckErrnoFailure(LxtSemTimedOp(Id, Operations, 1, &Timeout), EINVAL);

ErrorExit:
    LXT_SYNCHRONIZATION_POINT_END();
    if (ChildPid == 0)
    {
        _exit(Result);
    }

    if (Id != -1)
    {
        LxtSemCtl(Id, 0, IPC_RMID, NULL);
    }

    return Result;
}

void SemPrintInfo(struct semid_ds* Stat)

{

    if (g_VerboseSem == false)
    {
        return;
    }

    LxtLogInfo("sem_perm.__key %u", Stat->sem_perm.__key);
    LxtLogInfo("sem_perm.uid %u", Stat->sem_perm.uid);
    LxtLogInfo("sem_perm.gid %u", Stat->sem_perm.gid);
    LxtLogInfo("sem_perm.cuid %u", Stat->sem_perm.cuid);
    LxtLogInfo("sem_perm.cgid %u", Stat->sem_perm.cgid);
    LxtLogInfo("sem_perm.mode %o", Stat->sem_perm.mode);
    LxtLogInfo("sem_perm.__seq %d", Stat->sem_perm.__seq);
    LxtLogInfo("sem_otime %Iu", Stat->sem_otime);
    LxtLogInfo("sem_ctime %Iu", Stat->sem_ctime);
    LxtLogInfo("sem_nsems %Iu", Stat->sem_nsems);
    return;
}
