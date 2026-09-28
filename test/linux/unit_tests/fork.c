/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    Fork.c

Abstract:

    This file is a Fork test.

--*/

#include "lxtcommon.h"
#include "unittests.h"
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <linux/futex.h>
#include <sys/time.h>
#include <fpu_control.h>
#include <alloca.h>
#include <stdlib.h>
#include <sys/prctl.h>

#if defined(__amd64__)

#include <asm/prctl.h>

#endif

#if defined(__i386__)
#define GET_STACK_POINTER asm("esp")
#elif defined(__amd64__)
#define GET_STACK_POINTER asm("rsp")
#elif defined(__arm__) || defined(__aarch64__)
#define GET_STACK_POINTER asm("sp")
#else
#error "Unsupported architecture"
#endif

#include <fcntl.h>
#include <unistd.h>

#define LXT_NAME "Fork"

#define LXT_INVALID_TID_VALUE -1
#define LXT_INVALID_TID_ADDRESS ((void*)LXT_INVALID_TID_VALUE)
#define LXT_THREAD_UMASK 0555
#define LXT_CONTROL_WORD_DEFAULT 0x37f
#define LXT_CONTROL_WORD_NEW 0x40
#define LXT_STACK_SIZE (1 * 1024 * 1024)
#define LXT_TEST_CWD "/"

int SetTidAddress(PLXT_ARGS Args);

int ForkPids(PLXT_ARGS Args);

int ExecvFailure(PLXT_ARGS Args);

int RobustFutex(PLXT_ARGS Args);

int CloneFs(PLXT_ARGS Args);

int CloneInvalidFlags(PLXT_ARGS Args);

int CloneThread(PLXT_ARGS Args);

int VForkTestBasic(PLXT_ARGS Args);

int VForkTest(PLXT_ARGS Args);

int CloneTestFlags(PLXT_ARGS Args);

int CloneTestSignalParent(PLXT_ARGS Args);

//
// Global constants
//
// N.B. Test ordering is important for child process variations.
//
// TODO_LX: Enable fork tests.
//

#define LXT_DEFAULT_VARIATIONS ((unsigned long)(-1))
#define LXT_CHILD_VARIATIONS 0

static const LXT_VARIATION g_LxtVariations[] = {
    {"Fork check pids", ForkPids},
    {"Set tid address", SetTidAddress},
    {"Execv failure", ExecvFailure},
    {"Get / Set Robust Futex List", RobustFutex},
    {"Clone CLONE_FS", CloneFs},
    {"Clone invalid flags", CloneInvalidFlags},
    {"VFork test basic", VForkTestBasic},
    {"Clone thread", CloneThread},
    {"Vfork behavior", VForkTest},
    {"Clone test flags", CloneTestFlags},
    {"Clone signal parent test", CloneTestSignalParent},
};

int ForkTestEntry(int Argc, char* Argv[])

/*++
--*/

{

    LXT_ARGS Args;
    int Result;

    LxtCheckResult(LxtInitialize(Argc, Argv, &Args, LXT_NAME));
    if (Args.VariationMask == LXT_DEFAULT_VARIATIONS)
    {
        Args.VariationMask &= ~LXT_CHILD_VARIATIONS;
    }

    LxtCheckResult(LxtRunVariations(&Args, g_LxtVariations, LXT_COUNT_OF(g_LxtVariations)));

ErrorExit:
    LxtUninitialize();
    return !LXT_SUCCESS(Result);
}

long my_set_tid_address(int* tid)

{
    return syscall(SYS_set_tid_address, tid);
}

int my_futex(int* uaddr, int op, int val, const struct timespec* timeout, int* uaddr2, int val3)

{
    return syscall(SYS_futex, uaddr, op, val, timeout, uaddr2, val3);
}

int my_set_robust_list(struct robust_list_head* head, size_t len)

{
    return syscall(SYS_set_robust_list, head, len);
}

int my_get_robust_list(int pid, struct robust_list_head** head_ptr, size_t* len_ptr)

{
    return syscall(SYS_get_robust_list, pid, head_ptr, len_ptr);
}

mode_t my_umask(mode_t mask)

{
    return syscall(SYS_umask, mask);
}

void* SetChildTidThread(void* Args)

