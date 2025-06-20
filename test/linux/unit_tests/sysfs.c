/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    sysfs.c

Abstract:

    This file contains tests for the sysfs file system.

--*/

#include "lxtcommon.h"
#include "unittests.h"
#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>

#define LXT_NAME "SysFs"

#define SYSFS_MNT "/sys"

int SysFsClassNet(PLXT_ARGS Args);

int SysFsDevicesSystemCpu(PLXT_ARGS Args);

int SysFsDevicesVirtualNet(PLXT_ARGS Args);

int SysFsKernelDebug(PLXT_ARGS Args);

int SysFsRoot(PLXT_ARGS Args);

//
// Global constants
//

static const LXT_VARIATION g_LxtVariations[] = {
    {"SysFs - /sys root", SysFsRoot},
    {"SysFs - /sys/class/net", SysFsClassNet},
    {"SysFs - /sys/devices/virtual/net", SysFsDevicesVirtualNet},
    {"SysFs - /sys/devices/system/cpu", SysFsDevicesSystemCpu},
    {"SysFs - /sys/kernel/debug", SysFsKernelDebug},
};

static const LXT_CHILD_INFO g_SysFsRootChildren[] = {
    {"block", DT_DIR},
    {"bus", DT_DIR},
    {"class", DT_DIR},
    {"dev", DT_DIR},
    {"devices", DT_DIR},
    {"firmware", DT_DIR},
    {"fs", DT_DIR},
    {"kernel", DT_DIR},
    {"module", DT_DIR},
    {"power", DT_DIR}};

static const LXT_CHILD_INFO g_SysFsClassNetChildren[] = {{"lo", DT_LNK}};

static const LXT_CHILD_INFO g_SysFsDevicesVirtualNetChildren[] = {{"lo", DT_DIR}};

static const LXT_CHILD_INFO g_SysFsDevicesVirtualNetDeviceChildren[] = {
    {"address", DT_REG}, {"ifindex", DT_REG}, {"flags", DT_REG}, {"mtu", DT_REG}};

static const LXT_CHILD_INFO g_SysFsDevicesSystemCpuChildren[] = {{"cpu0", DT_DIR}, {"present", DT_REG}, {"possible", DT_REG}};

static const LXT_CHILD_INFO g_SysFsDevicesSystemCpuDeviceChildren[] = {{"topology", DT_DIR}};

static const LXT_CHILD_INFO g_SysFsDevicesSystemCpuDeviceCpuFreqChildren[] = {{"cpuinfo_max_freq", DT_REG}, {"scaling_max_freq", DT_REG}};

static const LXT_CHILD_INFO g_SysFsDevicesSystemCpuDeviceTopologyChildren[] = {
    {"core_id", DT_REG},
    {"core_siblings", DT_REG},
    {"core_siblings_list", DT_REG},
    {"physical_package_id", DT_REG},
    {"thread_siblings", DT_REG},
    {"thread_siblings_list", DT_REG}};

static const LXT_CHILD_INFO g_SysFsKernelDebugChildren[] = {{"tracing", DT_DIR}, {"wakeup_sources", DT_REG}};

static const LXT_CHILD_INFO g_SysFsKernelDebugTracingChildren[] = {{"trace_marker", DT_REG}};

static const LXT_CHILD_INFO g_SysFsKernelIp4Children[] = {
    {"tcp_rmem_min", DT_REG}, {"tcp_rmem_def", DT_REG}, {"tcp_rmem_max", DT_REG}, {"tcp_wmem_min", DT_REG}, {"tcp_wmem_def", DT_REG}, {"tcp_wmem_max", DT_REG}};

static const LXT_CHILD_INFO g_SysFsModuleLowmemorykillerChildren[] = {{"parameters", DT_DIR}};

static const LXT_CHILD_INFO g_SysFsModuleLowmemorykillerParametersChildren[] = {{"adj", DT_REG}, {"minfree", DT_REG}};

static const LXT_CHILD_INFO g_SysFsPowerChildren[] = {{"autosleep", DT_REG}};

int SysfsTestEntry(int Argc, char* Argv[])

