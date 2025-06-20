/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    unittests.h

Abstract:

    This file contains definitions for the unittest executable.

--*/

#pragma once

#include "lxtutil.h"

// Define test names

#define AUXV_TESTNAME "auxv"
#define BINFMT_TESTNAME "binfmt"
#define BRK_TESTNAME "brk"
#define CGROUP_TESTNAME "cgroup"
#define DEV_PT_TESTNAME "dev_pt"
#define DEV_PT_TWO_TESTNAME "dev_pt_2"
#define DRVFS_TESTNAME "drvfs"
#define DUP_TESTNAME "dup"
#define EPOLL_TESTNAME "epoll"
#define EVENTFD_TESTNAME "eventfd"
#define EXECVE_TESTNAME "execve"
#define FLOCK_TESTNAME "flock"
#define FORK_TESTNAME "fork"
#define FSCOMMON_TESTNAME "fscommon"
#define FSTAB_TESTNAME "fstab"
#define GET_SET_ID_TESTNAME "get_set_id"
#define GETADDRINFO_TESTNAME "get_addr_info"
#define GETTIME_TESTNAME "get_time"
#define INOTIFY_TESTNAME "inotify"
#define INTEROP_TESTNAME "interop"
#define IOPRIO_TESTNAME "ioprio"
#define KEYMGMT_TESTNAME "keymgmt"
#define MADVISE_TESTNAME "madvise"
#define MOUNT_TESTNAME "mount"
#define MPROTECT_TESTNAME "mprotect"
#define MREMAP_TESTNAME "mremap"
#define NAMESPACE_TESTNAME "namespace"
#define NETLINK_TESTNAME "netlink"
#define OVERLAYFS_TESTNAME "overlayfs"
#define PIPE_TESTNAME "pipe"
#define POLL_TESTNAME "poll"
#define PTRACE_TESTNAME "ptrace"
#define RANDOM_TESTNAME "random"
#define RESOURCELIMITS_TESTNAME "resource_limits"
#define SCHED_TESTNAME "sched"
#define SELECT_TESTNAME "select"
#define SEM_TESTNAME "sem"
#define SHM_TESTNAME "shm"
#define SOCKET_NONBLOCK_TESTNAME "socket_nonblock"
#define SPLICE_TESTNAME "splice"
#define SYSFS_TESTNAME "sysfs"
#define SYS_INFO_TESTNAME "sysinfo"
#define TIMER_TESTNAME "timer"
#define TIMERFD_TESTNAME "timerfd"
#define TTY_TESTNAME "tty"
#define TTYS_TESTNAME "ttys"
#define USER_TESTNAME "user"
#define UTIMENSAT_TESTNAME "utimensat"
#define VFSACCESS_TESTNAME "vfsaccess"
#define VNET_TESTNAME "vnet"
#define WAITPID_TESTNAME "waitpid"
#define WSLPATH_TESTNAME "wslpath"
#define XATTR_TESTNAME "xattr"

// define path to unit test binary
#define WSL_UNIT_TEST_BINARY "/data/test/wsl_unit_tests"

// #define FUNCTIONS_H_INCLUDED
// #ifndef FUNCTIONS_H_INCLUDED

// Define each test's entry point

int AuxvTestEntry(int Argc, char* Argv[]);

int BinFmtTestEntry(int Argc, char* Argv[]);

int BrkTestEntry(int Argc, char* Argv[]);

int CgroupTestEntry(int Argc, char* Argv[]);

int DevPtTestEntry(int Argc, char* Argv[]);

int DevPtTwoTestEntry(int Argc, char* Argv[]);

int DrvfsTestEntry(int Argc, char* Argv[]);

int DupTestEntry(int Argc, char* Argv[]);

int EpollTestEntry(int Argc, char* Argv[]);

int EventfdTestEntry(int Argc, char* Argv[]);

int ExecveTestEntry(int Argc, char* Argv[], char** Envp);

int FlockTestEntry(int Argc, char* Argv[]);

int ForkTestEntry(int Argc, char* Argv[]);

int FsCommonTestEntry(int Argc, char* Argv[]);

int FstabTestEntry(int Argc, char* Argv[]);

int FutexTestEntry(int Argc, char* Argv[]);

int GetSetIdTestEntry(int Argc, char* Argv[]);

int GetAddrInfoTestEntry(int Argc, char* Argv[]);

int GetTimeTestEntry(int Argc, char* Argv[]);

int InotifyTestEntry(int Argc, char* Argv[]);

int InteropTestEntry(int Argc, char* Argv[]);

int IoprioTestEntry(int Argc, char* Argv[]);

int KeymgmtTestEntry(int Argc, char* Argv[]);

int MadviseTestEntry(int Argc, char* Argv[]);

int MprotectTestEntry(int Argc, char* Argv[]);

int MremapTestEntry(int Argc, char* Argv[]);

int NamespaceTestEntry(int Argc, char* Argv[]);

int NetlinkTestEntry(int Argc, char* Argv[]);

int OverlayFsTestEntry(int Argc, char* Argv[]);

int PipeTestEntry(int Argc, char* Argv[]);

int PollTestEntry(int Argc, char* Argv[]);

int RandomTestEntry(int Argc, char* Argv[]);

int ResourceLimitsTestEntry(int Argc, char* Argv[]);

int SchedTestEntry(int Argc, char* Argv[]);

int SelectTestEntry(int Argc, char* Argv[]);

int SemTestEntry(int Argc, char* Argv[]);

int ShmTestEntry(int Argc, char* Argv[]);

int SocketNonblockTestEntry(int Argc, char* Argv[]);

int SpliceTestEntry(int Argc, char* Argv[]);

int SysfsTestEntry(int Argc, char* Argv[]);

int SysInfoTestEntry(int Argc, char* Argv[]);

int TimerTestEntry(int Argc, char* Argv[]);

int TimerFdTestEntry(int Argc, char* Argv[]);

int TtyTestEntry(int Argc, char* Argv[]);

int TtysTestEntry(int Argc, char* Argv[]);

int UserTestEntry(int Argc, char* Argv[]);

int UtimensatTestEntry(int Argc, char* Argv[]);

int VfsAccessTestEntry(int Argc, char* Argv[]);

int VnetTestEntry(int Argc, char* Argv[]);

int WaitPidTestEntry(int Argc, char* Argv[]);

int WslPathTestEntry(int Argc, char* Argv[]);

int XattrTestEntry(int Argc, char* Argv[]);

// #endif

// for parsing which test to use

typedef int LXT_TEST_HANDLER(int Argc, char* Argv[]);

typedef int LXT_TEST_HANDLER_ENVP(int Argc, char* Argv[], char** Envp);

typedef union _TEST_HANDLER_UNION
{
    LXT_TEST_HANDLER* TestHandler;
    LXT_TEST_HANDLER_ENVP* TestHandlerEnvp;
} LXT_TEST_HANDLER_UNION;

typedef struct _LXT_TEST
{
    const char* Name;
    const bool Envp;
    LXT_TEST_HANDLER_UNION Handler;
} LXT_TEST;