{

    int* TidPointer = Args;
    int tid;
    mode_t umaskTemp;

    tid = my_set_tid_address(TidPointer);
    LxtLogInfo("In pthread tid = %d", tid);
    if (TidPointer != LXT_INVALID_TID_ADDRESS)
    {
        *TidPointer = tid;
    }

    umaskTemp = my_umask(LXT_THREAD_UMASK);
    LxtLogInfo("In pthread tid = %d, initial umask %u, umask set to %u", tid, umaskTemp, LXT_THREAD_UMASK);

    sleep(2);
    return 0;
}

int SetTidAddress(PLXT_ARGS Args)
{

    pid_t ChildPid;
    mode_t ChildUmask;
    int ForkTid;
    mode_t OriginalUmask;
    int ParentTid;
    int Result = LXT_RESULT_FAILURE;
    int Return;
    int SavedThread1Tid = 0;
    pthread_t Thread1 = {0};
    int Thread1Tid;
    pthread_t Thread2 = {0};
    int Thread2Tid;
    mode_t UmaskTemp;

    ChildUmask = 0770;
    OriginalUmask = 0777;
    ForkTid = -1;
    UmaskTemp = my_umask(OriginalUmask);
    LxtLogInfo("Initial umask %u", UmaskTemp);
    LxtLogInfo("OriginalUmask %u", OriginalUmask);
    ParentTid = -1;
    Thread1Tid = -1;
    Thread2Tid = -1;
    ParentTid = my_set_tid_address(&ParentTid);
    LxtCheckResult(ChildPid = fork());
    if (ChildPid == 0)
    {

        //
        // Set the clear child tid value for the new process.
        //

        ForkTid = my_set_tid_address(&ForkTid);
        UmaskTemp = my_umask(ChildUmask);
        LxtLogInfo("Before Thread1");
        LxtLogInfo("Initial child umask %u", UmaskTemp);
        LxtLogInfo("ChildUmask %u", ChildUmask);
        LxtLogInfo("ParentTid %d", ParentTid);
        LxtLogInfo("ForkTid %d", ForkTid);
        LxtLogInfo("Thread1Tid %d", Thread1Tid);
        LxtLogInfo("Thread2Tid %d", Thread2Tid);

        //
        // Spawn two threads and set a different address for each child tid.
        //

        LxtCheckErrno(pthread_create(&Thread1, NULL, SetChildTidThread, &Thread1Tid));
        LxtCheckErrno(pthread_create(&Thread2, NULL, SetChildTidThread, LXT_INVALID_TID_ADDRESS));
        sleep(1);
        SavedThread1Tid = Thread1Tid;
        UmaskTemp = my_umask(ChildUmask);
        LxtLogInfo("After thread creation");
        LxtLogInfo("Original umask %u", UmaskTemp);
        LxtLogInfo("set back to ChildUmask %u", ChildUmask);
        LxtLogInfo("ParentTid %d", ParentTid);
        LxtLogInfo("ForkTid %d", ForkTid);
        LxtLogInfo("Thread1Tid %d", Thread1Tid);
        LxtLogInfo("Thread2Tid %d", Thread2Tid);
        if (Thread1Tid == 0)
        {
            LxtLogError("Thread1Tid == 0 after calling set_tid_address");
        }

        //
        // Do a futex wait on Thread1Tid and validate that it has been set to 0
        // by the kernel. Futex will fail with EAGAIN if the value has already
        // been set.
        //

        Return = my_futex(&Thread1Tid, FUTEX_WAIT, SavedThread1Tid, NULL, NULL, 0);
        if (Return != 0)
        {
            if (errno != EAGAIN)
            {
                LxtLogError("futex returned unexpected error %d - %s", errno, strerror(errno));
            }
        }

        //
        // Don't join thread 1; pthread_join is implemented using the clear
        // tid address, so since it has been changed it won't work.
        //

        pthread_join(Thread2, NULL);

        LxtLogInfo("After Thread join and futex wait");
        LxtLogInfo("ParentTid %d", ParentTid);
        LxtLogInfo("ForkTid %d", ForkTid);
        LxtLogInfo("Thread1Tid %d", Thread1Tid);
        LxtLogInfo("Thread2Tid %d", Thread2Tid);
        if (Thread1Tid != 0)
        {
            LxtLogError("Thread1Tid != 0, was %d", Thread1Tid);
        }

        if (Thread2Tid != LXT_INVALID_TID_VALUE)
        {
            LxtLogError("Thread2Tid != -1, was %d", Thread2Tid);
        }

        _exit(0);
    }

    LxtWaitPidPollOptions(ChildPid, 0, 0, 5);
    LxtLogInfo("Parent after fork");
    LxtLogInfo("ParentTid %d", ParentTid);
    LxtLogInfo("ForkTid %d", ForkTid);
    LxtLogInfo("Thread1Tid %d", Thread1Tid);
    LxtLogInfo("Thread2Tid %d", Thread2Tid);

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    return Result;
}

