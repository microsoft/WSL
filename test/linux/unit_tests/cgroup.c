/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    cgroup.c

Abstract:

    This file contains unit tests for cgroup support.

    N.B. This test depends on libmount, which is part of the libmount-dev
         apt package.

    N.B. To pass on native Linux this test requires cgroups to not be managed by
         an OS daemon. cgclear can be used to remove some subsystems.

--*/

#include "lxtcommon.h"
#include "unittests.h"
#include <sys/mount.h>
#include <linux/capability.h>
#include <libgen.h>
#include <fcntl.h>
#include <stdlib.h>
#include <libmount/libmount.h>
#include <sys/mman.h>
#include "lxtmount.h"
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>

#define LXT_NAME "cgroup"
#define CGROUP_TEST_PATH "/data"
#define CGROUP_TEST_MOUNT_NAME "cgroup"
#define CGROUP_TEST_MOUNT_POINT_NAME "cgroup_mount_test"
#define CGROUP_TEST_MOUNT_POINT CGROUP_TEST_PATH "/" CGROUP_TEST_MOUNT_POINT_NAME
#define CGROUP_TEST_MOUNT_POINT2 "/sys/fs/cgroup"

#define CGROUP_TEST_MOUNT_POINT_DIR1_NAME "dir1"
#define CGROUP_TEST_MOUNT_POINT_DIR1 CGROUP_TEST_MOUNT_POINT "/" CGROUP_TEST_MOUNT_POINT_DIR1_NAME
#define CGROUP_TEST_MOUNT_POINT2_DIR1 CGROUP_TEST_MOUNT_POINT2 "/" CGROUP_TEST_MOUNT_POINT_DIR1_NAME
#define CGROUP_TEST_MOUNT_POINT_DIR1_CHILD_NAME "child"
#define CGROUP_TEST_MOUNT_POINT_DIR1_CHILD CGROUP_TEST_MOUNT_POINT_DIR1 "/" CGROUP_TEST_MOUNT_POINT_DIR1_CHILD_NAME

#define CGROUP_TEST_DEFAULT_BUFFER_SIZE 128
#define CGROUP_TEST_MAX_CGROUPS 12
#define CGROUP_TEST_MAX_NAME_LENGTH 32

#define CGROUP_TEST_MAX_PIDS 2048

#define CGROUP_TEST_DEVICES_DEFAULT_LIST "a *:* rwm\n"

LXT_VARIATION_HANDLER CgroupTestBasicMount;
LXT_VARIATION_HANDLER CgroupTestMkdir;
LXT_VARIATION_HANDLER CgroupTestThreads;
LXT_VARIATION_HANDLER CgroupTestProcfs;
LXT_VARIATION_HANDLER CgroupTestProcsFile;
LXT_VARIATION_HANDLER CgroupTestMountReuse;
LXT_VARIATION_HANDLER CgroupTestDevices;

//
// TODO_LX: Enable all files when supported.
//

static const LXT_CHILD_INFO g_CgroupRootChildren[] = {
    {"cgroup.sane_behavior", DT_REG},
    /* {"cgroup.clone_children", DT_REG},
    {"cgroup.event_control", DT_REG}, */
    {"cgroup.procs", DT_REG}};
/* {"notify_on_release", DT_REG},
 {"release_agent", DT_REG},
 {"tasks", DT_REG}};*/

static const LXT_CHILD_INFO g_CgroupDefaultChildren[] = {/* {"cgroup.clone_children", DT_REG},
                                                         {"cgroup.event_control", DT_REG}, */
                                                         {"cgroup.procs", DT_REG}};
/* {"notify_on_release", DT_REG},
 {"release_agent", DT_REG},
 {"tasks", DT_REG}};*/

static const LXT_CHILD_INFO g_CgroupDevicesChildren[] = {{"devices.allow", DT_REG}, {"devices.deny", DT_REG}, {"devices.list", DT_REG}};

//
// Global constants
//

static const LXT_VARIATION g_LxtVariations[] = {
    {"cgroup - basic mount", CgroupTestBasicMount},
    {"cgroup - mkdir", CgroupTestMkdir},
    {"cgroup - threads", CgroupTestThreads},
    {"cgroup - procfs", CgroupTestProcfs},
    {"cgroup - cgroup.procs file", CgroupTestProcsFile},
    {"cgroup - mount reuse", CgroupTestMountReuse},
    {"cgroup - devices subsystem", CgroupTestDevices}};

static int g_TestPathMountId;

int CgroupTestEntry(int Argc, char* Argv[])

/*++
--*/

