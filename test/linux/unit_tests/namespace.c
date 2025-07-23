/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    Namespace.c

Abstract:

    This file is a Namespace test.

--*/

#include "lxtcommon.h"
#include "unittests.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <dirent.h>
#include <ctype.h>
#include <sys/mman.h>
#include <sys/utsname.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <net/if.h>
#include <bits/local_lim.h>
#include <linux/capability.h>
#include <linux/futex.h>
#include <linux/net_namespace.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/reboot.h>

#define LXT_NAME "Namespace"
#define SOCKET_LOOPBACK_IF_NAME "lo"

#define LxtReboot(_magic1, _magic2, _cmd, _arg) (syscall(SYS_reboot, (_magic1), (_magic2), (_cmd), (_arg)))

int NamespaceNetwork(PLXT_ARGS Args);

int NamespaceNetworkProcfs(PLXT_ARGS Args);

int NamespaceNetworkProcfsCheckFile(char* File, int ExpectedLineCount);

int NamespaceSetNs(PLXT_ARGS Args);

int NamespaceStat(PLXT_ARGS Args);

int NamespacePid(PLXT_ARGS Args);

int NamespacePidCheckProcPidStatStatusFiles(char* Dir);

void* NamespacePidChildPThread(void* Args);

int NamespacePidGetProcPidFolderCount(char* Dir, int* Count);

int NamespacePidBasic(int Level);

int NamespacePidBasicChild(void* Param);

int NamespacePidParentPThread(void);

int NamespaceUts(PLXT_ARGS Args);

int NamespaceIpc(PLXT_ARGS Args);

int NamespaceCloneInvalid(PLXT_ARGS Args);

//
// Global constants
//

static const LXT_VARIATION g_LxtVariations[] = {
    {"NamespaceSetNs", NamespaceSetNs},
    {"NamespaceStat", NamespaceStat},
    {"Namespace UTS", NamespaceUts},
    {"Namespace PID", NamespacePid},
    {"Namespace Network", NamespaceNetwork},
    {"Namespace Network - reading /proc/<pid>/net", NamespaceNetworkProcfs},
    {"Namespace IPC", NamespaceIpc},
    {"Namespace Clone - invalid namespace flags", NamespaceCloneInvalid}};

typedef struct _LXT_NAMESPACE_DATA
{
    const char* Name;
    int NsType;
} LXT_NAMESPACE_DATA, *PLXT_NAMESPACE_DATA;

static const LXT_NAMESPACE_DATA g_LxtNamespaces[] = {
    {"ipc", CLONE_NEWIPC}, {"mnt", CLONE_NEWNS}, {"net", CLONE_NEWNET}, {"pid", CLONE_NEWPID}, {"user", CLONE_NEWUSER}, {"uts", CLONE_NEWUTS}};

int NamespaceTestEntry(int Argc, char* Argv[])

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

void NamespaceSetNsChild(int NsFd)

/*++
--*/

{

    struct __user_cap_data_struct CapData[2];
    struct __user_cap_header_struct CapHeader;
    int Result;

    //
    // Drop the CAP_SYS_ADMIN capability.
    //

    memset(&CapData, 0, sizeof(CapData));
    memset(&CapHeader, 0, sizeof(CapHeader));
    CapHeader.version = _LINUX_CAPABILITY_VERSION_3;
    LxtCheckErrno(LxtCapGet(&CapHeader, CapData)) CapData[CAP_TO_INDEX(CAP_SYS_ADMIN)].effective &= ~CAP_TO_MASK(CAP_SYS_ADMIN);
    LxtCheckErrno(LxtCapSet(&CapHeader, CapData));

    //
    // Try to setns without CAP_SYS_ADMIN.
    //

    LxtCheckErrnoFailure(setns(NsFd, 0), EPERM);

ErrorExit:
    _exit(Result);
}

int NamespaceSetNs(PLXT_ARGS Args)

/*++
--*/

{

    char Buffer[32];
    pid_t ChildPid;
    int Index;
    int NsFd;
    int Result;

    NsFd = -1;

    //
    // Pass invalid parameters to setns
    //

    LxtCheckErrnoFailure(setns(0, CLONE_NEWPID), EINVAL);
    LxtCheckErrnoFailure(setns(0, -1), EINVAL);
    LxtCheckErrnoFailure(setns(-1, 0), EBADF);

    //
    // Pass the self fds to sets.
    //

    for (Index = 0; Index < LXT_COUNT_OF(g_LxtNamespaces); ++Index)
    {
        snprintf(Buffer, sizeof(Buffer), "/proc/self/ns/%s", g_LxtNamespaces[Index].Name);
        printf("%s\n", Buffer);
        LxtCheckErrno(NsFd = open(Buffer, O_RDONLY));
        if (g_LxtNamespaces[Index].NsType != CLONE_NEWUSER)
        {
            LxtCheckErrno(setns(NsFd, 0));
            LxtCheckErrno(setns(NsFd, g_LxtNamespaces[Index].NsType));
            LxtCheckErrno(ChildPid = fork());
            if (ChildPid == 0)
            {
                NamespaceSetNsChild(NsFd);
            }

            LxtWaitPidPoll(ChildPid, 0);
        }
        else
        {
            LxtCheckErrnoFailure(setns(NsFd, 0), EINVAL);
            LxtCheckErrnoFailure(setns(NsFd, g_LxtNamespaces[Index].NsType), EINVAL);
        }

        LxtCheckErrnoFailure(setns(NsFd, g_LxtNamespaces[(Index + 1) % LXT_COUNT_OF(g_LxtNamespaces)].NsType), EINVAL);
        LxtCheckErrnoFailure(setns(NsFd, -1), EINVAL);
        LxtClose(NsFd);
        NsFd = -1;
    }

ErrorExit:
    if (NsFd != -1)
    {
        LxtClose(NsFd);
    }

    return Result;
}

int NamespaceStat(PLXT_ARGS Args)