int ForkPids(PLXT_ARGS Args)

/*++
--*/

{

    pid_t ChildPid;
    int Result;
    pid_t ParentPid;
    pid_t Pid;
    pid_t WaitPidResult;
    int WaitPidStatus;

    //
    // Basic test for fork and vfork that confirms pids are incremented by 1 to
    // ensure syscalls were plumbed correctly. Additional tests should be added
    // to check the many other cases for fork.
    //

    Pid = getpid();
    if (Pid == 0)
    {
        Result = LXT_RESULT_FAILURE;
        LxtLogError("getpid returned 0 for parent");
        goto ErrorExit;
    }

    //
    // Fork should return parent + 1 (assumes no other processes are running).
    //

    LxtCheckResult(ChildPid = fork());
    if (ChildPid == 0)
    {
        Result = LXT_RESULT_SUCCESS;
        ChildPid = getpid();
        if (ChildPid == 0)
        {
            Result = LXT_RESULT_FAILURE;
            LxtLogError("getpid returned 0 for child");
        }

        ParentPid = getppid();
        if (ParentPid != Pid)
        {
            Result = LXT_RESULT_FAILURE;
            LxtLogError("fork() Unexpected parent pid %d for child pid %d", ParentPid, ChildPid);
        }

        _exit(Result);
    }

    LxtCheckResult((WaitPidResult = waitpid(ChildPid, &WaitPidStatus, 0)));
    if ((WIFEXITED(WaitPidStatus) == 0) || (WEXITSTATUS(WaitPidStatus) != 0))
    {
        Result = LXT_RESULT_FAILURE;
        LxtLogError("Unexpected child pid exit status %d", WaitPidStatus);
        goto ErrorExit;
    }

    LxtCheckResult(ChildPid = vfork());
    if (ChildPid == 0)
    {
        Result = LXT_RESULT_SUCCESS;
        ChildPid = getpid();
        if (ChildPid == 0)
        {
            Result = LXT_RESULT_FAILURE;
            LxtLogError("getpid returned 0 for child");
        }

        ParentPid = getppid();
        if (ParentPid != Pid)
        {
            Result = LXT_RESULT_FAILURE;
            LxtLogError("vfork() Unexpected parent pid %d for child pid %d", ParentPid, ChildPid);
        }

        _exit(Result);
    }

    LxtCheckResult((WaitPidResult = waitpid(ChildPid, &WaitPidStatus, 0)));
    if ((WIFEXITED(WaitPidStatus) == 0) || (WEXITSTATUS(WaitPidStatus) != 0))
    {
        Result = LXT_RESULT_FAILURE;
        LxtLogError("Unexpected child pid exit status %d", WaitPidStatus);
        goto ErrorExit;
    }

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    return Result;
}

int ExecvFailure(PLXT_ARGS Args)

/*++
--*/

{

    char* CommandLine1[] = {"/foo/bar/foo/bar", NULL};
    char* CommandLine2[] = {"/data/test/Makefile", NULL};
    int Result;

    //
    // Check that execv fails for an invalid file name and an invalid elf file.
    //

    LxtCheckErrnoFailure(execv(CommandLine1[0], CommandLine1), ENOENT);
    LxtCheckErrnoFailure(execv(CommandLine2[0], CommandLine2), ENOEXEC);

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    return Result;
}

int RobustFutex(PLXT_ARGS Args)