/*++

Routine Description:

    This routine is the main entry point for the procfs tests.

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

int SysFsClassNet(PLXT_ARGS Args)

/*++

Description:

    This routine tests the sysfs network class directory (/sys/class/net).

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int Result;

    //
    // This test may fail on real Linux because the contents are not
    // guaranteed to be the same on every system.
    //

    LxtCheckResult(LxtCheckDirectoryContents(SYSFS_MNT "/class/net", g_SysFsClassNetChildren, LXT_COUNT_OF(g_SysFsClassNetChildren)));

    LxtCheckResult(LxtCheckLinkTarget(SYSFS_MNT "/class/net/lo", "../../devices/virtual/net/lo"));

ErrorExit:
    return Result;
}

int SysFsDevicesSystemCpu(PLXT_ARGS Args)

/*++

Description:

    This routine tests the cpu device directory (/sys/devices/system/cpu).

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    char Buffer[100];
    char ChildPath[256];
    int CpuInfoProcessorCount;
    ssize_t BytesRead;
    int Fd;
    char* Line;
    int ProcessorIndex;
    int ProcessorCount;
    int Result;
    size_t Size;
    FILE* Stream;

    //
    // First check always present contents.
    //

    Fd = 0;
    Stream = NULL;
    Line = NULL;
    LxtCheckResult(LxtCheckDirectoryContents(
        SYSFS_MNT "/devices/system/cpu", g_SysFsDevicesSystemCpuChildren, LXT_COUNT_OF(g_SysFsDevicesSystemCpuChildren)));

    //
    // Determine the number of CPUs.
    //

    LxtCheckErrno(Fd = open(SYSFS_MNT "/devices/system/cpu/present", O_RDONLY));
    LxtCheckErrno(BytesRead = read(Fd, Buffer, sizeof(Buffer)));
    LxtCheckEqual(Buffer[0], '0', "%c");
    ProcessorCount = 1;
    if (Buffer[1] != '\n')
    {
        LxtCheckEqual(Buffer[1], '-', "%c");
        ProcessorCount = strtol(&Buffer[2], NULL, 10);
        LxtCheckGreater(ProcessorCount, 0, "%d");
        ProcessorCount += 1;
    }

    LxtCheckErrnoZeroSuccess(close(Fd));

    //
    // Check CPU directory for each CPU.
    //

    for (ProcessorIndex = 0; ProcessorIndex < ProcessorCount; ProcessorIndex += 1)
    {

        snprintf(ChildPath, sizeof(ChildPath), SYSFS_MNT "/devices/system/cpu/cpu%d", ProcessorIndex);

        LxtCheckResult(LxtCheckDirectoryContents(
            ChildPath, g_SysFsDevicesSystemCpuDeviceChildren, LXT_COUNT_OF(g_SysFsDevicesSystemCpuDeviceChildren)));

        snprintf(ChildPath, sizeof(ChildPath), SYSFS_MNT "/devices/system/cpu/cpu%d/topology", ProcessorIndex);

        LxtCheckResult(LxtCheckDirectoryContents(
            ChildPath, g_SysFsDevicesSystemCpuDeviceTopologyChildren, LXT_COUNT_OF(g_SysFsDevicesSystemCpuDeviceTopologyChildren)));
    }

    //
    // No more directories should exist.
    //

    snprintf(ChildPath, sizeof(ChildPath), SYSFS_MNT "/devices/system/cpu/cpu%d", ProcessorCount);

    LxtCheckErrnoFailure(open(ChildPath, O_RDONLY | O_DIRECTORY), ENOENT);

    //
    // Check if the number of CPUs matches /proc/cpuinfo.
    //

    Stream = fopen("/proc/cpuinfo", "r");
    if (Stream == NULL)
    {
        LxtLogError("fopen failed, errno: %d", errno);
        Result = LXT_RESULT_FAILURE;
        goto ErrorExit;
    }

    Size = 0;
    CpuInfoProcessorCount = 0;
    while (getline(&Line, &Size, Stream) != -1)
    {
        if (strstr(Line, "processor") == Line)
        {
            CpuInfoProcessorCount += 1;
        }
    }

    LxtCheckEqual(ProcessorCount, CpuInfoProcessorCount, "%d");

ErrorExit:
    if (Line != NULL)
    {
        free(Line);
    }

    if (Stream != NULL)
    {
        fclose(Stream);
    }

    if (Fd > 0)
    {
        close(Fd);
    }

    return Result;
}

int SysFsDevicesVirtualNet(PLXT_ARGS Args)

/*++

Description:

    This routine tests the sysfs network device directory
    (/sys/devices/virtual/net).

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int Result;

    //
    // This test may fail on real Linux because the contents are not
    // guaranteed to be the same on every system.
    //

    LxtCheckResult(LxtCheckDirectoryContents(
        SYSFS_MNT "/devices/virtual/net", g_SysFsDevicesVirtualNetChildren, LXT_COUNT_OF(g_SysFsDevicesVirtualNetChildren)));

    LxtCheckResult(LxtCheckDirectoryContents(
        SYSFS_MNT "/devices/virtual/net/lo", g_SysFsDevicesVirtualNetDeviceChildren, LXT_COUNT_OF(g_SysFsDevicesVirtualNetDeviceChildren)));

ErrorExit:
    return Result;
}

int SysFsKernelDebug(PLXT_ARGS Args)

/*++

Description:

    This routine tests the debug directory (/sys/kernel/debug).

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int Result;

    LxtCheckResult(LxtCheckDirectoryContents(SYSFS_MNT "/kernel/debug", g_SysFsKernelDebugChildren, LXT_COUNT_OF(g_SysFsKernelDebugChildren)));

    LxtCheckResult(LxtCheckDirectoryContents(
        SYSFS_MNT "/kernel/debug/tracing", g_SysFsKernelDebugTracingChildren, LXT_COUNT_OF(g_SysFsKernelDebugTracingChildren)));

    LxtCheckResult(LxtCheckWrite(SYSFS_MNT "/kernel/debug/tracing/trace_marker", "bogus"));

ErrorExit:
    return Result;
}

int SysFsRoot(PLXT_ARGS Args)

/*++

Description:

    This routine tests the sysfs root directory (/sys).

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int Result;

    LxtCheckResult(LxtCheckStat(SYSFS_MNT, 1, DT_DIR));
    LxtCheckResult(LxtCheckDirectoryContents(SYSFS_MNT, g_SysFsRootChildren, LXT_COUNT_OF(g_SysFsRootChildren)));

ErrorExit:
    return Result;
}