{

    char Buffer[32];
    int Index;
    int NsFd;
    int Result;
    struct stat StatData;

    NsFd = -1;

    //
    // stat each namespace file and check the result.
    //

    for (Index = 0; Index < LXT_COUNT_OF(g_LxtNamespaces); ++Index)
    {
        snprintf(Buffer, sizeof(Buffer), "/proc/self/ns/%s", g_LxtNamespaces[Index].Name);
        printf("%s\n", Buffer);
        LxtCheckErrno(NsFd = open(Buffer, O_RDONLY));
        LxtCheckErrno(fstat(NsFd, &StatData));

        //
        // TODO: st_dev is reported as 0 for files.
        //

        LxtCheckEqual(major(StatData.st_dev), 0, "%d");
        LxtCheckNotEqual(StatData.st_ino, 0, "%d");
        LxtCheckNotEqual(StatData.st_mode, 0, "%d");
        LxtCheckEqual(StatData.st_nlink, 1, "%d");
        LxtCheckEqual(StatData.st_uid, 0, "%d");
        LxtCheckEqual(StatData.st_gid, 0, "%d");
        LxtCheckEqual(StatData.st_rdev, 0, "%d");
        LxtCheckEqual(StatData.st_size, 0, "%d");
        LxtCheckEqual(StatData.st_blksize, 4096, "%d");
        LxtCheckEqual(StatData.st_blocks, 0, "%d");
        LxtClose(NsFd);
        NsFd = -1;
    }

    Result = 0;

ErrorExit:
    if (NsFd != -1)
    {
        LxtClose(NsFd);
    }

    return Result;
}

int VerifyUtsData(struct utsname* ExpectedValues)

{

    struct utsname ActualValues;
    int Fd;
    int Length;
    char ProcfsDomain[HOST_NAME_MAX];
    char ProcfsHost[HOST_NAME_MAX];
    int Result;

    memset(&ActualValues, 0, sizeof(ActualValues));
    LxtCheckErrno(uname(&ActualValues));
    LxtCheckMemoryEqual(ExpectedValues, &ActualValues, sizeof(ActualValues));
    LxtCheckErrno(Fd = open("/proc/sys/kernel/hostname", O_RDONLY));
    LxtCheckErrno(Length = read(Fd, ProcfsHost, sizeof(ProcfsHost)));
    ProcfsHost[Length - 1] = 0;
    LxtClose(Fd);
    Fd = -1;
    LxtCheckStringEqual(ExpectedValues->nodename, ProcfsHost);
    LxtCheckErrno(Fd = open("/proc/sys/kernel/domainname", O_RDONLY));
    LxtCheckErrno(Length = read(Fd, ProcfsDomain, sizeof(ProcfsDomain)));
    ProcfsDomain[Length - 1] = 0;
    LxtClose(Fd);
    Fd = -1;
    LxtCheckStringEqual(ExpectedValues->domainname, ProcfsDomain);
    Result = 0;

ErrorExit:
    if (Fd != -1)
    {
        LxtClose(Fd);
    }

    return Result;
}

#define CHILD_HOST "childmachine"
#define CHILD_DOMAIN "childdomain"

int NamespaceUtsChild(struct utsname* ParentValues)

{

    int Fd;
    char Path[64];
    pid_t Ppid;
    int Result;
    struct utsname UtsBuffer;

    Fd = -1;

    //
    // Check the uts namespace behavior for before\after unshare.
    //

    memcpy(&UtsBuffer, ParentValues, sizeof(UtsBuffer));
    LxtCheckResult(VerifyUtsData(&UtsBuffer));
    LxtCheckErrno(unshare(CLONE_NEWUTS));
    LxtCheckResult(VerifyUtsData(&UtsBuffer));
    LxtCheckErrno(sethostname(CHILD_HOST, sizeof(CHILD_HOST)));
    LxtCheckErrno(setdomainname(CHILD_DOMAIN, sizeof(CHILD_DOMAIN)));
    memset(UtsBuffer.nodename, 0, sizeof(UtsBuffer.nodename));
    memcpy(UtsBuffer.nodename, CHILD_HOST, sizeof(CHILD_HOST));
    memset(UtsBuffer.domainname, 0, sizeof(UtsBuffer.domainname));
    memcpy(UtsBuffer.domainname, CHILD_DOMAIN, sizeof(CHILD_DOMAIN));
    LxtCheckResult(VerifyUtsData(&UtsBuffer));

    //
    // Check the uts namespace behavior after switching back to the parent
    // uts namespace.
    //

    Ppid = getppid();
    sprintf(Path, "/proc/%d/ns/uts", Ppid);
    LxtCheckErrno(Fd = open(Path, O_RDONLY));
    LxtCheckErrno(setns(Fd, CLONE_NEWUTS));
    memcpy(&UtsBuffer, ParentValues, sizeof(UtsBuffer));
    LxtCheckResult(VerifyUtsData(&UtsBuffer));

    Result = 0;

ErrorExit:
    if (Fd != -1)
    {
        LxtClose(Fd);
    }

    return Result;
}

void NamespaceUtsFork(struct utsname* ParentValues)

{

    int Result;

    Result = NamespaceUtsChild(ParentValues);
    _exit(Result);
}

void* NamespaceUtsThread(void* Args)

{

    struct utsname* ParentValues;
    int Result;

    ParentValues = Args;
    Result = NamespaceUtsChild(Args);
    pthread_exit(&Result);
}

int NamespaceUts(PLXT_ARGS Args)