{

    struct robust_list_head Head;
    struct robust_list_head* HeadReturned;
    int Result = LXT_RESULT_FAILURE;
    size_t SizeReturned;

    //
    // Set and get the robust list.
    //

    LxtCheckResult(my_set_robust_list(&Head, sizeof(struct robust_list_head)));
    LxtCheckResult(my_get_robust_list(0, &HeadReturned, &SizeReturned));

    if (HeadReturned != &Head)
    {
        LxtLogError("HeadReturned %p != &Head %p", HeadReturned, &Head);
        goto ErrorExit;
    }

    if (SizeReturned != sizeof(struct robust_list_head))
    {
        LxtLogError("SizeReturned %u != sizeof(struct robust_list_head) %u", SizeReturned, sizeof(struct robust_list_head));

        goto ErrorExit;
    }

    //
    // set_robust_list validates the size of the buffer.
    //

    LxtCheckErrnoFailure(my_set_robust_list(&Head, 0), EINVAL);
    LxtCheckErrnoFailure(my_set_robust_list(&Head, (sizeof(struct robust_list_head) + 1)), EINVAL);

    //
    // get_robust_list validates the buffers.
    //

    LxtCheckErrnoFailure(my_get_robust_list(0, NULL, &SizeReturned), EFAULT);
    LxtCheckErrnoFailure(my_get_robust_list(0, &HeadReturned, NULL), EFAULT);

    //
    // No validation is done on the buffer for set_robust_list.
    //

    LxtCheckResult(my_set_robust_list(NULL, sizeof(struct robust_list_head)));
    LxtCheckResult(my_set_robust_list((void*)-1, sizeof(struct robust_list_head)));

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    return Result;
}

int CloneFs(PLXT_ARGS Args)

/*++
--*/

{

    char BackupCwd[256];
    pid_t ChildPid;
    char Path[256];
    bool RestoreCwd;
    int Result;

    ChildPid = -1;
    RestoreCwd = false;
    LxtLogInfo("cwd = %s", getcwd(BackupCwd, sizeof(BackupCwd)));
    LxtCheckResult(ChildPid = LxtCloneSyscall((CLONE_FS | SIGCHLD), NULL, NULL, NULL, NULL));
    if (ChildPid == 0)
    {
        LxtCheckErrno(chdir(LXT_TEST_CWD));
        LxtLogInfo("child cwd = %s", getcwd(Path, sizeof(Path)));
        goto ErrorExit;
    }

    //
    // Wait for the child to exit.
    //

    RestoreCwd = true;
    LxtCheckResult(LxtWaitPidPoll(ChildPid, LXT_RESULT_SUCCESS));

    //
    // Ensure the parent's current working directory was changed.
    //

    LxtLogInfo("parent cwd = %s", getcwd(Path, sizeof(Path)));
    LxtCheckStringEqual(Path, LXT_TEST_CWD);

ErrorExit:
    if (ChildPid == 0)
    {
        _exit(Result);
    }

    if (RestoreCwd != false)
    {
        chdir(BackupCwd);
    }

    return Result;
}

int CloneInvalidFlags(PLXT_ARGS Args)

/*++
--*/

{

    pid_t ChildPid;
    int Result;

    ChildPid = -1;

    //
    // Check for failure cases.
    //

    LxtCheckErrnoFailure(ChildPid = LxtCloneSyscall(CLONE_SIGHAND, NULL, NULL, NULL, NULL), EINVAL);
    LxtCheckErrnoFailure(ChildPid = LxtCloneSyscall(CLONE_THREAD, NULL, NULL, NULL, NULL), EINVAL);
    LxtCheckErrnoFailure(ChildPid = LxtCloneSyscall((CLONE_FS | CLONE_NEWNS), NULL, NULL, NULL, NULL), EINVAL);

ErrorExit:
    if (ChildPid == 0)
    {
        _exit(Result);
    }

    return Result;
}

struct CloneThreadArgs
{
    long Fs0;
    unsigned long FsBase;
    unsigned long GsBase;
};

int CloneThreadEntry(void* Argument)

{

    struct CloneThreadArgs* Args;
    unsigned long FsBase;
    int Result;

    Args = Argument;
    Result = 0;

    //
    // Make sure TLS values can be read without SIGSEGV.
    //

#if defined(__amd64__)

    __asm("movq %%fs:0, %0" : "=r"(Args->Fs0));
    syscall(SYS_arch_prctl, ARCH_GET_FS, &FsBase);
    Result = (Args->FsBase != FsBase);
    Args->FsBase = FsBase;
    syscall(SYS_arch_prctl, ARCH_GET_GS, &Args->GsBase);

#endif

    syscall(SYS_exit, Result);
    return Result;
}

int CloneThread(PLXT_ARGS Args)

