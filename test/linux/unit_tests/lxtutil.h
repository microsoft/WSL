/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    lxtutil.h

Abstract:

    This file contains lx test common utility functions that log appropriately.

--*/

#ifndef _LXT_UTIL
#define _LXT_UTIL

#include "lxtlog.h"

#if defined(__aarch64__)

#define __ARCH_WANT_SYSCALL_DEPRECATED

#endif

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <signal.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>

#define LXT_WAITPID_DEFAULT_TIMEOUT 10

#define LXT_CHECK_DIRECTORY_CONTENTS_READ_FILES (0x1)

#define LXT_ROUND_UP_COUNT(Count, Pow2) (((Count) + (Pow2) - 1) & (~(((Pow2)) - 1)))

#define LXT_CLONE_STACK_SIZE (1024 * 1024)

#define LxtCheckClose(_Fd) \
    { \
        LxtCheckResult(LxtClose(_Fd)); \
        (_Fd) = -1; \
    }

typedef struct _LXT_ARGS
{
    LxtLogType LogType;
    bool LogAppend;
    unsigned long long VariationMask;
    bool HelpRequested;
    int Argc;
    char** Argv;
} LXT_ARGS, *PLXT_ARGS;

typedef int LXT_VARIATION_HANDLER(PLXT_ARGS Args);

typedef struct _LXT_VARIATION
{
    const char* Name;
    LXT_VARIATION_HANDLER* Variation;
} LXT_VARIATION, *PLXT_VARIATION;

typedef const LXT_VARIATION* PCLXT_VARIATION;

typedef unsigned char BOOLEAN;

#define TRUE 1
#define FALSE 0

#define LXT_COUNT_OF(x) (sizeof(x) / sizeof((x)[0]))

#define LXT_INIT_PID ((pid_t)1)

#define LXT_DEFAULT_EXIT_CODE 0

typedef struct _LXT_CHILD_INFO
{
    const char* Name;
    unsigned char FileType;
} LXT_CHILD_INFO, *PLXT_CHILD_INFO;

//
// Test framework code
//

int LxtCheckDirectoryContents(const char* Path, const LXT_CHILD_INFO* Children, size_t Count);

int LxtCheckDirectoryContentsEx(const char* Path, const LXT_CHILD_INFO* Children, size_t Count, int Flags);

int LxtCheckFdPath(int Fd, char* ExpectedPath);

int LxtCheckLinkTarget(const char* Path, const char* ExpectedTarget);

int LxtCheckRead(const char* FullPath, unsigned char FileType);

int LxtCheckStat(const char* FullPath, unsigned long long ExpectedInode, unsigned char FileType);

int LxtCheckWslPathTranslation(char* Path, const char* ExpectedPath, bool WinPath);

int LxtCompareMemory(const void* First, const void* Second, size_t Size, const char* FirstDescription, const char* SecondDescription);

int LxtCopyFile(const char* Source, const char* Destination);

int LxtInitialize(int Argc, char* Argv[], PLXT_ARGS Args, const char* TestName);

int LxtRunVariations(PLXT_ARGS Args, PCLXT_VARIATION Variations, unsigned int VariationCount);

int LxtRunVariationsForked(PLXT_ARGS Args, PCLXT_VARIATION Variations, unsigned int VariationCount);

int LxtCheckWrite(const char* FullPath, const char* Value);

void LxtUninitialize(void);

//
// stdlib wrappers
//

void* LxtAlloc(size_t Size);

void LxtFree(void* Allocation);

//
// syscall wrappers
//