{

    pid_t ChildPid;
    char* Name;
    char NameBuffer[HOST_NAME_MAX];
    int Result;
    pthread_t ThreadId = {0};
    struct utsname UtsBuffer;

    //
    // Check the UTS behavior for fork()
    //

    memset(&UtsBuffer, 0, sizeof(UtsBuffer));
    LxtCheckErrno(uname(&UtsBuffer));
    LxtCheckResult(VerifyUtsData(&UtsBuffer));
    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        NamespaceUtsFork(&UtsBuffer);
    }

    LxtWaitPidPoll(ChildPid, 0);
    LxtCheckResult(VerifyUtsData(&UtsBuffer));

    //
    // Check the UTS behavior for a pthread.
    //

    LxtCheckErrnoZeroSuccess(pthread_create(&ThreadId, NULL, NamespaceUtsThread, &UtsBuffer));

    pthread_join(ThreadId, NULL);
    LxtCheckResult(VerifyUtsData(&UtsBuffer));

    //
    // Test behavior for NULL names.
    //

    Name = NULL;
    LxtCheckErrno(sethostname(Name, 0));
    LxtCheckErrno(setdomainname(Name, 0));
    memset(NameBuffer, 1, sizeof(NameBuffer));
    LxtCheckErrno(gethostname(NameBuffer, sizeof(NameBuffer)));
    LxtCheckEqual(NameBuffer[0], 0, "%c");
    memset(NameBuffer, 1, sizeof(NameBuffer));
    LxtCheckErrno(getdomainname(NameBuffer, sizeof(NameBuffer)));
    LxtCheckEqual(NameBuffer[0], 0, "%c");

    Result = 0;

ErrorExit:
    return Result;
}

int NamespacePidCheckProcPidStatStatusFiles(char* Dir)

{

    char Command[80];
    int Gid;
    int IntValue;
    char Line[40];
    char Name[20];
    int Pid;
    int Ppid;
    int Result;
    char State[10];
    FILE* StatFile;
    char StatFileName[30];
    FILE* StatusFile;
    char StatusFileName[30];
    int Tid;
    int Tgid;

    //
    // Check the stat file in ProcFs.
    //

    sprintf(StatFileName, "%s/%s", Dir, "/stat");
    StatFile = fopen(StatFileName, "r");
    LxtCheckNotEqual(StatFile, NULL, "%p");
    LxtCheckEqual(fscanf(StatFile, "%d %s %s %d %d", &Tid, Command, State, &Ppid, &Gid), 5, "%d");
    LxtLogInfo("%d %s %s %d %d", Tid, Command, State, Ppid, Gid);
    LxtCheckEqual(Tid, 1, "%d");
    LxtCheckEqual(Ppid, 0, "%d");
    LxtCheckEqual(Gid, 0, "%d");

    //
    // Check the status file in ProcFs.
    //

    sprintf(StatusFileName, "%s/%s", Dir, "/status");
    StatusFile = fopen(StatusFileName, "r");
    LxtCheckNotEqual(StatusFile, NULL, "%p");
    Tgid = -1;
    Pid = -1;
    Ppid = -1;
    while (fgets(Line, LXT_COUNT_OF(Line), StatusFile) != NULL)
    {
        if (sscanf(Line, "%s %d", Name, &IntValue) == 2)
        {
            if (strcmp(Name, "Tgid:") == 0)
            {
                Tgid = IntValue;
            }
            else if (strcmp(Name, "Pid:") == 0)
            {
                Pid = IntValue;
            }
            else if (strcmp(Name, "PPid:") == 0)
            {
                Ppid = IntValue;
            }
        }
    }

    LxtCheckEqual(Tgid, 1, "%d");
    LxtCheckEqual(Pid, 1, "%d");
    LxtCheckEqual(Ppid, 0, "%d");
    Result = LXT_RESULT_SUCCESS;

ErrorExit:

    //
    // Intentionally do not close the files. These files are leaked to ensure
    // the private procfs instance is cleaned up correctly when there are
    // file descriptors that need to be closed during thread group end.
    //

    return Result;
}

void* NamespacePidChildPThread(void* Args)

{

    int Result;

    LxtLogError("Child pthread ran unexpectedly in new namespace");
    Result = -1;
    pthread_exit(&Result);
}

int NamespacePidGetProcPidFolderCount(char* Dir, int* Count)

{

    int CountLocal;
    struct dirent* DirEnt;
    DIR* Fd;
    char FullPath[257];
    int Result;
    struct stat StatBuffer;

    //
    // Get the number of /proc/<pid> folders.
    //

    Fd = opendir(Dir);
    if (Fd == NULL)
    {
        LxtLogError("opendir failed, errno: %d (%s)", errno, strerror(errno));
        Result = LXT_RESULT_FAILURE;
        goto ErrorExit;
    }

    CountLocal = 0;
    while ((DirEnt = readdir(Fd)) != NULL)
    {
        sprintf(FullPath, "%s/%s", Dir, DirEnt->d_name);
        LxtLogInfo("Calling stat() on proc folder %s", FullPath);
        LxtCheckErrnoZeroSuccess(stat(FullPath, &StatBuffer));

        //
        // Must be a directory, and its name must start with a digit.
        //

        if ((StatBuffer.st_mode & S_IFDIR) != S_IFDIR)
        {
            continue;
        }

        if (isdigit(DirEnt->d_name[0]) == FALSE)
        {
            continue;
        }

        CountLocal += 1;
    }

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    if (Fd != NULL)
    {
        closedir(Fd);
    }

    *Count = CountLocal;
    return Result;
}

int PidBasicPipesA[2];
int PidBasicPipesB[2];

#define NAMESPACE_PID_BASIC_TOKEN (0x12345678)

#define PID_BASIC_PARENT_PIPE_READ (PidBasicPipesA[0])
#define PID_BASIC_PARENT_PIPE_WRITE (PidBasicPipesB[1])
#define PID_BASIC_CHILD_PIPE_READ (PidBasicPipesB[0])
#define PID_BASIC_CHILD_PIPE_WRITE (PidBasicPipesA[1])

int NamespacePidBasic(int Level)