{

    LXT_ARGS Args;
    int Result;

    //
    // Clean-up from previous iterations.
    //

    rmdir(CGROUP_TEST_MOUNT_POINT_DIR1_CHILD);
    rmdir(CGROUP_TEST_MOUNT_POINT_DIR1);
    umount(CGROUP_TEST_MOUNT_POINT2);
    umount(CGROUP_TEST_MOUNT_POINT);
    rmdir(CGROUP_TEST_MOUNT_POINT);
    rmdir(CGROUP_TEST_MOUNT_POINT2);

    //
    // Run the test variations.
    //

    LxtCheckResult(g_TestPathMountId = MountGetMountId(CGROUP_TEST_PATH));
    LxtCheckResult(LxtInitialize(Argc, Argv, &Args, LXT_NAME));
    LxtCheckResult(LxtRunVariations(&Args, g_LxtVariations, LXT_COUNT_OF(g_LxtVariations)));

    //
    // Mount cgroup with a folder to test the instance uninitialize path.
    //

    LxtCheckErrnoZeroSuccess(mkdir(CGROUP_TEST_MOUNT_POINT, 0777));
    LxtCheckErrnoZeroSuccess(mount("mycgroupnew", CGROUP_TEST_MOUNT_POINT, CGROUP_TEST_MOUNT_NAME, 0, "devices"));

    LxtCheckErrno(mkdir(CGROUP_TEST_MOUNT_POINT_DIR1, 0777));

ErrorExit:
    LxtUninitialize();
    return !LXT_SUCCESS(Result);
}

int CgroupTestBasicMount(PLXT_ARGS Args)