{

    struct CloneThreadArgs ThreadArgs;
    int Flags;
    unsigned long FsBase;
    pid_t Pid;
    int Result;
    char* Stack;
    size_t StackSize;
    int Status;
    pid_t Tid;
    long Tls[1];

    StackSize = 1024 * 1024;
    Stack = malloc(StackSize);
    Tls[0] = (long)&Tls[0];
    Flags = CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD | CLONE_SYSVSEM | CLONE_PARENT_SETTID | CLONE_CHILD_CLEARTID;

    //
    // Clone without setting TLS.
    //

    LxtCheckErrno(clone(CloneThreadEntry, Stack + StackSize, Flags, &ThreadArgs, &Tid, NULL, &Tid));
    LxtCheckErrno(LxtJoinThread(&Tid));

#if defined(__amd64__)

    LxtCheckErrno(syscall(SYS_arch_prctl, ARCH_GET_FS, &FsBase));
    LxtCheckEqual(ThreadArgs.FsBase, FsBase, "%lx");

#endif

    //
    // Clone and set TLS.
    //

    ThreadArgs.Fs0 = 0;
    ThreadArgs.FsBase = 0;
    ThreadArgs.GsBase = 0;

#if defined(__amd64__)

    LxtCheckErrno(syscall(SYS_arch_prctl, ARCH_SET_GS, &ThreadArgs));

#endif

    LxtCheckErrno(clone(CloneThreadEntry, Stack + StackSize, Flags | CLONE_SETTLS, &ThreadArgs, &Tid, Tls, &Tid));
    LxtCheckErrno(LxtJoinThread(&Tid));

#if defined(__amd64__)

    LxtCheckEqual(ThreadArgs.Fs0, Tls[0], "%ld");
    LxtCheckEqual(ThreadArgs.FsBase, (unsigned long)&Tls[0], "%lx");

    //
    // Ensure GS base is inherited.
    //

    LxtCheckEqual(ThreadArgs.GsBase, (unsigned long)&ThreadArgs, "%lx");

#endif

    //
    // Disallow invalid TLS values.
    //

    LxtCheckErrnoFailure(clone(CloneThreadEntry, Stack + StackSize, Flags | CLONE_SETTLS, NULL, &Tid, (void*)-1, &Tid), EPERM);
    LxtCheckErrno(LxtJoinThread(&Tid));

    //
    // Set TLS on thread group clone too.
    //

    ThreadArgs.FsBase = (unsigned long)&Tls[0];
    LxtCheckErrno(Pid = clone(CloneThreadEntry, Stack + StackSize, CLONE_SETTLS | SIGCHLD, &ThreadArgs, NULL, Tls, NULL));
    LxtCheckErrno(waitpid(Pid, &Status, 0));
    if (!WIFEXITED(Status) || WEXITSTATUS(Status) != 0)
    {
        LxtLogError("FS check failed: %x", Status);
    }

ErrorExit:
    free(Stack);
    return Result;
}

int VForkTestBasicChild(void* Param)

{

    return 0;
}

int VForkTestBasic(PLXT_ARGS Args)

/*++
--*/

{

    LXT_CLONE_ARGS CloneArgs;
    pid_t ChildPid;
    pid_t Pid;
    int Result;

    ChildPid = -1;

    //
    // Check that vfork runs in a new threadgroup.
    //

    Pid = LxtGetTid();
    LxtCheckErrno(ChildPid = vfork());
    if (ChildPid == 0)
    {
        _exit(LXT_RESULT_SUCCESS);
    }

    LxtCheckNotEqual(Pid, ChildPid, "%d");
    LxtCheckResult(LxtWaitPidPoll(ChildPid, LXT_RESULT_SUCCESS));

    //
    // Repeat the above with the clone variant of vfork.
    //

    LxtCheckErrno(ChildPid = LxtCloneSyscall(CLONE_VM | CLONE_VFORK | SIGCHLD, NULL, NULL, NULL, NULL));
    if (ChildPid == 0)
    {
        _exit(LXT_RESULT_SUCCESS);
    }

    LxtCheckNotEqual(Pid, ChildPid, "%d");
    LxtCheckResult(LxtWaitPidPoll(ChildPid, LXT_RESULT_SUCCESS));
    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    if ((Result < 0) && (ChildPid != 0))
    {
        _exit(Result);
    }

    return Result;
}

int VForkTest(PLXT_ARGS Args)

/*++
--*/