{

    pid_t ChildId;
    LXT_CLONE_ARGS CloneArgs;
    pid_t CurrentId;
    int Result;
    int Size;
    int Token;

    //
    // Create a set of pipes to synchronize with the child PID namespaces.
    //

    LxtCheckErrnoZeroSuccess(pipe(PidBasicPipesA));
    LxtCheckErrnoZeroSuccess(pipe(PidBasicPipesB));

    //
    // Clone a child into a new PID namespace.
    //

    LxtCheckResult(LxtClone(NamespacePidBasicChild, &Level, CLONE_NEWPID | SIGCHLD, &CloneArgs));

    //
    // Close the child pipes.
    //

    LxtCheckErrnoZeroSuccess(close(PID_BASIC_CHILD_PIPE_READ));
    PID_BASIC_CHILD_PIPE_READ = -1;
    LxtCheckErrnoZeroSuccess(close(PID_BASIC_CHILD_PIPE_WRITE));
    PID_BASIC_CHILD_PIPE_WRITE = -1;

    //
    // Wait for the entire hierarchy to be created.
    //

    LxtCheckErrno(Size = read(PID_BASIC_PARENT_PIPE_READ, &Token, sizeof(Token)));

    LxtCheckEqual(Size, sizeof(Token), "%d");
    LxtCheckEqual(Token, NAMESPACE_PID_BASIC_TOKEN, "%x");

    //
    // Validate the PGID of the child.
    //

    LxtCheckErrno(CurrentId = getpgid(0));
    LxtCheckErrno(ChildId = getpgid(CloneArgs.CloneId));
    LxtCheckEqual(CurrentId, ChildId, "%d");

    //
    // Validate the SID of the child.
    //

    LxtCheckErrno(CurrentId = getsid(0));
    LxtCheckErrno(ChildId = getsid(CloneArgs.CloneId));
    LxtCheckEqual(CurrentId, ChildId, "%d");

    //
    // Notify the hierarchy to exit and wait.
    //

    LxtCheckErrno(Size = write(PID_BASIC_PARENT_PIPE_WRITE, &Token, sizeof(Token)));

    LxtCheckEqual(Size, sizeof(Token), "%d");
    LxtCheckResult(LxtWaitPidPoll(CloneArgs.CloneId, 0));
    Result = 0;

ErrorExit:
    return Result;
}

int NamespacePidBasicChild(void* Param)

{

    pid_t ChildPid;
    LXT_CLONE_ARGS CloneArgs;
    int Level;
    pid_t Pgid;
    pid_t Pid;
    int PidFolderCount;
    pid_t Ppid;
    int Result;
    pid_t Sid;
    int Size;
    pid_t Tid;
    int Token;

    Level = *((int*)Param);

    //
    // Close the parent pipes.
    //

    if (Level == 0)
    {
        LxtCheckErrnoZeroSuccess(close(PID_BASIC_PARENT_PIPE_READ));
        PID_BASIC_PARENT_PIPE_READ = -1;
        LxtCheckErrnoZeroSuccess(close(PID_BASIC_PARENT_PIPE_WRITE));
        PID_BASIC_PARENT_PIPE_WRITE = -1;
    }

    usleep(1000 * 80);

    //
    // Validate that the first thread/threadgroup in a PID namespace has PID 1.
    //

    LxtCheckErrno(Tid = gettid());
    LxtCheckEqual(Tid, 1, "%d");
    LxtCheckErrno(Pid = getpid());
    LxtCheckEqual(Pid, 1, "%d");

    //
    // Validate that the first thread in a PID namespace cannot see its current
    // process group, session or parent.
    //

    LxtCheckErrno(Pgid = getpgid(0));
    LxtCheckEqual(Pgid, 0, "%d");
    LxtCheckErrno(Sid = getsid(0));
    LxtCheckEqual(Sid, 0, "%d");
    LxtCheckErrno(Ppid = getppid());
    LxtCheckEqual(Ppid, 0, "%d");

    //
    // Run the following in its own mount namespace and mark all mounts in the
    // new namespace private so changes cannot propagate to the rest of the
    // system.
    //

    LxtCheckErrno(unshare(CLONE_NEWNS));
    LxtCheckErrnoZeroSuccess(mount(NULL, "/", NULL, MS_PRIVATE | MS_REC, NULL));

    //
    // Re-mount /proc. This version of /proc should not contain any PIDs from
    // the parent PID namespace.
    //

    LxtCheckErrnoZeroSuccess(mount(NULL, "/proc", "proc", 0, NULL));

    //
    // Do some basic validation.
    //

    LxtCheckErrnoZeroSuccess(access("/proc", R_OK));
    LxtCheckErrnoZeroSuccess(access("/proc/1", R_OK));
    LxtCheckErrnoZeroSuccess(access("/proc/1/cmdline", R_OK));
    LxtCheckErrnoZeroSuccess(access("/proc/self", R_OK));
    LxtCheckErrnoZeroSuccess(access("/proc/self/cmdline", R_OK));
    LxtCheckErrnoFailure(access("/proc/0", R_OK), ENOENT);
    LxtCheckErrnoFailure(access("/proc/1234567890", R_OK), ENOENT);

    //
    // Check that there is only 1 /proc/<pid> folder.
    //

    LxtLogInfo("Checking /proc/<pid> folders, before clone, nested level %d", Level);
    LxtCheckResult(NamespacePidGetProcPidFolderCount("/proc", &PidFolderCount));
    LxtCheckEqual(PidFolderCount, 1, "%d");

    //
    // Check the /proc/1/stat, /proc/1/status, /proc/1/task/1/stat
    // and /proc/1/task/1/status files.
    //

    NamespacePidCheckProcPidStatStatusFiles("/proc/1/");
    NamespacePidCheckProcPidStatStatusFiles("/proc/1/task/1");

    //
    // Test nested PID namespaces.
    //

    if (Level < 3)
    {
        Level += 1;
        LxtCheckResult(LxtClone(NamespacePidBasicChild, &Level, CLONE_NEWPID | SIGCHLD, &CloneArgs));

        //
        // After the clone, check that there are now at least 2 /proc/<pid>
        // folders.
        //
        // N.B. The cloned process will recursively create more cloned PID
        //      namespaces, causing more PIDs to appear under this /proc mount.
        //

        LxtLogInfo("Checking /proc/<pid> folders, after clone, nested level %d", Level);
        LxtCheckResult(NamespacePidGetProcPidFolderCount("/proc", &PidFolderCount));
        LxtCheckGreaterOrEqual(PidFolderCount, 2, "%d");

        //
        // Check the /proc/1/stat, /proc/1/status, /proc/1/task/1/stat
        // and /proc/1/task/1/status files.
        //

        NamespacePidCheckProcPidStatStatusFiles("/proc/1/");
        NamespacePidCheckProcPidStatStatusFiles("/proc/1/task/1");

        //
        // Wait for the child to exit.
        //

        LxtCheckResult(LxtWaitPidPoll(CloneArgs.CloneId, 0));
    }
    else
    {

        //
        // Signal to the test that the hierarchy is created.
        //

        Token = NAMESPACE_PID_BASIC_TOKEN;
        LxtCheckErrno(Size = write(PID_BASIC_CHILD_PIPE_WRITE, &Token, sizeof(Token)));

        LxtCheckEqual(Size, sizeof(Token), "%d");

        //
        // Wait for the notification to exit.
        //

        LxtCheckErrno(Size = read(PID_BASIC_CHILD_PIPE_READ, &Token, sizeof(Token)));

        LxtCheckEqual(Size, sizeof(Token), "%d");
        LxtCheckEqual(Token, NAMESPACE_PID_BASIC_TOKEN, "%x");
    }

    Result = 0;

ErrorExit:
    return Result;
}