/*++

Description:

    This routine tests the mount and umount system calls for cgroups.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int MountId;
    int Result;

    //
    // Create the directory and ensure it's not a mount point yet.
    //
    LxtCheckErrnoZeroSuccess(mkdir(CGROUP_TEST_MOUNT_POINT, 0777));
    LxtCheckResult(MountCheckIsNotMount(CGROUP_TEST_MOUNT_POINT));

    //
    // Mount a cgroup instance and check it was mounted.
    //

    LxtCheckErrnoZeroSuccess(mount("mycgroupnew", CGROUP_TEST_MOUNT_POINT, CGROUP_TEST_MOUNT_NAME, 0, NULL));

    LxtCheckResult(
        MountId = MountCheckIsMount(
            CGROUP_TEST_MOUNT_POINT,
            g_TestPathMountId,
            "mycgroupnew",
            CGROUP_TEST_MOUNT_NAME,
            "/",
            "rw,relatime",
            "rw,devices",
            "rw,relatime,devices",
            0));

    //
    // Mounting again should fail.
    //

    LxtCheckErrnoFailure(mount("mycgroupnew", CGROUP_TEST_MOUNT_POINT, CGROUP_TEST_MOUNT_NAME, 0, NULL), EBUSY);

    LxtCheckErrnoFailure(mount("mycgroupnew", CGROUP_TEST_MOUNT_POINT, CGROUP_TEST_MOUNT_NAME, 0, "devices"), EBUSY);

    //
    // Unmount and check it was unmounted.
    //

    LxtCheckErrnoZeroSuccess(umount(CGROUP_TEST_MOUNT_POINT));
    LxtCheckResult(MountCheckIsNotMount(CGROUP_TEST_MOUNT_POINT));

ErrorExit:
    umount(CGROUP_TEST_MOUNT_POINT);
    rmdir(CGROUP_TEST_MOUNT_POINT);
    return Result;
}

int CgroupTestMkdir(PLXT_ARGS Args)

/*++

Description:

    This routine tests the mkdir and rmdir system calls for cgroups.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    LXT_CHILD_INFO ChildInfo;
    char ChildPidBuffer[CGROUP_TEST_DEFAULT_BUFFER_SIZE];
    int ChildPidBufferLength;
    char Path[512];
    int ProcsFd;
    int MountId;
    int Result;

    ProcsFd = -1;

    //
    // Mount cgroup.
    //

    LxtCheckErrnoZeroSuccess(mkdir(CGROUP_TEST_MOUNT_POINT, 0777));
    LxtCheckErrnoZeroSuccess(mount("mycgroupnew", CGROUP_TEST_MOUNT_POINT, CGROUP_TEST_MOUNT_NAME, 0, "devices"));

    //
    // Removing the mount point root directory should fail.
    //

    LxtCheckErrnoFailure(rmdir(CGROUP_TEST_MOUNT_POINT), EBUSY);

    //
    // Create two subdirectories.
    //

    LxtCheckResult(LxtCheckDirectoryContents(CGROUP_TEST_MOUNT_POINT, g_CgroupRootChildren, LXT_COUNT_OF(g_CgroupRootChildren)));

    LxtCheckErrno(mkdir(CGROUP_TEST_MOUNT_POINT_DIR1, 0777));
    ChildInfo.FileType = DT_DIR;
    ChildInfo.Name = CGROUP_TEST_MOUNT_POINT_DIR1_NAME;
    LxtCheckResult(LxtCheckDirectoryContents(CGROUP_TEST_MOUNT_POINT, &ChildInfo, 1));

    LxtCheckErrno(mkdir(CGROUP_TEST_MOUNT_POINT_DIR1_CHILD, 0777));
    ChildInfo.Name = CGROUP_TEST_MOUNT_POINT_DIR1_CHILD_NAME;
    LxtCheckResult(LxtCheckDirectoryContents(CGROUP_TEST_MOUNT_POINT_DIR1, &ChildInfo, 1));

    //
    // Removing the first directory should fail if the second one still exists,
    // otherwise it succeeds.
    //

    LxtCheckErrnoFailure(rmdir(CGROUP_TEST_MOUNT_POINT_DIR1), EBUSY);
    LxtCheckErrno(rmdir(CGROUP_TEST_MOUNT_POINT_DIR1_CHILD));
    LxtCheckErrno(rmdir(CGROUP_TEST_MOUNT_POINT_DIR1));

    //
    // Check that removing the first directory fails if a thread is still
    // associated; otherwise, it succeeds.
    //

    LxtCheckErrno(mkdir(CGROUP_TEST_MOUNT_POINT_DIR1, 0777));
    sprintf(Path, "%s/%s", CGROUP_TEST_MOUNT_POINT_DIR1, "cgroup.procs");
    LxtCheckErrno(ProcsFd = open(Path, O_WRONLY));
    ChildPidBufferLength = sprintf(ChildPidBuffer, "%d\n", getpid());
    LxtCheckErrno(write(ProcsFd, ChildPidBuffer, ChildPidBufferLength));
    LxtClose(ProcsFd);
    ProcsFd = -1;
    LxtCheckErrnoFailure(rmdir(CGROUP_TEST_MOUNT_POINT_DIR1), EBUSY);
    sprintf(Path, "%s/%s", CGROUP_TEST_MOUNT_POINT, "cgroup.procs");
    LxtCheckErrno(ProcsFd = open(Path, O_WRONLY));
    LxtCheckErrno(write(ProcsFd, ChildPidBuffer, ChildPidBufferLength));
    LxtClose(ProcsFd);
    ProcsFd = -1;
    LxtCheckErrno(rmdir(CGROUP_TEST_MOUNT_POINT_DIR1));

    //
    // Unmount cgroup.
    //

    LxtCheckErrnoZeroSuccess(umount(CGROUP_TEST_MOUNT_POINT));
    LxtCheckResult(MountCheckIsNotMount(CGROUP_TEST_MOUNT_POINT));

ErrorExit:
    if (ProcsFd != -1)
    {
        LxtClose(ProcsFd);
    }

    rmdir(CGROUP_TEST_MOUNT_POINT_DIR1_CHILD);
    rmdir(CGROUP_TEST_MOUNT_POINT_DIR1);
    umount(CGROUP_TEST_MOUNT_POINT);
    rmdir(CGROUP_TEST_MOUNT_POINT);
    return Result;
}

int CgroupTestThreads(PLXT_ARGS Args)

/*++

Description:

    This routine tests thread behavior with cgroups mounts.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    pid_t ChildPid;
    int MountId;
    LXT_PIPE Pipe = {-1, -1};
    int Result;

    //
    // Create a thread, mount cgroup, and signal the thread to exit to test
    // cgroup assignment during mount.
    //

    LxtCheckResult(LxtCreatePipe(&Pipe));
    LxtCheckResult(ChildPid = fork());
    if (ChildPid == 0)
    {
        read(Pipe.Read, &Result, sizeof(Result));
        _exit(Result);
    }

    LxtCheckErrnoZeroSuccess(mkdir(CGROUP_TEST_MOUNT_POINT, 0777));
    LxtCheckErrnoZeroSuccess(mount("mycgroupnew", CGROUP_TEST_MOUNT_POINT, CGROUP_TEST_MOUNT_NAME, 0, NULL));

    write(Pipe.Write, &Result, sizeof(Result));
    LxtWaitPidPoll(ChildPid, 0);

    //
    // Create a thread to test cgroup inheritance.
    //

    LxtCheckResult(ChildPid = fork());
    if (ChildPid == 0)
    {
        _exit(Result);
    }

    LxtWaitPidPoll(ChildPid, 0);

    //
    // Unmount and exit.
    //

    LxtCheckErrnoZeroSuccess(umount(CGROUP_TEST_MOUNT_POINT));
    LxtCheckResult(MountCheckIsNotMount(CGROUP_TEST_MOUNT_POINT));

ErrorExit:
    write(Pipe.Write, &Result, sizeof(Result));
    LxtClosePipe(&Pipe);
    umount(CGROUP_TEST_MOUNT_POINT);
    rmdir(CGROUP_TEST_MOUNT_POINT);
    return Result;
}

typedef struct _CGROUP_TEST_PROCFS_ENTRY
{
    char Name[CGROUP_TEST_MAX_NAME_LENGTH];
    int Hierarchy;
    int NumCgroups;
    int Enabled;
} CGROUP_TEST_PROCFS_ENTRY, *PCGROUP_TEST_PROCFS_ENTRY;

typedef struct _CGROUP_TEST_PROCFS
{
    int EntryCount;
    CGROUP_TEST_PROCFS_ENTRY Entries[CGROUP_TEST_MAX_CGROUPS];
} CGROUP_TEST_PROCFS, *PCGROUP_TEST_PROCFS;

int CgroupTestReadProcfs(PCGROUP_TEST_PROCFS Data)

/*++

Description:

    This routine parses /proc/cgroups.

Arguments:

    Data - Supplies a buffer to store the data

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    char Line[CGROUP_TEST_DEFAULT_BUFFER_SIZE];
    FILE* CgroupFile;
    int NumCgroups;
    int Result;

    memset(Data, 0, sizeof(*Data));
    CgroupFile = fopen("/proc/cgroups", "r");
    LxtCheckNotEqual(CgroupFile, NULL, "%p");
    if (fgets(Line, LXT_COUNT_OF(Line), CgroupFile) == NULL)
    {
        Result = LXT_RESULT_FAILURE;
        LxtLogError("Failed to read header");
        goto ErrorExit;
    }

    LxtCheckStringEqual(Line, "#subsys_name\thierarchy\tnum_cgroups\tenabled\n");
    NumCgroups = 0;
    while (fgets(Line, LXT_COUNT_OF(Line), CgroupFile) != NULL)
    {
        sscanf(
            Line,
            "%s\t%d\t%d\t%d",
            Data->Entries[NumCgroups].Name,
            &Data->Entries[NumCgroups].Hierarchy,
            &Data->Entries[NumCgroups].NumCgroups,
            &Data->Entries[NumCgroups].Enabled);

        NumCgroups += 1;
    }

    Data->EntryCount = NumCgroups;

ErrorExit:
    if (CgroupFile != NULL)
    {
        fclose(CgroupFile);
    }

    return Result;
}

typedef struct _CGROUP_TEST_PROCFS_PID_ENTRY
{
    int Hierarchy;
    char Subsystems[CGROUP_TEST_DEFAULT_BUFFER_SIZE];
    char CgroupPath[CGROUP_TEST_DEFAULT_BUFFER_SIZE];
} CGROUP_TEST_PROCFS_PID_ENTRY, *PCGROUP_TEST_PROCFS_PID_ENTRY;

typedef struct _CGROUP_TEST_PROCFS_PID
{
    int EntryCount;
    CGROUP_TEST_PROCFS_PID_ENTRY Entries[CGROUP_TEST_MAX_CGROUPS];
} CGROUP_TEST_PROCFS_PID, *PCGROUP_TEST_PROCFS_PID;

int CgroupTestReadProcfsPid(PCGROUP_TEST_PROCFS_PID Data)

/*++

Description:

    This routine parses /proc/self/cgroup.

Arguments:

    Data - Supplies a buffer to store the data

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    char Line[CGROUP_TEST_DEFAULT_BUFFER_SIZE];
    FILE* CgroupFile;
    int NumCgroups;
    int Result;

    memset(Data, 0, sizeof(*Data));
    CgroupFile = fopen("/proc/self/cgroup", "r");
    LxtCheckNotEqual(CgroupFile, NULL, "%p");

    NumCgroups = 0;
    while (fgets(Line, LXT_COUNT_OF(Line), CgroupFile) != NULL)
    {
        sscanf(
            Line,
            "%d:%[^:]:%[^:\n]",
            &Data->Entries[NumCgroups].Hierarchy,
            Data->Entries[NumCgroups].Subsystems,
            Data->Entries[NumCgroups].CgroupPath);

        NumCgroups += 1;
    }

    Data->EntryCount = NumCgroups;

ErrorExit:
    if (CgroupFile != NULL)
    {
        fclose(CgroupFile);
    }

    return Result;
}

int CgroupTestProcfs(PLXT_ARGS Args)

/*++

Description:

    This routine tests the cgroup procfs files.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int BytesRead;
    int Found;
    int Index;
    int MountId;
    CGROUP_TEST_PROCFS ProcfsNew;
    CGROUP_TEST_PROCFS ProcfsOrig;
    CGROUP_TEST_PROCFS_PID ProcfsPidNew;
    CGROUP_TEST_PROCFS_PID ProcfsPidOrig;
    int Result;

    //
    // Create the cgroup mount.
    //

    LxtCheckErrnoZeroSuccess(mkdir(CGROUP_TEST_MOUNT_POINT, 0777));
    LxtCheckErrnoZeroSuccess(mount("mycgroupnew", CGROUP_TEST_MOUNT_POINT, CGROUP_TEST_MOUNT_NAME, 0, "devices"));

    //
    // Read the procfs files before cgroups are mounted, staring
    // with /proc/cgroup.
    //

    LxtCheckResult(CgroupTestReadProcfs(&ProcfsNew));
    LxtCheckNotEqual(ProcfsNew.EntryCount, 0, "%d");
    Found = 0;
    for (Index = 0; Index < ProcfsNew.EntryCount; ++Index)
    {
        LxtCheckNotEqual(ProcfsNew.Entries[Index].NumCgroups, 0, "%d");
        LxtCheckEqual(ProcfsNew.Entries[Index].Enabled, 1, "%d");
        if (strcmp(ProcfsNew.Entries[Index].Name, "devices") == 0)
        {
            LxtCheckNotEqual(Found, 1, "%d");
            LxtCheckNotEqual(ProcfsNew.Entries[Index].Hierarchy, 0, "%d");
            Found = 1;
        }
    }

    LxtCheckEqual(Found, 1, "%d");

    //
    // Now /proc/self/cgroup.
    //

    LxtCheckResult(CgroupTestReadProcfsPid(&ProcfsPidNew));
    Found = 0;
    for (Index = 0; Index < ProcfsPidNew.EntryCount; ++Index)
    {
        if (strstr(ProcfsPidNew.Entries[Index].Subsystems, "devices") != NULL)
        {
            LxtCheckNotEqual(Found, 1, "%d");
            LxtCheckNotEqual(ProcfsPidNew.Entries[Index].Hierarchy, 0, "%d");
            LxtCheckStringEqual(ProcfsPidNew.Entries[Index].CgroupPath, "/");
            Found = 1;
        }
    }

    LxtCheckEqual(Found, 1, "%d");

    //
    // Unmount and recheck the original.
    //

    LxtCheckErrnoZeroSuccess(umount(CGROUP_TEST_MOUNT_POINT));
    LxtCheckResult(MountCheckIsNotMount(CGROUP_TEST_MOUNT_POINT));

ErrorExit:
    umount(CGROUP_TEST_MOUNT_POINT);
    rmdir(CGROUP_TEST_MOUNT_POINT);
    return Result;
}

int CgroupTestGetProcsFileIds(char* CgroupPath, pid_t* IdArray[], int* IdArrayCount)

/*++

Description:

    This routine tests the behavior of the cgroup.procs file.

Arguments:

    Path - Supplies the path to query.

    IdArray - Supplies a buffer to store the array of ids.

    IdArrayCount - Supplies a buffer to store the number of ids.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    pid_t* Array;
    int Count;
    int Index;
    FILE* File;
    char Line[CGROUP_TEST_DEFAULT_BUFFER_SIZE];
    char Path[512];
    int PathLength;
    int Result;

    Array = NULL;
    File = NULL;
    sprintf(Path, "%s/%s", CgroupPath, "cgroup.procs");
    File = fopen(Path, "r");
    LxtCheckNotEqual(File, NULL, "%p");
    Array = LxtAlloc(sizeof(*Array) * CGROUP_TEST_MAX_PIDS);
    if (Array == NULL)
    {
        Result = LXT_RESULT_FAILURE;
        goto ErrorExit;
    }

    Count = 0;
    while (fgets(Line, LXT_COUNT_OF(Line), File) != NULL)
    {
        if (Count > CGROUP_TEST_MAX_PIDS)
        {
            LxtLogError("Unexpected count");
            Result = LXT_RESULT_FAILURE;
            goto ErrorExit;
        }

        sscanf(Line, "%d\n", &Array[Count]);
        Count += 1;
    }

    for (Index = 1; Index < Count; ++Index)
    {
        if (Array[Index - 1] >= Array[Index])
        {
            LxtLogError("Unexpected value %d, %d", Array[Index - 1], Array[Index]);
            Result = LXT_RESULT_FAILURE;
            goto ErrorExit;
        }
    }

    *IdArray = Array;
    Array = NULL;
    *IdArrayCount = Count;

ErrorExit:
    if (File != NULL)
    {
        fclose(File);
    }

    if (Array != NULL)
    {
        LxtFree(Array);
    }

    return Result;
}

int CgroupTestProcsFile(PLXT_ARGS Args)

/*++

Description:

    This routine tests the behavior of the cgroup.procs file.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    pid_t* Array;
    pid_t ChildPid;
    char ChildPidBuffer[CGROUP_TEST_DEFAULT_BUFFER_SIZE];
    int ChildPidBufferLength;
    int Count;
    int Index;
    int MountId;
    char Path[512];
    int ProcsFd;
    LXT_PIPE Pipe = {-1, -1};
    int Result;

    Array = NULL;
    ProcsFd = -1;
    LxtCheckErrnoZeroSuccess(mkdir(CGROUP_TEST_MOUNT_POINT, 0777));
    LxtCheckErrnoZeroSuccess(mount("mycgroupnew", CGROUP_TEST_MOUNT_POINT, CGROUP_TEST_MOUNT_NAME, 0, "devices"));

    //
    // Create a threadgroup and check that it is in the root folder.
    //

    LxtCheckResult(LxtCreatePipe(&Pipe));
    LxtCheckResult(ChildPid = fork());
    if (ChildPid == 0)
    {
        read(Pipe.Read, &Result, sizeof(Result));
        _exit(0);
    }

    LxtCheckResult(CgroupTestGetProcsFileIds(CGROUP_TEST_MOUNT_POINT, &Array, &Count));
    for (Index = 0; Index < Count; ++Index)
    {
        if (Array[Index] == ChildPid)
        {
            break;
        }
    }

    LxtCheckNotEqual(Index, Count, "%d");
    LxtFree(Array);
    Array = NULL;

    //
    // Create a folder and check that it is empty.
    //

    rmdir(CGROUP_TEST_MOUNT_POINT_DIR1);
    LxtCheckErrno(mkdir(CGROUP_TEST_MOUNT_POINT_DIR1, 0777));
    LxtCheckResult(CgroupTestGetProcsFileIds(CGROUP_TEST_MOUNT_POINT_DIR1, &Array, &Count));
    LxtCheckEqual(0, Count, "%d");
    LxtFree(Array);
    Array = NULL;

    //
    // Move the thread to the folder and check that the thread was moved.
    //

    sprintf(Path, "%s/%s", CGROUP_TEST_MOUNT_POINT_DIR1, "cgroup.procs");
    LxtCheckErrno(ProcsFd = open(Path, O_WRONLY));
    ChildPidBufferLength = sprintf(ChildPidBuffer, "%d\n", ChildPid);
    LxtCheckErrno(write(ProcsFd, ChildPidBuffer, ChildPidBufferLength));
    LxtCheckResult(CgroupTestGetProcsFileIds(CGROUP_TEST_MOUNT_POINT_DIR1, &Array, &Count));
    for (Index = 0; Index < Count; ++Index)
    {
        if (Array[Index] == ChildPid)
        {
            break;
        }
    }

    LxtCheckNotEqual(Index, Count, "%d");
    LxtFree(Array);
    Array = NULL;

    LxtCheckResult(CgroupTestGetProcsFileIds(CGROUP_TEST_MOUNT_POINT, &Array, &Count));
    for (Index = 0; Index < Count; ++Index)
    {
        if (Array[Index] == ChildPid)
        {
            break;
        }
    }

    LxtCheckEqual(Index, Count, "%d");
    LxtFree(Array);
    Array = NULL;
    LxtClose(ProcsFd);
    ProcsFd = -1;

    //
    // Move the thread to the root and check that the thread was moved.
    //

    sprintf(Path, "%s/%s", CGROUP_TEST_MOUNT_POINT, "cgroup.procs");
    LxtCheckErrno(ProcsFd = open(Path, O_WRONLY));
    ChildPidBufferLength = sprintf(ChildPidBuffer, "%d\n", ChildPid);
    LxtCheckErrno(write(ProcsFd, ChildPidBuffer, ChildPidBufferLength));
    LxtCheckResult(CgroupTestGetProcsFileIds(CGROUP_TEST_MOUNT_POINT_DIR1, &Array, &Count));
    LxtCheckEqual(0, Count, "%d");
    LxtFree(Array);
    Array = NULL;

    LxtCheckResult(CgroupTestGetProcsFileIds(CGROUP_TEST_MOUNT_POINT, &Array, &Count));
    for (Index = 0; Index < Count; ++Index)
    {
        if (Array[Index] == ChildPid)
        {
            break;
        }
    }

    LxtCheckNotEqual(Index, Count, "%d");
    LxtFree(Array);
    Array = NULL;
    LxtClose(ProcsFd);
    ProcsFd = -1;

    //
    // Unmount and exit.
    //

    rmdir(CGROUP_TEST_MOUNT_POINT_DIR1);
    LxtCheckErrnoZeroSuccess(umount(CGROUP_TEST_MOUNT_POINT));
    LxtCheckResult(MountCheckIsNotMount(CGROUP_TEST_MOUNT_POINT));

ErrorExit:
    write(Pipe.Write, &Result, sizeof(Result));
    LxtClosePipe(&Pipe);
    if (Array != NULL)
    {
        LxtFree(Array);
    }

    if (ProcsFd != -1)
    {
        LxtClose(ProcsFd);
    }

    umount(CGROUP_TEST_MOUNT_POINT);
    rmdir(CGROUP_TEST_MOUNT_POINT);
    rmdir(CGROUP_TEST_MOUNT_POINT_DIR1);
    return Result;
}

int CgroupTestMountReuse(PLXT_ARGS Args)

/*++

Description:

    This routine tests the behavior of reusing cgroup hierarchy mounts.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    LXT_CHILD_INFO ChildInfo;
    char ChildPidBuffer[CGROUP_TEST_DEFAULT_BUFFER_SIZE];
    int ChildPidBufferLength;
    int Found;
    int Index;
    char Path[512];
    int ProcsFd;
    CGROUP_TEST_PROCFS ProcfsNew;
    int MountId;
    int Result;

    ProcsFd = -1;

    rmdir(CGROUP_TEST_MOUNT_POINT_DIR1_CHILD);
    rmdir(CGROUP_TEST_MOUNT_POINT_DIR1);
    umount(CGROUP_TEST_MOUNT_POINT);
    rmdir(CGROUP_TEST_MOUNT_POINT);

    //
    // Mount cgroup.
    //

    LxtCheckErrnoZeroSuccess(mkdir(CGROUP_TEST_MOUNT_POINT, 0777));
    LxtCheckErrnoZeroSuccess(mount("mycgroupnew", CGROUP_TEST_MOUNT_POINT, CGROUP_TEST_MOUNT_NAME, 0, "devices"));

    //
    // A cgroup with a directory should be reported as active when unmounted
    //

    LxtCheckErrno(mkdir(CGROUP_TEST_MOUNT_POINT_DIR1, 0777));
    LxtCheckErrno(access(CGROUP_TEST_MOUNT_POINT_DIR1, F_OK));
    LxtCheckErrnoZeroSuccess(umount(CGROUP_TEST_MOUNT_POINT));
    LxtCheckErrnoFailure(access(CGROUP_TEST_MOUNT_POINT_DIR1, F_OK), ENOENT);
    LxtCheckResult(CgroupTestReadProcfs(&ProcfsNew));
    LxtCheckNotEqual(ProcfsNew.EntryCount, 0, "%d");
    Found = 0;
    for (Index = 0; Index < ProcfsNew.EntryCount; ++Index)
    {
        LxtCheckNotEqual(ProcfsNew.Entries[Index].NumCgroups, 0, "%d");
        LxtCheckEqual(ProcfsNew.Entries[Index].Enabled, 1, "%d");
        if (strcmp(ProcfsNew.Entries[Index].Name, "devices") == 0)
        {
            LxtCheckNotEqual(Found, 1, "%d");
            LxtCheckNotEqual(ProcfsNew.Entries[Index].Hierarchy, 0, "%d");
            Found = 1;
        }
    }
    LxtCheckEqual(Found, 1, "%d");

    //
    // When remounted the directory is present
    //

    LxtCheckErrnoZeroSuccess(mount("mycgroupnew", CGROUP_TEST_MOUNT_POINT, CGROUP_TEST_MOUNT_NAME, 0, "devices"));

    LxtCheckErrno(access(CGROUP_TEST_MOUNT_POINT_DIR1, F_OK));

    //
    // When that cgroup is mounted again, the directory should be present
    //

    LxtCheckErrnoZeroSuccess(mount("mycgroupnew", CGROUP_TEST_MOUNT_POINT2, CGROUP_TEST_MOUNT_NAME, 0, "devices"));

    LxtCheckErrno(access(CGROUP_TEST_MOUNT_POINT2_DIR1, F_OK));
    umount(CGROUP_TEST_MOUNT_POINT2);

    //
    // Failing variation to check the mount all case.
    //
    // TODO_LX: This variation needs to be updated once multiple subsystems are
    //          supported.
    //

    LxtCheckErrnoZeroSuccess(mount("mycgroupnew", CGROUP_TEST_MOUNT_POINT2, CGROUP_TEST_MOUNT_NAME, 0, NULL));

    LxtCheckErrno(access(CGROUP_TEST_MOUNT_POINT2_DIR1, F_OK));

    //
    // Unmount and exit.
    //

    rmdir(CGROUP_TEST_MOUNT_POINT_DIR1);
    LxtCheckErrnoZeroSuccess(umount(CGROUP_TEST_MOUNT_POINT2));
    LxtCheckErrnoZeroSuccess(umount(CGROUP_TEST_MOUNT_POINT));
    LxtCheckResult(MountCheckIsNotMount(CGROUP_TEST_MOUNT_POINT));

ErrorExit:
    if (ProcsFd != -1)
    {
        LxtClose(ProcsFd);
    }

    rmdir(CGROUP_TEST_MOUNT_POINT_DIR1_CHILD);
    rmdir(CGROUP_TEST_MOUNT_POINT_DIR1);
    umount(CGROUP_TEST_MOUNT_POINT2);
    umount(CGROUP_TEST_MOUNT_POINT);
    rmdir(CGROUP_TEST_MOUNT_POINT);
    rmdir(CGROUP_TEST_MOUNT_POINT2);
    return Result;
}

int CgroupTestDevices(PLXT_ARGS Args)

/*++

Description:

    This routine tests the files for the devices subsystem.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int BytesRead;
    int CgroupFd;
    char DevicesListBuffer[32];
    char Path[512];
    char PidBuffer[CGROUP_TEST_DEFAULT_BUFFER_SIZE];
    int PidBufferLength;
    int Result;

    CgroupFd = -1;
    PidBufferLength = sprintf(PidBuffer, "%d\n", getpid());

    //
    // Mount cgroup.
    //

    LxtCheckErrnoZeroSuccess(mkdir(CGROUP_TEST_MOUNT_POINT, 0777));
    LxtCheckErrnoZeroSuccess(mount("mycgroupnew", CGROUP_TEST_MOUNT_POINT, CGROUP_TEST_MOUNT_NAME, 0, "devices"));

    //
    // Check for the expected default files and devices files in the root.
    //

    LxtCheckResult(LxtCheckDirectoryContents(CGROUP_TEST_MOUNT_POINT, g_CgroupRootChildren, LXT_COUNT_OF(g_CgroupRootChildren)));

    LxtCheckResult(LxtCheckDirectoryContents(CGROUP_TEST_MOUNT_POINT, g_CgroupDevicesChildren, LXT_COUNT_OF(g_CgroupDevicesChildren)));

    //
    // Check for the expected default files and devices files in a subdirectory.
    //
    // N.B. A thread has to exist in the cgroup for some files to successfully
    //      be read.
    //

    LxtCheckErrno(mkdir(CGROUP_TEST_MOUNT_POINT_DIR1, 0777));
    sprintf(Path, "%s/%s", CGROUP_TEST_MOUNT_POINT_DIR1, "cgroup.procs");
    LxtCheckErrno(CgroupFd = open(Path, O_WRONLY));
    LxtCheckErrno(write(CgroupFd, PidBuffer, PidBufferLength));
    LxtClose(CgroupFd);
    CgroupFd = -1;
    LxtCheckResult(LxtCheckDirectoryContents(CGROUP_TEST_MOUNT_POINT_DIR1, g_CgroupDefaultChildren, LXT_COUNT_OF(g_CgroupDefaultChildren)));

    LxtCheckResult(LxtCheckDirectoryContents(CGROUP_TEST_MOUNT_POINT_DIR1, g_CgroupDevicesChildren, LXT_COUNT_OF(g_CgroupDevicesChildren)));

    //
    // Check for the expected value of the devices.list file in both folders.
    //

    sprintf(Path, "%s/%s", CGROUP_TEST_MOUNT_POINT, "devices.list");
    LxtCheckErrno(CgroupFd = open(Path, O_RDONLY));
    LxtCheckErrno(BytesRead = read(CgroupFd, DevicesListBuffer, sizeof(DevicesListBuffer) - 1));
    DevicesListBuffer[BytesRead] = 0;
    LxtClose(CgroupFd);
    CgroupFd = -1;
    LxtCheckStringEqual(DevicesListBuffer, CGROUP_TEST_DEVICES_DEFAULT_LIST);

    sprintf(Path, "%s/%s", CGROUP_TEST_MOUNT_POINT_DIR1, "devices.list");
    LxtCheckErrno(CgroupFd = open(Path, O_RDONLY));
    LxtCheckErrno(BytesRead = read(CgroupFd, DevicesListBuffer, sizeof(DevicesListBuffer) - 1));
    DevicesListBuffer[BytesRead] = 0;
    LxtClose(CgroupFd);
    CgroupFd = -1;
    LxtCheckStringEqual(DevicesListBuffer, CGROUP_TEST_DEVICES_DEFAULT_LIST);

    //
    // Unmount cgroup.
    //

    sprintf(Path, "%s/%s", CGROUP_TEST_MOUNT_POINT, "cgroup.procs");
    LxtCheckErrno(CgroupFd = open(Path, O_WRONLY));
    LxtCheckErrno(write(CgroupFd, PidBuffer, PidBufferLength));
    LxtClose(CgroupFd);
    CgroupFd = -1;
    rmdir(CGROUP_TEST_MOUNT_POINT_DIR1);
    LxtCheckErrnoZeroSuccess(umount(CGROUP_TEST_MOUNT_POINT));
    LxtCheckResult(MountCheckIsNotMount(CGROUP_TEST_MOUNT_POINT));

ErrorExit:
    if (CgroupFd != -1)
    {
        LxtClose(CgroupFd);
    }

    rmdir(CGROUP_TEST_MOUNT_POINT_DIR1);
    umount(CGROUP_TEST_MOUNT_POINT);
    rmdir(CGROUP_TEST_MOUNT_POINT);
    return Result;
}