#define LxtExit(_status) (syscall(1, _status))
#define LxtRt_SigAction(_signal, _action, _oaction, _setsize) (syscall(SYS_rt_sigaction, _signal, _action, _oaction, _setsize))
#define LxtRt_SigProcMask(_how, _set, _oset, _setsize) (syscall(SYS_rt_sigprocmask, _how, _set, _oset, _setsize))
#define LxtTKill(_tid, _sig) (syscall(SYS_tkill, _tid, _sig))
#define LxtTgKill(_tgid, _tid, _sig) (syscall(SYS_tgkill, _tgid, _tid, _sig))
#define LxtRead(_fd, _buffer, _count) (syscall(SYS_read, _fd, _buffer, _count))
#define LxtWrite(_fd, _buffer, _count) (syscall(SYS_write, _fd, _buffer, _count))
#define LxtGetTid() (syscall(SYS_gettid))
#define gettid() LxtGetTid()
#define LxtPipe2(_pipefds, _flags) (syscall(__NR_pipe2, _pipefds, _flags))
#define LxtFutex(_uaddr, _op, _val, _timeout, _uaddr2, _val3) (syscall(SYS_futex, _uaddr, _op, _val, _timeout, _uaddr2, _val3))
#define LxtCapGet(_header, _data) (syscall(SYS_capget, _header, _data))
#define LxtCapSet(_header, _data) (syscall(SYS_capset, _header, _data))
#define LxtExecve(_Filename, _Argv, _Envp) syscall(SYS_execve, (_Filename), (_Argv), (_Envp))
#define LxtWaitId(_idtype, _id, _infop, _options, _rusage) syscall(SYS_waitid, (_idtype), (_id), (_infop), (_options), (_rusage))
#define LxtSetUid(_uid) syscall(SYS_setuid, (_uid))
#if !defined(__amd64__)
#define LxtCloneSyscall(_flags, _stack, _ptid, _ctid, _tls) (syscall(SYS_clone, _flags, _stack, _ptid, _tls, _ctid))
#else
#define LxtCloneSyscall(_flags, _stack, _ptid, _ctid, _tls) (syscall(SYS_clone, _flags, _stack, _ptid, _ctid, _tls))
#endif
#define LxtTimer_Create(_clockid, _sevp, _timerid) syscall(SYS_timer_create, (_clockid), (_sevp), (_timerid))
#define LxtTimer_SetTime(_timerid, _flags, _new_value, _old_value) \
    syscall(SYS_timer_settime, (_timerid), (_flags), (_new_value), (_old_value))
#define LxtTimer_GetTime(_timerid, _curr_value) syscall(SYS_timer_gettime, (_timerid), (_curr_value))
#define LxtTimer_GetOverrun(_timerid) syscall(SYS_timer_getoverrun, (_timerid))
#define LxtTimer_Delete(_timerid) syscall(SYS_timer_delete, (_timerid))
#define LxtClock_Nanosleep(_clockid, _flags, _request, _remain) \
    syscall(SYS_clock_nanosleep, (_clockid), (_flags), (_request), (_remain))
#define LxtGetrandom(_buffer, _size, _flags) syscall(SYS_getrandom, (_buffer), (_size), (_flags))
#define LxtShmAt(_id, _address, _flags) (void*)(syscall(SYS_shmat, (_id), (_address), (_flags)))
#define LxtShmCtl(_id, _cmd, _buffer) syscall(SYS_shmctl, (_id), (_cmd), (_buffer))
#define LxtShmDt(_address) syscall(SYS_shmdt, (_address))
#define LxtShmGet(_key, _size, _flags) syscall(SYS_shmget, (_key), (_size), (_flags))
#define LxtMremap(_oldAddress, _oldSize, _newSize, _flags, _newAddress) \
    (void*)(syscall(SYS_mremap, _oldAddress, _oldSize, _newSize, _flags, _newAddress))