int NamespacePidParentPThread(void)

{

    pthread_t ThreadId = {0};
    int Result;

    //
    // Create a new child PID namespace and check that pthread creation fails.
    //

    LxtCheckErrno(unshare(CLONE_NEWPID));
    LxtCheckEqual(pthread_create(&ThreadId, NULL, NamespacePidChildPThread, NULL), EINVAL, "%d");

    Result = 0;

ErrorExit:
    return Result;
}

int NamespacePidTerminate(int Level, int* Ready)

{

    pid_t ChildPid;
    int Index;
    int Result;

    LxtLogInfo("[%d/%d] PID namespace leader", Level, getpid());

    //
    // Create 10 child threadgroups that loop sleeping.
    //

    for (Index = 0; Index < 10; Index += 1)
    {
        LxtCheckErrno(ChildPid = fork());
        if (ChildPid == 0)
        {
            for (;;)
            {
                sleep(-1);
            }
        }

        LxtLogInfo("[%d/%d] PID namespace sleeper %d", Level, getpid(), ChildPid);
    }

    //
    // Create 3 levels of nested PID namespaces and then signal the ready futex.
    //

    if (Level < 3)
    {
        LxtCheckErrno(ChildPid = fork());
        if (ChildPid == 0)
        {
            LxtLogInfo("[%d/%d] PID namespace trampoline", Level, getpid());
            LxtCheckErrno(unshare(CLONE_NEWPID));
            LxtCheckErrno(ChildPid = fork());
            if (ChildPid == 0)
            {
                _exit(NamespacePidTerminate(Level + 1, Ready));
            }

            _exit(0);
        }
    }
    else
    {
        LxtLogInfo("[%d/%d] Signaling ready futex...", Level, getpid());
        *Ready = 1;
        LxtCheckErrno(LxtFutex(Ready, FUTEX_WAKE, 1, NULL, NULL, 0));
    }

    //
    // Sleep.
    //

    for (;;)
    {
        sleep(-1);
    }

    Result = 0;

ErrorExit:
    return Result;
}

int NamespacePidTestTerminate()

{

    pid_t ChildPid;
    void* MapResult;
    int* Ready;
    int Result;
    struct timespec Time;
    int WaitStatus;

    //
    // Create the ready futex.
    //

    LxtCheckMapErrno(Ready = mmap(NULL, sizeof(int), PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_SHARED, -1, 0));
    *Ready = 0;

    //
    // Create the top-level PID namespace.
    //

    LxtCheckErrno(unshare(CLONE_NEWPID));
    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        _exit(NamespacePidTerminate(0, Ready));
    }

    //
    // Wait for the ready futex.
    //

    Time.tv_sec = 10;
    Time.tv_nsec = 0;
    LxtLogInfo("[%d] Waiting for all child threadgroups and namespaces to be created...", getpid());
    while (*Ready == 0)
    {
        Result = LxtFutex(Ready, FUTEX_WAIT, 0, &Time, NULL, 0);
        if ((Result == -1) && (errno != EAGAIN) && (errno != EINTR))
        {
            LxtCheckErrno(Result);
        }
    }

    //
    // Terminate the top-level PID namespace and wait on the leader.
    //

    LxtCheckErrnoZeroSuccess(kill(ChildPid, SIGKILL));
    LxtWaitPidPoll(ChildPid, SIGKILL);

    //
    // Sleep and make sure that there are no other waits pending.
    //

    sleep(1);
    LxtCheckErrnoFailure(waitpid(-1, &WaitStatus, WNOHANG), ECHILD);
    Result = 0;

ErrorExit:
    return Result;
}

int NamespacePidTestReboot()

{

    pid_t ChildPid;
    pid_t Pid;
    int* Ready;
    int Result;
    int WaitStatus;

    Pid = getpid();

    //
    // Create a child process of init that calls reboot.
    //
    // N.B. Init is terminated with SIGINT and the signal handler is not
    //      invoked.
    //

    LxtCheckErrno(unshare(CLONE_NEWPID));
    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        LxtCheckErrnoFailure(LxtReboot(LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2, LINUX_REBOOT_CMD_CAD_ON, NULL), EINVAL);
        LxtCheckErrnoFailure(LxtReboot(LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2, LINUX_REBOOT_CMD_CAD_OFF, NULL), EINVAL);
        LxtCheckResult(LxtSignalInitialize());
        LxtCheckResult(LxtSignalSetupHandler(SIGINT, SA_SIGINFO));
        LxtLogInfo("Forking...");
        LxtCheckErrno(ChildPid = fork());
        if (ChildPid == 0)
        {
            LxtCheckResult(LxtReboot(LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2, LINUX_REBOOT_CMD_POWER_OFF, NULL));
            _exit(0);
        }

        LxtLogInfo("Waiting...");
        LxtSignalWait();
        LxtCheckResult(LxtWaitPidPoll(ChildPid, 0));
        _exit(0);
    }

    //
    // Wait for the reboot signal.
    //

    LxtCheckResult(LxtWaitPidPoll(ChildPid, SIGINT));

    //
    // Sleep and make sure that there are no other waits pending.
    //

    sleep(1);
    LxtCheckErrnoFailure(waitpid(-1, &WaitStatus, WNOHANG), ECHILD);
    Result = 0;

