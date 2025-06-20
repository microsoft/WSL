/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    Sysinfo.c

Abstract:

    This file is a sysinfo test.

--*/

#include "lxtcommon.h"
#include "unittests.h"
#include "linux/kernel.h"

#define LXT_NAME "Sysinfo"

extern int sysinfo(struct sysinfo* info);

int SysinfoVariationPrint(PLXT_ARGS Args);

//
// Global constants
//

static const LXT_VARIATION g_LxtVariations[] = {{"SysinfoVariationPrint", SysinfoVariationPrint}};

int SysInfoTestEntry(int Argc, char* Argv[])

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

int SysinfoVariationPrint(PLXT_ARGS Args)

/*++
--*/

{

    int Result;
    struct sysinfo SysInfo;

    LxtCheckErrnoFailure(sysinfo(NULL), EFAULT);
    LxtCheckErrno(sysinfo(&SysInfo));
    LxtLogInfo(
        "SysInfo.uptime: %d\n"
        "SysInfo.loads[0]: %d\n"
        "SysInfo.loads[1]: %d\n"
        "SysInfo.loads[2]: %d\n"
        "SysInfo.totalram: %d\n"
        "SysInfo.freeram: %d\n"
        "SysInfo.sharedram: %d\n"
        "SysInfo.bufferram: %d\n"
        "SysInfo.totalswap: %d\n"
        "SysInfo.freeswap: %d\n"
        "SysInfo.procs: %d\n"
        "SysInfo.pad: %d\n"
        "SysInfo.totalhigh: %d\n"
        "SysInfo.freehigh: %d\n"
        "SysInfo.mem_unit: %d\n",
        SysInfo.uptime,
        SysInfo.loads[0],
        SysInfo.loads[1],
        SysInfo.loads[2],
        SysInfo.totalram,
        SysInfo.freeram,
        SysInfo.sharedram,
        SysInfo.bufferram,
        SysInfo.totalswap,
        SysInfo.freeswap,
        SysInfo.procs,
        SysInfo.pad,
        SysInfo.totalhigh,
        SysInfo.freehigh,
        SysInfo.mem_unit);

    LxtCheckGreater(SysInfo.uptime, 0, "%d");
    LxtCheckEqual(SysInfo.loads[0], 33984, "%d");
    LxtCheckEqual(SysInfo.loads[1], 37856, "%d");
    LxtCheckEqual(SysInfo.loads[2], 38400, "%d");
    LxtCheckGreater(SysInfo.totalram, 0, "%d");
    LxtCheckGreater(SysInfo.freeram, 0, "%d");
    LxtCheckEqual(SysInfo.sharedram, 0, "%d");
    LxtCheckEqual(SysInfo.bufferram, 0, "%d");
    LxtCheckGreater(SysInfo.totalswap, 0, "%d");
    LxtCheckGreater(SysInfo.freeswap, 0, "%d");
    LxtCheckGreater(SysInfo.procs, 1, "%d"); // TODO: change back to 2, right now there is only a single process running.
    LxtCheckEqual(SysInfo.pad, 0, "%d");
    LxtCheckEqual(SysInfo.totalhigh, 139208 * 1024, "%d");
    LxtCheckEqual(SysInfo.freehigh, 272 * 1024, "%d");
    LxtCheckEqual(SysInfo.mem_unit, 1, "%d");

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    return Result;
}