{

    char* ChildCmdLine[] = {WSL_UNIT_TEST_BINARY, Args->Argv[0], "-l", "2", "-v", "32", NULL};

    LxtLogInfo("VForkTest ChildCmdLine: %s %s", ChildCmdLine[0], ChildCmdLine[1]);

    pid_t ChildPid;
    pid_t ChildPidFromChild;
    pid_t ChildPidFromChildNested;
    pid_t ChildPidNested;
    unsigned short ControlWord;
    unsigned short OriginalControlWord;
    char* InvalidCmdLine[] = {"/data/test/Makefile", NULL};
    pid_t Pid;
    int Result;

    ChildPid = -1;
    ChildPidFromChild = -1;
    ChildPidFromChild = -1;
    ChildPidFromChildNested = -1;
    ChildPidNested = -1;
    LxtCheckResult(LxtSignalInitialize());

    //
    // Check that vfork runs in a new threadgroup but in the same address space.
    //

    Pid = LxtGetTid();
    LxtCheckErrno(ChildPid = vfork());
    if (ChildPid == 0)
    {
        ChildPidFromChild = LxtGetTid();
        _exit(LXT_RESULT_SUCCESS);
    }

    LxtCheckNotEqual(Pid, ChildPidFromChild, "%d");
    LxtCheckEqual(ChildPid, ChildPidFromChild, "%d");
    LxtCheckResult(LxtWaitPidPoll(ChildPid, LXT_RESULT_SUCCESS));

    //
    // Release the above with execv.
    //

    ChildPidFromChild = -1;
    Pid = LxtGetTid();
    LxtCheckErrno(ChildPid = vfork());
    if (ChildPid == 0)
    {
        ChildPidFromChild = LxtGetTid();
        LxtCheckErrno(execv(ChildCmdLine[0], ChildCmdLine));
        _exit(LXT_RESULT_FAILURE);
    }

    LxtCheckNotEqual(Pid, ChildPidFromChild, "%d");
    LxtCheckEqual(ChildPid, ChildPidFromChild, "%d");
    LxtCheckResult(LxtWaitPidPoll(ChildPid, LXT_RESULT_SUCCESS));

    //
    // Repeat the above with execv failure.
    //

    ChildPidFromChild = -1;
    Pid = LxtGetTid();
    LxtCheckErrno(ChildPid = vfork());
    if (ChildPid == 0)
    {
        ChildPidFromChild = LxtGetTid();
        LxtCheckErrnoFailure(execv(InvalidCmdLine[0], InvalidCmdLine), ENOEXEC);
        _exit(LXT_RESULT_SUCCESS);
    }

    LxtCheckNotEqual(Pid, ChildPidFromChild, "%d");
    LxtCheckEqual(ChildPid, ChildPidFromChild, "%d");
    LxtCheckResult(LxtWaitPidPoll(ChildPid, LXT_RESULT_SUCCESS));

    //
    // Check that signals sent to the parent after the
    // parent releases the address space.
    //

    LxtCheckResult(LxtSignalSetupHandler(SIGUSR1, 0));
    ChildPidFromChild = -1;
    Pid = LxtGetTid();
    LxtCheckErrno(ChildPid = vfork());
    if (ChildPid == 0)
    {
        ChildPidFromChild = LxtGetTid();
        LxtSignalInitializeThread();
        LxtCheckErrno(LxtTKill(Pid, SIGUSR1));
        LxtCheckResult(LxtSignalCheckNoSignal());
        _exit(LXT_RESULT_SUCCESS);
    }

    LxtCheckNotEqual(Pid, ChildPidFromChild, "%d");
    LxtCheckEqual(ChildPid, ChildPidFromChild, "%d");
    LxtCheckResult(LxtWaitPidPoll(ChildPid, LXT_RESULT_SUCCESS));
    LxtCheckResult(LxtSignalCheckReceived(SIGUSR1));
    LxtSignalResetReceived();

    //
    // Check the behavior for nested vfork.
    //

    Pid = LxtGetTid();
    LxtCheckErrno(ChildPid = vfork());
    if (ChildPid == 0)
    {
        ChildPidFromChild = LxtGetTid();
        LxtCheckErrno(ChildPidNested = vfork());
        if (ChildPidNested == 0)
        {
            ChildPidFromChildNested = LxtGetTid();
            _exit(LXT_RESULT_SUCCESS);
        }

        LxtCheckNotEqual(ChildPid, ChildPidFromChildNested, "%d");
        LxtCheckEqual(ChildPidNested, ChildPidFromChildNested, "%d");
        LxtCheckResult(LxtWaitPidPoll(ChildPidNested, LXT_RESULT_SUCCESS));
        _exit(LXT_RESULT_SUCCESS);
    }

    LxtCheckNotEqual(Pid, ChildPidFromChild, "%d");
    LxtCheckEqual(ChildPid, ChildPidFromChild, "%d");
    LxtCheckResult(LxtWaitPidPoll(ChildPid, LXT_RESULT_SUCCESS));

#if defined(__i386__) || defined(__amd64__)

    //
    // Check that floating point context is preserved across vfork.
    //

    _FPU_GETCW(OriginalControlWord);
    // LX_TODO: Initial control word is not being set correctly
    // LxtCheckEqual(LXT_CONTROL_WORD_DEFAULT, OriginalControlWord, "%hu");
    Pid = LxtGetTid();
    LxtCheckErrno(ChildPid = vfork());
    if (ChildPid == 0)
    {
        ChildPidFromChild = LxtGetTid();
        ControlWord = LXT_CONTROL_WORD_NEW;
        _FPU_SETCW(ControlWord);
        _FPU_GETCW(ControlWord);
        LxtCheckEqual(LXT_CONTROL_WORD_NEW, ControlWord, "%hu");
        _exit(LXT_RESULT_SUCCESS);
    }

    _FPU_GETCW(ControlWord);
    LxtCheckEqual(OriginalControlWord, ControlWord, "%hu");
    LxtCheckNotEqual(Pid, ChildPidFromChild, "%d");
    LxtCheckEqual(ChildPid, ChildPidFromChild, "%d");
    LxtCheckResult(LxtWaitPidPoll(ChildPid, LXT_RESULT_SUCCESS));

#endif

    //
    // Check the stack pointer isn't modified across vfork.
    //

    {
        register unsigned long Rsp GET_STACK_POINTER;
        LxtCheckNotEqual(Rsp, 0, "%lu");
        Pid = LxtGetTid();
        alloca(1024);
        LxtCheckErrno(ChildPid = vfork());
        if (ChildPid == 0)
        {
            register unsigned long RspChild GET_STACK_POINTER;
            LxtCheckEqual(Rsp, RspChild, "%lu");
            ChildPidFromChild = LxtGetTid();
            _exit(LXT_RESULT_SUCCESS);
        }

        LxtCheckNotEqual(Pid, ChildPidFromChild, "%d");
        LxtCheckEqual(ChildPid, ChildPidFromChild, "%d");
        LxtCheckResult(LxtWaitPidPoll(ChildPid, LXT_RESULT_SUCCESS));
    }

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    if (ChildPid == 0)
    {
        _exit(Result);
    }

    return Result;
}