ErrorExit:
    if (Pid != getpid())
    {
        _exit(Result);
    }

    return Result;
}

int NamespacePid(PLXT_ARGS Args)

{

    pid_t ChildPid;
    int Result;

    //
    // Check basic PID namespace behavior.
    //

    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        _exit(NamespacePidBasic(0));
    }

    LxtWaitPidPoll(ChildPid, 0);

    //
    // Check the pid namespace behavior for a pthread.
    //

    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        _exit(NamespacePidParentPThread());
    }

    LxtWaitPidPoll(ChildPid, 0);

    //
    // Check the pid namespace behavior for signals, termination and waits.
    //

    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        _exit(NamespacePidTestTerminate());
    }

    LxtWaitPidPoll(ChildPid, 0);

    //
    // Check the pid namespace behavior for reboot.
    //

    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        _exit(NamespacePidTestReboot());
    }

    LxtWaitPidPoll(ChildPid, 0);

    Result = 0;

ErrorExit:
    return Result;
}

int NamespaceNetworkGetNSID(int* NetworkNamespaceId)
{

    socklen_t AddressLength;
    struct rtattr* Attribute;
    struct sockaddr_nl BindAddress;
    struct nlmsgerr* Error;
    struct nlmsghdr* Header;
    int Index;
    int NetworkNamespaceFd;
    int ReceiveResult;
    int RemainingLength;
    struct
    {
        struct nlmsghdr nlh;
        struct rtgenmsg msg __attribute__((aligned(NLMSG_ALIGNTO)));
        struct
        {
            struct rtattr rta __attribute__((aligned(RTA_ALIGNTO)));
            int val __attribute__((aligned(RTA_ALIGNTO)));
        } data[5];
    } Request, Response;
    int Result;
    struct rtgenmsg* RtGenMsg;
    int Socket;

    //
    // Open network namespace file descriptor.
    //

    LxtCheckErrno(NetworkNamespaceFd = open("/proc/self/ns/net", O_RDONLY));

    //
    // Create and bind socket. Create a RTM_GETNSID request.
    //

    LxtCheckErrno(Socket = socket(AF_NETLINK, SOCK_RAW, 0));
    memset(&BindAddress, 0, sizeof(BindAddress));
    BindAddress.nl_family = AF_NETLINK;
    AddressLength = sizeof(BindAddress);
    LxtCheckErrno(bind(Socket, (struct sockaddr*)&BindAddress, AddressLength));

    memset(&Request, 0, sizeof(Request));
    Request.nlh.nlmsg_len = NLMSG_SPACE(sizeof(Request.msg)) + RTA_SPACE(sizeof(int));

    Request.nlh.nlmsg_type = RTM_GETNSID;
    Request.nlh.nlmsg_seq = 0x4567;
    Request.msg.rtgen_family = AF_UNSPEC;
    Request.nlh.nlmsg_flags = NLM_F_REQUEST;
    Request.data[0].rta.rta_len = RTA_LENGTH(sizeof(int));
    Request.data[0].rta.rta_type = NETNSA_FD;
    Request.data[0].val = NetworkNamespaceFd;
    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    memset(&Response, 0, sizeof(Response));
    LxtCheckErrno(ReceiveResult = recvfrom(Socket, &Response, sizeof(Response), 0, NULL, 0));

    LxtCheckTrue(NLMSG_OK(&Response.nlh, ReceiveResult));
    LxtCheckEqual(Response.nlh.nlmsg_type, RTM_NEWNSID, "%hd");
    LxtCheckTrue(Response.nlh.nlmsg_len >= NLMSG_LENGTH(sizeof(Response.msg)));

    Attribute = &Response.data[0].rta;
    RemainingLength = Response.nlh.nlmsg_len - NLMSG_LENGTH(sizeof(Response.msg));

    LxtCheckTrue(RTA_OK(Attribute, RemainingLength));
    *NetworkNamespaceId = *(int*)RTA_DATA(Attribute);
    Attribute = RTA_NEXT(Attribute, RemainingLength);
    LxtCheckTrue(RTA_OK(Attribute, RemainingLength) == FALSE);
    LxtCheckTrue(NLMSG_OK(NLMSG_NEXT(&Response.nlh, ReceiveResult), ReceiveResult) == FALSE);

    Result = 0;

ErrorExit:
    if (Socket > 0)
    {
        close(Socket);
    }

    if (NetworkNamespaceFd > 0)
    {
        close(NetworkNamespaceFd);
    }

    return Result;
}

int NamespaceNetwork(PLXT_ARGS Args)

{

    int NetworkNamespaceId;
    int OriginalNetworkNamespaceFd;
    int Result;
    DIR* SysClassNetDirectory;

    //
    // Open file descriptor of default network namespace.
    //

    LxtCheckErrno(OriginalNetworkNamespaceFd = open("/proc/self/ns/net", 0));

    //
    // Verify default namespace ID is set.
    //

    LxtCheckErrno(NamespaceNetworkGetNSID(&NetworkNamespaceId));
    LxtCheckEqual(NetworkNamespaceId, -1, "%d");

    //
    // Switch to a new network namespace.
    //

    LxtCheckErrno(unshare(CLONE_NEWNET));

    //
    // Verify default namespace ID is set.
    //

    LxtCheckErrno(NamespaceNetworkGetNSID(&NetworkNamespaceId));
    LxtCheckEqual(NetworkNamespaceId, -1, "%d");

    //
    // Switch back to original network namespace.
    //

    LxtCheckErrno(setns(OriginalNetworkNamespaceFd, CLONE_NEWNET));
    Result = 0;

ErrorExit:
    if (OriginalNetworkNamespaceFd >= 0)
    {
        LxtClose(OriginalNetworkNamespaceFd);
    }

    return Result;
}