#define LxtSemCtl(_id, _number, _command, _buffer) syscall(SYS_semctl, (_id), (_number), (_command), (_buffer))
#define LxtSemGet(_key, _count, _flags) syscall(SYS_semget, (_key), (_count), (_flags))
#define LxtSemOp(_id, _operations, _opcount) syscall(SYS_semop, (_id), (_operations), (_opcount))
#define LxtSemTimedOp(_id, _operations, _opcount, _timeout) syscall(SYS_semtimedop, (_id), (_operations), (_opcount), (_timeout))
#define LxtIoprio_get(_which, _who) syscall(__NR_ioprio_get, (_which), (_who))
#define LxtIoprio_set(_which, _who, _prio) syscall(__NR_ioprio_set, (_which), (_who), (_prio))
#define LxtSched_GetAffinity(_pid, _cpusetsize, _mask) syscall(__NR_sched_getaffinity, (_pid), (_cpusetsize), (_mask))
#define LxtSched_SetAffinity(_pid, _cpusetsize, _mask) syscall(__NR_sched_setaffinity, (_pid), (_cpusetsize), (_mask))
#define LxtListxattr(_path, _buffer, _size) (syscall(SYS_listxattr, (_path), (_buffer), (_size)))
#define LxtLlistxattr(_path, _buffer, _size) (syscall(SYS_llistxattr, (_path), (_buffer), (_size)))
#define LxtFlistxattr(_fd, _buffer, _size) (syscall(SYS_flistxattr, (_fd), (_buffer), (_size)))
#define LxtGetxattr(_path, _name, _buffer, _size) (syscall(SYS_getxattr, (_path), (_name), (_buffer), (_size)))
#define LxtLgetxattr(_path, _name, _buffer, _size) (syscall(SYS_lgetxattr, (_path), (_name), (_buffer), (_size)))
#define LxtFgetxattr(_fd, _name, _buffer, _size) (syscall(SYS_fgetxattr, (_fd), (_name), (_buffer), (_size)))
#define LxtSetxattr(_path, _name, _buffer, _size, _flags) (syscall(SYS_setxattr, (_path), (_name), (_buffer), (_size), (_flags)))
#define LxtLsetxattr(_path, _name, _buffer, _size, _flags) \
    (syscall(SYS_lsetxattr, (_path), (_name), (_buffer), (_size), (_flags)))
#define LxtFsetxattr(_fd, _name, _buffer, _size, _flags) (syscall(SYS_fsetxattr, (_fd), (_name), (_buffer), (_size), (_flags)))
#define LxtRemovexattr(_path, _name) (syscall(SYS_removexattr, (_path), (_name)))
#define LxtLremovexattr(_path, _name) (syscall(SYS_lremovexattr, (_path), (_name)))
#define LxtFremovexattr(_fd, _name) (syscall(SYS_fremovexattr, (_fd), (_name)))
#define LxtGetresuid(_real, _effective, _saved) (syscall(SYS_getresuid, (_real), (_effective), (_saved)))
#define LxtSetresuid(_real, _effective, _saved) (syscall(SYS_setresuid, (_real), (_effective), (_saved)))
#define LxtSetresgid(_real, _effective, _saved) (syscall(SYS_setresgid, (_real), (_effective), (_saved)))
#define LxtSetfsgid(_gid) syscall(__NR_setfsgid, (_gid))
#define LxtSetfsuid(_uid) syscall(__NR_setfsuid, (_uid))
#define LxtPrlimit64(_pid, _resource, _newvalue, _oldvalue) syscall(__NR_prlimit64, (_pid), (_resource), (_newvalue), (_oldvalue))
//__NR_getdents has been removed from newer versions of libc
#ifdef __NR_getdents
#define LxtGetdents(_fd, _buffer, _size) syscall(__NR_getdents, (_fd), (_buffer), (_size))
#endif
#define LxtGetdents64(_fd, _buffer, _size) syscall(__NR_getdents64, (_fd), (_buffer), (_size))
#define LxtGetcwd(_buffer, _size) syscall(SYS_getcwd, (_buffer), (_size))

#if defined(__aarch64__)
#define LxtFStatAt64(_dirfd, _path, _buffer, _flags) fstatat(_dirfd, _path, _buffer, _flags)
#elif !defined(__amd64__)
#define LxtFStatAt64(_dirfd, _path, _buffer, _flags) syscall(SYS_fstatat64, _dirfd, _path, _buffer, _flags)
#else
#define LxtFStatAt64(_dirfd, _path, _buffer, _flags) fstatat64(_dirfd, _path, _buffer, _flags)
#endif

#ifndef clock_gettime
#define LxtClockGetTime(_clockid, _timespec) syscall(__NR_clock_gettime, _clockid, _timespec)
#else
#define LxtClockGetTime(_clockid, _timespec) clock_gettime(_clockid, _timespec)
#endif

#define LxtClockGetRes(_clockid, _timespec) syscall(__NR_clock_getres, _clockid, _timespec)