struct CloneFlagArgs
{
    int Test;
    int Flags;
    int Fd;
};

int CloneFlags[] = {
    SIGCHLD,
    SIGCHLD | CLONE_FS,
    SIGCHLD | CLONE_FILES,
    SIGCHLD | CLONE_FS | CLONE_FILES,
    SIGCHLD | CLONE_VFORK,
    SIGUSR1,
    0,
    125, // invalid signal
    CLONE_THREAD | CLONE_VM | CLONE_SIGHAND,
    CLONE_THREAD | CLONE_VM | CLONE_SIGHAND | CLONE_FS,
    CLONE_THREAD | CLONE_VM | CLONE_SIGHAND | CLONE_FILES,
    CLONE_THREAD | CLONE_VM | CLONE_SIGHAND | CLONE_FS | CLONE_FILES,
    CLONE_THREAD | CLONE_VM | CLONE_SIGHAND | CLONE_VFORK,
    SIGCHLD | CLONE_THREAD | CLONE_VM | CLONE_SIGHAND,
    125 | CLONE_THREAD | CLONE_VM | CLONE_SIGHAND,
};

int CloneFlagEntry(void* Arg)

{

    struct CloneFlagArgs* FlagArgs = Arg;
    close(FlagArgs->Fd);
    chdir("/");
    syscall(SYS_exit, 0);
    return 0;
}

int CloneTestFlags(PLXT_ARGS Args)

