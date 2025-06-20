/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    unittests.c

Abstract:

    This file contains the entrypoint for the unittest executable.

--*/

#include "unittests.h"
#include <stdio.h>
#include <string.h>

static const LXT_TEST LxtTests[] = {
    {"auxv", false, AuxvTestEntry},
    {"binfmt", false, BinFmtTestEntry},
    {"brk", false, BrkTestEntry},
    {"cgroup", false, CgroupTestEntry},
    {"dev_pt", false, DevPtTestEntry},
    {"dev_pt_2", false, DevPtTwoTestEntry},
    {"drvfs", false, DrvfsTestEntry},
    {"dup", false, DupTestEntry},
    {"epoll", false, EpollTestEntry},
    {"eventfd", false, EventfdTestEntry},
    {"execve", true, .Handler = {.TestHandlerEnvp = ExecveTestEntry}},
    {"flock", false, FlockTestEntry},
    {"fork", false, ForkTestEntry},
    {"fscommon", false, FsCommonTestEntry},
    {"fstab", false, FstabTestEntry},
    {"get_set_id", false, GetSetIdTestEntry},
    {"get_addr_info", false, GetSetIdTestEntry},
    {"get_time", false, GetTimeTestEntry},
    {"inotify", false, InotifyTestEntry},
    {"interop", false, InteropTestEntry},
    {"ioprio", false, IoprioTestEntry},
    {"keymgmt", false, KeymgmtTestEntry},
    {"madvise", false, MadviseTestEntry},
    {"mprotect", false, MprotectTestEntry},
    {"mremap", false, MremapTestEntry},
    {"namespace", false, NamespaceTestEntry},
    {"netlink", false, NetlinkTestEntry},
    {"overlayfs", false, OverlayFsTestEntry},
    {"pipe", false, PipeTestEntry},
    {"poll", false, PollTestEntry},
    {"random", false, RandomTestEntry},
    {"resourcelimits", false, ResourceLimitsTestEntry},
    {"sched", false, SchedTestEntry},
#if !defined(__aarch64__)
    {"select", false, SelectTestEntry},
#endif
    {"sem", false, SemTestEntry},
    {"shm", false, ShmTestEntry},
    {"socket_nonblock", false, SocketNonblockTestEntry},
    {"splice", false, SpliceTestEntry},
    {"sysfs", false, SysfsTestEntry},
    {"sysinfo", false, SysInfoTestEntry},
    {"timer", false, TimerTestEntry},
    {"timerfd", false, TimerFdTestEntry},
    {"tty", false, TtyTestEntry},
    {"ttys", false, TtysTestEntry},
    {"user", false, UserTestEntry},
    {"utimensat", false, UtimensatTestEntry},
    {"vfsaccess", false, VfsAccessTestEntry},
    {"vnet", false, VnetTestEntry},
    {"waitpid", false, WaitPidTestEntry},
    {"wslpath", false, WslPathTestEntry},
    {"xattr", false, XattrTestEntry}};

int main(int Argc, char* Argv[], char** Envp)
{

    unsigned int Itr = 0;
    int Result = 1;

    // parse arguments as:
    // [0 - binary name] [1 - test name] [2.. - test name params]
    if (Argc < 2)
    {
        // throw error
        LxtLogError(
            "Error: too few arguments\n \
            example usage: ./unittests [testname] [testparam1] [testparam..]\n");
        goto ErrorExit;
    }

    // decrement Argc when passing it to the test
    // tests view themselves as the binaries being called
    // main(Argc = 3, Argv = ["unittests", false, "exampletest", false, "param1"]) -->
    // ex. ExampleTestEntry(Argc = 2, Argv = ["exampletest", false, "param1"])
    Argc--;

    for (Itr = 0; Itr < LXT_COUNT_OF(LxtTests); Itr++)
    {
        if (strcmp(Argv[1], LxtTests[Itr].Name) == 0)
        {
            Result = (LxtTests[Itr].Envp) ? (LxtTests[Itr].Handler.TestHandlerEnvp(Argc, Argv + 1, Envp))
                                          : (LxtTests[Itr].Handler.TestHandler(Argc, Argv + 1));

            if (LXT_SUCCESS(Result))
            {
                LxtLogPassed("%s", false, LxtTests[Itr].Name);
            }
            else
            {
                LxtLogError("%s", false, LxtTests[Itr].Name);
            }

            goto ErrorExit;
        }
    }

    LxtLogError("Test name [%s] could not be recognized", Argv[1]);

ErrorExit:
    return Result;
}