#ifndef timerfd_create
#define timerfd_create(ClockId, Flags) syscall(__NR_timerfd_create, ClockId, Flags)
#endif

#ifndef timerfd_gettime
#define timerfd_gettime(Fd, CurrentValue) syscall(__NR_timerfd_gettime, Fd, CurrentValue)
#endif

#ifndef timerfd_settime
#define timerfd_settime(Fd, Flags, NewValue, OldValue) syscall(__NR_timerfd_settime, Fd, Flags, NewValue, OldValue)
#endif

#define LXT_CLONE_FLAGS_DEFAULT (CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD | CLONE_SYSVSEM)

typedef long long kernel_sigset_t;

typedef struct _LXT_RT_SIG_ACTION
{
    void* Handler;
    int Flags;
    void* Restorer;
    kernel_sigset_t Mask;
} LXT_RT_SIG_ACTION, *PLXT_RT_SIG_ACTION;

typedef struct _LXT_CLONE_ARGS
{
    char* Stack;
    pid_t CloneId;
} LXT_CLONE_ARGS, *PLXT_CLONE_ARGS;

typedef struct _LXT_PIPE
{
    int Read;
    int Write;
} LXT_PIPE, *PLXT_PIPE;

typedef struct _LXT_SOCKET_PAIR
{
    int Parent;
    int Child;
} LXT_SOCKET_PAIR, *PLXT_SOCKET_PAIR;

int LxtAcceptPoll(int Socket);

int LxtClone(int (*Entry)(void* Parameter), void* Parameter, int Flags, PLXT_CLONE_ARGS Args);

int LxtClosePipe(PLXT_PIPE Pipe);

int LxtCreatePipe(PLXT_PIPE Pipe);

int LxtExecuteAndReadOutput(char** Argv, char* OutputBuffer, size_t OutputBufferSize);

int LxtExecuteWslPath(char* Path, bool WinPath, char* OutputBuffer, size_t OutputBufferSize);

int LxtJoinThread(pid_t* Tid);

int LxtReceiveMessage(int Socket, const char* ExpectedMessage);

int LxtSendMessage(int Socket, const char* Message);

int LxtSignalBlock(int Signal);

int LxtSignalDefault(int Signal);

int LxtSignalIgnore(int Signal);

int LxtSignalCheckInfoReceived(int Signal, int Code, pid_t Pid, uid_t Uid);

int LxtSignalCheckNoSignal(void);

int LxtSignalCheckReceived(int Signal);

int LxtSignalCheckSigChldReceived(int Code, pid_t Pid, uid_t Uid, int Status);

int LxtSignalGetCount(void);

int LxtSignalGetInfo(siginfo_t* SignalInfo);

int LxtSignalInitialize(void);

int LxtSignalInitializeThread(void);

void LxtSignalResetReceived(void);

void LxtSignalSetAllowMultiple(BOOLEAN AllowMultiple);

int LxtSignalSetupHandler(int Signal, int Flags);

int LxtSignalTimedWait(sigset_t* Set, siginfo_t* SignalInfo, struct timespec* Timeout);

int LxtSignalUnblock(int Signal);

void LxtSignalWait(void);

int LxtSignalWaitBlocked(int Signal, pid_t FromPid, int TimeoutSeconds);

int LxtSignalCheckSigChldReceived(int Code, pid_t Pid, uid_t Uid, int Status);

int LxtSocketPairClose(PLXT_SOCKET_PAIR SocketPair);

int LxtSocketPairCloseChild(PLXT_SOCKET_PAIR SocketPair);

int LxtSocketPairCloseParent(PLXT_SOCKET_PAIR SocketPair);

int LxtSocketPairCreate(PLXT_SOCKET_PAIR SocketPair);

int LxtWaitPidPoll(pid_t ChildPid, int ExpectedWaitStatus);

int LxtWaitPidPollOptions(pid_t ChildPid, int ExpectedWaitStatus, int Options, int TimeoutSeconds);

int LxtClose(int FileDescriptor);

int LxtMunmap(void* Address, size_t Length);

int LxtWslVersion(void);

#endif // _LXT_UTIL