{

    int Result;
    char __attribute__((aligned(16))) Stack[65536];

    int Root;
    LxtCheckErrno(Root = open(".", O_RDONLY));

    for (int Test = 0; Test < sizeof(CloneFlags) / sizeof(CloneFlags[0]); Test++)
    {
        int Flags = CloneFlags[Test];

        int Fd;
        LxtCheckErrno(Fd = dup(Root));

        int TermSignal = Flags & 0xff;
        if ((Flags & CLONE_THREAD) != 0 || TermSignal < 1 || TermSignal > 64)
        {
            TermSignal = 0;
        }

        if (TermSignal != 0 && TermSignal != SIGCHLD)
        {
            signal(TermSignal, SIG_IGN);
        }

        LxtLogInfo("Test %d: Flags 0x%x", Test, Flags);

        struct CloneFlagArgs FlagArgs = {0};
        FlagArgs.Test = Test;
        FlagArgs.Flags = Flags;
        FlagArgs.Fd = Fd;

        if (Flags & CLONE_THREAD)
        {
            Flags |= CLONE_PARENT_SETTID | CLONE_CHILD_CLEARTID;
        }

        int Pid;
        int Tid;
        LxtCheckErrno(Pid = clone(CloneFlagEntry, Stack + sizeof(Stack), Flags, &FlagArgs, &Tid, NULL, &Tid));

        if (Flags & CLONE_THREAD)
        {
            LxtCheckErrnoFailure(waitpid(Pid, NULL, __WALL), ECHILD);
            LxtCheckErrno(LxtJoinThread(&Tid));
        }
        else if (TermSignal == SIGCHLD)
        {
            LxtCheckErrnoFailure(waitpid(Pid, NULL, __WCLONE), ECHILD);
            LxtCheckErrno(waitpid(Pid, NULL, 0));
        }
        else
        {
            LxtCheckErrnoFailure(waitpid(Pid, NULL, 0), ECHILD);
            LxtCheckErrno(waitpid(Pid, NULL, __WCLONE));
        }

        if (TermSignal != 0)
        {
            signal(TermSignal, SIG_DFL);
        }

        if (Flags & CLONE_FILES)
        {
            // Make sure Fd is not open.
            LxtCheckErrnoFailure(fcntl(Fd, F_GETFL), EBADF);
        }
        else
        {
            // Make sure Fd is still open.
            LxtCheckErrno(fcntl(Fd, F_GETFL));
            close(Fd);
        }

        char Path[1024] = {0};
        LxtCheckErrno(LxtGetcwd(Path, sizeof(Path)));
        if (Flags & CLONE_FS)
        {
            if (strcmp(Path, "/") != 0)
            {
                LxtLogError("Root directory did not change from %s.", Path);
            }
            fchdir(Root);
        }
        else
        {
            if (strcmp(Path, "/") == 0)
            {
                LxtLogError("Root directory changed.");
            }
        }
    }

    Result = 0;

ErrorExit:
    return Result;
}

int CloneTestSignalEntry(void* Arg)
{
    //
    // Make sure the two SIGCHLD signals don't coalesce.
    //

    usleep(500000);
    syscall(SYS_exit, 0);
    return 1;
}

static int CloneTestSignalArrived;

void CloneTestSignalHandler(int Signal, siginfo_t* Info, void* Extra)
{

    CloneTestSignalArrived++;
}

int CloneTestSignalParent(PLXT_ARGS Args)
{

    int Result;

    LxtCheckErrno(LxtSignalInitialize());
    LxtCheckErrno(LxtSignalBlock(SIGCHLD));

    int Pid = 0;
    volatile int ChildPid = 0;
    LxtCheckErrno(Pid = vfork());
    if (Pid == 0)
    {
        char Stack[65536];

        //
        // Even though SIGUSR1 is passed here, SIGCHLD should be the
        // received signal since CLONE_PARENT is also passed
        // (SIGCHLD comes from the vfork call above).
        //

        ChildPid = clone(CloneTestSignalEntry, Stack + sizeof(Stack), SIGUSR1 | CLONE_PARENT, NULL);

        _exit(0);
    }

    LxtCheckErrno(LxtSignalWaitBlocked(SIGCHLD, Pid, 1));
    int Status;
    LxtCheckErrno(waitpid(Pid, &Status, 0));

    LxtCheckErrno(ChildPid);
    LxtCheckErrno(LxtSignalWaitBlocked(SIGCHLD, ChildPid, 1));
    LxtCheckErrno(waitpid(ChildPid, NULL, 0));
    Result = 0;

ErrorExit:
    return Result;
}