int NamespaceNetworkProcfs(PLXT_ARGS Args)

{

    pid_t ChildPid;
    char FileName[50];
    struct ifreq InterfaceUpRequest;
    int Result;
    int Socket;

    //
    // Create a child process and switch it to a new network namespace.
    // From the parent, read the child's /proc/<pid>/net entries and verify
    // that those entries reflect the network state of the child.
    //

    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        LxtCheckErrnoZeroSuccess(unshare(CLONE_NEWNET));

        //
        // Bring the loopback up so that it shows up in procfs. (Needed for native
        // Ubuntu only - on WSL the loopback is automatically UP on namespace creation.)
        //

        LxtCheckErrno(Socket = socket(AF_UNIX, SOCK_DGRAM, 0));
        memset(&InterfaceUpRequest, 0, sizeof(InterfaceUpRequest));
        strncpy(InterfaceUpRequest.ifr_name, SOCKET_LOOPBACK_IF_NAME, sizeof(InterfaceUpRequest.ifr_name) - 1);

        usleep(1000 * 100);
        LxtCheckErrnoZeroSuccess(ioctl(Socket, SIOCGIFFLAGS, &InterfaceUpRequest));
        LxtCheckEqual(InterfaceUpRequest.ifr_flags & IFF_LOOPBACK, IFF_LOOPBACK, "%d");
        InterfaceUpRequest.ifr_flags |= IFF_UP;
        LxtCheckErrnoZeroSuccess(ioctl(Socket, SIOCSIFFLAGS, &InterfaceUpRequest));
        close(Socket);

        //
        // Keep the child alive so that the parent can examine its /proc/<pid>/net entries.
        //

        while (1)
            ;
        exit(0);
    }

    //
    // N.B. The sleep is because it can take some time for the lxcore cache to get
    //      the new network interface notification.
    //

    usleep(1000 * 200);

    //
    // Check the /proc/<pid>/net/dev file. This file should have 3 lines
    // (2 lines header and one line for lo).
    //

    sprintf(FileName, "/proc/%d/net/dev", ChildPid);
    LxtCheckResult(NamespaceNetworkProcfsCheckFile(FileName, 3));

    //
    // Check the /proc/<pid>/net/if_inet6 file. This file should have 1 line,
    // for lo.
    //

    sprintf(FileName, "/proc/%d/net/if_inet6", ChildPid);
    LxtCheckResult(NamespaceNetworkProcfsCheckFile(FileName, 1));

    //
    // Check the /proc/<pid>/net/route file. This file has one line on Ubuntu
    // (1 line header and no routing entries) and multiple lines on WSL. Therefore,
    // don't check the line count for this file.
    //

    sprintf(FileName, "/proc/%d/net/route", ChildPid);
    LxtCheckResult(NamespaceNetworkProcfsCheckFile(FileName, -1));

    //
    // All done, the child should die now.
    //

    kill(ChildPid, SIGKILL);
    LxtCheckResult(LxtWaitPidPoll(ChildPid, SIGKILL));
    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    return Result;
}

int NamespaceNetworkProcfsCheckFile(char* FileName, int ExpectedLineCount)

{

    char Buffer[200];
    FILE* File;
    int LineCount;
    int Result;

    //
    // Open the file up and check how many lines it has.
    //

    LineCount = 0;
    File = fopen(FileName, "r");
    LxtCheckTrue(File != NULL);
    while (fgets(Buffer, LXT_COUNT_OF(Buffer), File) != NULL)
    {
        LineCount += 1;

        //
        // The following strings should not be seen, since the only network
        // interface in the file should be lo.
        //

        LxtCheckTrue(strstr(Buffer, "eth") == NULL);
        LxtCheckTrue(strstr(Buffer, "wlan") == NULL);
        LxtCheckTrue(strstr(Buffer, "wifi") == NULL);
        LxtCheckTrue(strstr(Buffer, "und") == NULL);
    }

    if (ExpectedLineCount != -1)
    {
        LxtCheckEqual(LineCount, ExpectedLineCount, "%d");
    }

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    if (File != NULL)
    {
        fclose(File);
    }

    return Result;
}

typedef struct _LXT_NAMESPACE_IPC_DATA
{
    int Id;
    void* Address;
} LXT_NAMESPACE_IPC_DATA, *PLXT_NAMESPACE_IPC_DATA;

int NamespaceIpcChild(PLXT_NAMESPACE_IPC_DATA Data)

{

    int Fd;
    char Path[64];
    pid_t Ppid;
    int Result;
    struct shmid_ds Stat;

    Fd = -1;

    //
    // Check the ipc namespace behavior for before\after unshare.
    //

    LxtCheckErrno(LxtShmCtl(Data->Id, IPC_STAT, &Stat));
    LxtCheckErrno(unshare(CLONE_NEWIPC));
    LxtCheckErrnoFailure(LxtShmCtl(Data->Id, IPC_STAT, &Stat), EINVAL);

    //
    // shmdt should still succeed in the new namespace.
    //

    LxtCheckErrno(LxtShmDt(Data->Address));
    //
    // Check the ipc namespace behavior after switching back to the parent
    // ipc namespace.
    //

    Ppid = getppid();
    sprintf(Path, "/proc/%d/ns/ipc", Ppid);
    LxtCheckErrno(Fd = open(Path, O_RDONLY));
    LxtCheckErrno(setns(Fd, CLONE_NEWIPC));
    LxtCheckErrno(LxtShmCtl(Data->Id, IPC_STAT, &Stat));

    Result = 0;

ErrorExit:
    if (Fd != -1)
    {
        LxtClose(Fd);
    }

    return Result;
}

void NamespaceIpcFork(PLXT_NAMESPACE_IPC_DATA Data)

{

    int Result;

    Result = NamespaceIpcChild(Data);
    _exit(Result);
}

void* NamespaceIpcThread(void* Args)

{

    PLXT_NAMESPACE_IPC_DATA Data;
    int Result;

    Data = Args;
    Result = NamespaceIpcChild(Data);
    pthread_exit(&Result);
}

int NamespaceIpc(PLXT_ARGS Args)

{

    int ChildPid;
    LXT_NAMESPACE_IPC_DATA Data;
    void* MapResult;
    int Result;
    struct shmid_ds Stat;
    pthread_t ThreadId = {0};

    Data.Id = -1;
    Data.Address = NULL;

    //
    // Check the IPC behavior for fork()
    //

    LxtCheckErrno(Data.Id = LxtShmGet(IPC_PRIVATE, PAGE_SIZE, 0));
    LxtCheckMapErrno(Data.Address = LxtShmAt(Data.Id, NULL, 0));
    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        NamespaceIpcFork(&Data);
    }

    LxtWaitPidPoll(ChildPid, 0);
    LxtCheckErrno(LxtShmCtl(Data.Id, IPC_STAT, &Stat));

    //
    // Verify shmdt succeeds and create a new mapping.
    //

    LxtCheckErrno(LxtShmDt(Data.Address));
    LxtCheckMapErrno(Data.Address = LxtShmAt(Data.Id, NULL, 0));

    //
    // Check the IPC behavior for a pthread.
    //

    LxtCheckErrnoZeroSuccess(pthread_create(&ThreadId, NULL, NamespaceIpcThread, &Data));

    pthread_join(ThreadId, NULL);
    LxtCheckErrno(LxtShmCtl(Data.Id, IPC_STAT, &Stat));

    //
    // Verify shmdt fails (was unmapped by the pthread).
    //

    LxtCheckErrnoFailure(LxtShmDt(Data.Address), EINVAL);
    Data.Address = NULL;

    Result = 0;

ErrorExit:
    if (Data.Id != -1)
    {
        LxtShmCtl(Data.Id, IPC_RMID, NULL);
    }

    if (Data.Address != NULL)
    {
        LxtShmDt(Data.Address);
    }

    return Result;
}

int NamespaceCloneInvalidChild(unsigned long Flags, PLXT_PIPE Pipe)

/*++

Description:

    This routine is the child process for WaitPidVariationCloneParent.

Arguments:

    None..

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    pid_t ChildPid;
    pid_t GrandChildParent;
    pid_t ChildParent;
    int Result;
    int WaitPidStatus;

    Result = 0;

    //
    // Create a child process with the requested CLONE_PARENT and other flag.
    //
    // The new process should not be reported as a child.
    //

    ChildParent = getppid();
    LxtLogInfo("ChildParent %d", ChildParent);
    LxtCheckResult(ChildPid = LxtCloneSyscall(Flags | SIGCHLD, NULL, NULL, NULL, NULL));
    if (ChildPid == 0)
    {
        GrandChildParent = getppid();
        if ((Flags & CLONE_NEWPID) != 0)
        {
            LxtCheckEqual(0, GrandChildParent, "%d");
        }
        else
        {
            LxtCheckEqual(ChildParent, GrandChildParent, "%d");
        }

        LxtLogInfo("Grand child %d exiting", LxtGetTid());
    }
    else
    {
        LxtCheckErrnoFailure(waitpid(ChildPid, &WaitPidStatus, 0), ECHILD);
        LxtCheckResult(write(Pipe->Write, &ChildPid, sizeof(ChildPid)));
        LxtLogInfo("Child %d exiting", LxtGetTid());
    }

    Result = 0;

ErrorExit:
    _exit(Result);
}

int NamespaceCloneInvalid(PLXT_ARGS Args)

/*++
--*/

{

    int ChildPid;
    LXT_PIPE Pipe = {-1, -1};
    int PipeData;
    int Result;

    ChildPid = -1;
    LxtCheckResult(LxtCreatePipe(&Pipe));

    //
    // CLONE_NEWPID and CLONE_NEWUSER can't be specified with CLONE_THREAD.
    //

    LxtCheckErrnoFailure(ChildPid = LxtCloneSyscall(CLONE_NEWPID | CLONE_THREAD, NULL, NULL, NULL, NULL), EINVAL);
    LxtCheckErrnoFailure(ChildPid = LxtCloneSyscall(CLONE_NEWUSER | CLONE_THREAD, NULL, NULL, NULL, NULL), EINVAL);

    //
    // CLONE_NEWPID and CLONE_NEWUSER can be specified with CLONE_PARENT
    // (incorrect man pages).
    //

    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        NamespaceCloneInvalidChild(CLONE_NEWPID | CLONE_PARENT, &Pipe);
    }

    LxtCheckResult(read(Pipe.Read, &PipeData, sizeof(PipeData)));
    LxtCheckResult(LxtWaitPidPoll(PipeData, 0));
    LxtCheckResult(LxtWaitPidPoll(ChildPid, 0));

    //
    // CLONE_NEWIPC and CLONE_SYSVSEM are not allowed together.
    //

    LxtCheckErrnoFailure(ChildPid = LxtCloneSyscall(CLONE_NEWIPC | CLONE_SYSVSEM, NULL, NULL, NULL, NULL), EINVAL);

    //
    // TODO_LX: Enable the variation below when CLONE_NEWUSER is supported on
    //          WSL.
    //

    /*
        LxtCheckErrno(ChildPid = fork());
        if (ChildPid == 0) {
            NamespaceCloneInvalidChild(CLONE_NEWUSER | CLONE_PARENT, &Pipe);
        }

        LxtCheckResult(read(Pipe.Read, &PipeData, sizeof(PipeData)));
        LxtCheckResult(LxtWaitPidPoll(PipeData, 0));
        LxtCheckResult(LxtWaitPidPoll(ChildPid, 0));
    */

    Result = 0;

ErrorExit:
    if (ChildPid == 0)
    {
        _exit(Result);
    }

    LxtClosePipe(&Pipe);
    return Result;
}
