/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    get_set_id.c

Abstract:

    This file is a test for the get*id and set*id system calls.

--*/

#include "lxtcommon.h"
#include "unittests.h"
#include <stdio.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <sys/un.h>
#include <limits.h>

#define LXT_NAME "get_set_id"

#if !defined(__amd64__) && !defined(__aarch64__)

#define __NR_getuid16 24
#define __NR_geteuid16 49
#define __NR_getgid16 47
#define __NR_getegid16 50
#define __NR_getresuid16 165
#define __NR_getresgid16 171

int GetSetId16Bit(PLXT_ARGS Args);

#endif

typedef unsigned short USHORT;

//
// uid16_t and gid16_t are unsigned 16-bit integers.
//

typedef USHORT uid16_t;

#define MAX_UID16_T USHRT_MAX

typedef USHORT gid16_t;

#define MAX_GID16_T USHRT_MAX

int GetSetId0(PLXT_ARGS Args);

int GetSetId1(PLXT_ARGS Args);

int GetSetIdParseArgs(int Argc, char* Argv[], LXT_ARGS* Args);

int GetSetPgid(PLXT_ARGS Args);

int GetSetPgidChildProcess(void);

int GetSetPgidChildProcess2(pid_t GroupId);

int GetSetPgidExecve(PLXT_ARGS Args);

int GetSetPgidExecveChild(void);

int GetSetSid(PLXT_ARGS Args);

//
// Global constants
//

static const LXT_VARIATION g_LxtVariations[] = {
    {"GetSetId Basic", GetSetId0},
    {"GetResuid-GetResgid Basic", GetSetId1},

#if !defined(__amd64__) && !defined(__aarch64__)
    {"GetSetId 16-bit versions", GetSetId16Bit},
#endif
    {"GetSetPgid Basic", GetSetPgid},
    {"GetSetPgid with execve", GetSetPgidExecve},
    {"GetSetSid Basic", GetSetSid}};

static const char* g_SocketPath = "/data/test/lxt_get_set_id_sock";

int GetSetIdTestEntry(int Argc, char* Argv[])

/*++

Routine Description:

    This routine main entry point for the test for get*id,set*id system call.

Arguments:

    Argc - Supplies the number of command line arguments.

    Argv - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    LXT_ARGS Args;
    int Result;

    LxtCheckResult(GetSetIdParseArgs(Argc, Argv, &Args));
ErrorExit:
    LxtUninitialize();
    return !LXT_SUCCESS(Result);
}

int GetSetId0(PLXT_ARGS Args)

/*++

Routine Description:

    This routine validates the various get*id and set*id system
    calls.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    gid_t Egid;
    gid_t EgidFromGetResgid;
    uid_t Euid;
    uid_t EuidFromGetResuid;
    gid_t Gid;
    gid_t GidFromGetResgid;
    pid_t Pgid;
    pid_t Pgid0;
    pid_t Pid;
    pid_t Ppid;
    int Result;
    uid_t SuidFromGetResuid;
    gid_t SgidFromGetResgid;
    pid_t Tid;
    uid_t Uid;
    uid_t UidFromGetResuid;

    Pid = getpid();
    Ppid = getppid();
    Gid = getgid();
    Egid = getegid();
    Uid = getuid();
    Euid = geteuid();
    Tid = gettid();
    LxtCheckResult((Pgid0 = getpgid(0)));
    LxtCheckResult((Pgid = getpgid(Pid)));
    LxtCheckErrnoFailure(getpgid(-1), ESRCH);
    LxtCheckResult(getresuid(&UidFromGetResuid, &EuidFromGetResuid, &SuidFromGetResuid));

    LxtCheckResult(getresgid(&GidFromGetResgid, &EgidFromGetResgid, &SgidFromGetResgid));

    LxtLogInfo(
        "ID Details. Pid:%d, Ppid:%d, Gid:%u, Egid:%u, Uid:%u, "
        "Euid:%u, Tid:%u, Pgid:%d, Uid From GetResuid:%u, "
        "Euid From GetResuid:%u, Suid From GetResuid:%u, Gid From "
        "GetResgid:%u, Egid From GetResgid:%u, Sgid From GetResgid:%u",
        Pid,
        Ppid,
        Gid,
        Egid,
        Uid,
        Euid,
        Tid,
        Pgid,
        UidFromGetResuid,
        EuidFromGetResuid,
        SuidFromGetResuid,
        GidFromGetResgid,
        EgidFromGetResgid,
        SgidFromGetResgid);

    //
    // getpgid(Pid) == getpgid(0)
    //

    if (Pgid != Pgid0)
    {
        LxtLogError(
            "getpgid(<self>) == getpgid(0). getpgid(<self>):%d, "
            "getpgid(0): %d",
            Pgid,
            Pgid0);
        Result = LXT_RESULT_FAILURE;
        goto ErrorExit;
    }

    //
    // For a single threaded process, Thread ID == Process ID
    //

    if (Pid != Tid)
    {
        LxtLogError(
            "For a single threaded process, Process ID == Thread ID.  "
            "Process ID:%u, Thread ID:%u",
            Pid,
            Tid);
        Result = LXT_RESULT_FAILURE;
        goto ErrorExit;
    }

    //
    // ID's from get*id and getresuid/getresgid should match
    //

    if (Uid != UidFromGetResuid)
    {
        LxtLogError(
            "UID from getuid and getresuid do not match.  "
            "uid from getuid:%u, uid from getresuid:%u",
            Uid,
            UidFromGetResuid);
        Result = LXT_RESULT_FAILURE;
        goto ErrorExit;
    }

    if (Euid != EuidFromGetResuid)
    {
        LxtLogError(
            "EUID from geteuid and getresuid do not match.  "
            "euid from getuid:%u, euid from getresuid:%u",
            Euid,
            EuidFromGetResuid);
        Result = LXT_RESULT_FAILURE;
        goto ErrorExit;
    }

    if (Gid != GidFromGetResgid)
    {
        LxtLogError(
            "GID from getgid and getresgid do not match.  "
            "gid from getgid:%u, gid from getresgid:%u",
            Gid,
            GidFromGetResgid);
        Result = LXT_RESULT_FAILURE;
        goto ErrorExit;
    }

    if ((Egid != EgidFromGetResgid))
    {
        LxtLogError(
            "EGID from getegid and getresgid do not match.  "
            "egid from getegid:%u, egid from getresgid:%u",
            Egid,
            EgidFromGetResgid);
        Result = LXT_RESULT_FAILURE;
        goto ErrorExit;
    }

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    return Result;
}

int GetSetId1(PLXT_ARGS Args)

/*++

Routine Description:

    This routine validates the GetResuid/GetResgid system calls.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    gid_t EgidFromGetResgid;
    uid_t EuidFromGetResuid;
    gid_t GidFromGetResgid;
    int Result;
    uid_t SuidFromGetResuid;
    gid_t SgidFromGetResgid;
    uid_t UidFromGetResuid;

    LxtCheckErrnoFailure(getresuid(NULL, NULL, NULL), EFAULT);

    LxtCheckErrnoFailure(getresuid(&UidFromGetResuid, NULL, NULL), EFAULT);

    LxtCheckErrnoFailure(getresuid(NULL, &EuidFromGetResuid, &SuidFromGetResuid), EFAULT);

    LxtCheckResult(getresuid(&UidFromGetResuid, &EuidFromGetResuid, &SuidFromGetResuid));
    LxtCheckErrnoFailure(getresgid(NULL, NULL, NULL), EFAULT);

    LxtCheckErrnoFailure(getresgid(&GidFromGetResgid, NULL, NULL), EFAULT);

    LxtCheckErrnoFailure(getresgid(NULL, &EgidFromGetResgid, &SgidFromGetResgid), EFAULT);

    LxtCheckResult(getresgid(&GidFromGetResgid, &EgidFromGetResgid, &SgidFromGetResgid));

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    return Result;
}

#if !defined(__amd64__) && !defined(__aarch64__)

int GetSetId16Bit(PLXT_ARGS Args)

/*++

Routine Description:

    This routine validates the various 16-bit versions of the
    get*id and set*id system calls.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    gid_t Egid;
    gid16_t Egid16;
    gid16_t EgidFromGetResgid16;
    uid_t Euid;
    uid16_t Euid16;
    uid16_t EuidFromGetResuid16;
    gid_t Gid;
    gid16_t Gid16;
    gid16_t GidFromGetResgid16;
    int Result;
    uid16_t SuidFromGetResuid16;
    gid16_t SgidFromGetResgid16;
    uid_t Uid;
    uid16_t Uid16;
    uid16_t UidFromGetResuid16;

    Gid = getgid();
    Gid16 = 0;
    Egid = getegid();
    Uid = getuid();
    Uid16 = 0;
    Euid = geteuid();

    LxtLogInfo("UID and GID Details. Gid:%d, Egid:%d, Uid:%u, Euid:%u", Gid, Egid, Uid, Euid);

    //
    // Before calling the 16-bit versions, make sure the IDs are
    // within the range.
    //

    if (Gid <= MAX_GID16_T)
    {
        Gid16 = syscall(__NR_getgid16);
        if (Gid != Gid16)
        {
            LxtLogError(
                "GID from getgid and getgid16 do not match.  "
                "gid from getgid:%d, gid from getgid16:%d",
                Gid,
                Gid16);
            Result = LXT_RESULT_FAILURE;
            goto ErrorExit;
        }
    }

    if (Egid <= MAX_GID16_T)
    {
        Egid16 = syscall(__NR_getegid16);
        if (Egid != Egid16)
        {
            LxtLogError(
                "EGID from getegid and getegid16 do not match.  "
                "egid from getegid:%d, egid from getegid16:%d",
                Egid,
                Egid16);
            Result = LXT_RESULT_FAILURE;
            goto ErrorExit;
        }
    }

    if ((Gid <= MAX_GID16_T) && (Egid <= MAX_GID16_T))
    {
        LxtCheckResult(syscall(__NR_getresgid16, &GidFromGetResgid16, &EgidFromGetResgid16, &SgidFromGetResgid16));

        LxtLogInfo("SGID16:%d", SgidFromGetResgid16);

        if (Gid16 != GidFromGetResgid16)
        {
            LxtLogError(
                "GID from getgid16 and getresgid16 do not match.  "
                "gid from getgid16:%d, gid from getresgid16:%d",
                Gid16,
                GidFromGetResgid16);
            Result = LXT_RESULT_FAILURE;
            goto ErrorExit;
        }

        if ((Egid16 != EgidFromGetResgid16))
        {
            LxtLogError(
                "EGID from getegid16 and getresgid16 do not match.  "
                "egid from getegid16:%d, egid from getresgid16:%d",
                Egid16,
                EgidFromGetResgid16);
            Result = LXT_RESULT_FAILURE;
            goto ErrorExit;
        }

        LxtCheckErrnoFailure(syscall(__NR_getresgid16, NULL, NULL, NULL), EFAULT);

        LxtCheckErrnoFailure(syscall(__NR_getresgid16, &GidFromGetResgid16, NULL, NULL), EFAULT);

        LxtCheckErrnoFailure(syscall(__NR_getresgid16, NULL, &EgidFromGetResgid16, &SgidFromGetResgid16), EFAULT);
    }

    if (Uid <= MAX_UID16_T)
    {
        Uid16 = syscall(__NR_getuid16);
        if (Uid != Uid16)
        {
            LxtLogError(
                "UID from getuid and getuid16 do not match.  "
                "uid from getuid:%d, uid from getuid16:%d",
                Uid,
                Uid16);
            Result = LXT_RESULT_FAILURE;
            goto ErrorExit;
        }
    }

    if (Euid <= MAX_UID16_T)
    {
        Euid16 = syscall(__NR_geteuid16);
        if (Euid != Euid16)
        {
            LxtLogError(
                "EUID from geteuid and geteuid16 do not match.  "
                "egid from geteuid:%d, egid from geteuid16:%d",
                Euid,
                Euid16);
            Result = LXT_RESULT_FAILURE;
            goto ErrorExit;
        }
    }

    if ((Uid <= MAX_UID16_T) && (Euid <= MAX_UID16_T))
    {
        LxtCheckResult(syscall(__NR_getresuid16, &UidFromGetResuid16, &EuidFromGetResuid16, &SuidFromGetResuid16));

        LxtLogInfo("SUID16:%d", SuidFromGetResuid16);

        if (Uid16 != UidFromGetResuid16)
        {
            LxtLogError(
                "UID from getuid16 and getresuid16 do not match.  "
                "uid from getuid16:%d, uid from getresuid16:%d",
                Uid16,
                UidFromGetResuid16);
            Result = LXT_RESULT_FAILURE;
            goto ErrorExit;
        }

        if (Euid16 != EuidFromGetResuid16)
        {
            LxtLogError(
                "EUID from geteuid16 and getresuid16 do not match.  "
                "euid from getuid16:%d, euid from getresuid16:%d",
                Euid16,
                EuidFromGetResuid16);
            Result = LXT_RESULT_FAILURE;
            goto ErrorExit;
        }

        LxtCheckErrnoFailure(syscall(__NR_getresuid16, NULL, NULL, NULL), EFAULT);

        LxtCheckErrnoFailure(syscall(__NR_getresuid16, &UidFromGetResuid16, NULL, NULL), EFAULT);

        LxtCheckErrnoFailure(syscall(__NR_getresuid16, NULL, &EuidFromGetResuid16, &SuidFromGetResuid16), EFAULT);
    }

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    return Result;
}

#endif

int GetSetIdParseArgs(int Argc, char* Argv[], LXT_ARGS* Args)

/*++

Routine Description:

    This routine parses command line arguments for the get_set_id tests.

Arguments:

    Argc - Supplies the number of arguments.

    Argv - Supplies an array of arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int ArgvIndex;
    int Result;
    int ValidArguments;

    Result = LXT_RESULT_FAILURE;
    ValidArguments = 0;

    if (Argc < 1)
    {
        goto ErrorExit;
    }

    for (ArgvIndex = 1; ArgvIndex < Argc; ++ArgvIndex)
    {
        if (Argv[ArgvIndex][0] != '-')
        {
            printf("Unexpected character %s", Argv[ArgvIndex]);
            goto ErrorExit;
        }

        switch (Argv[ArgvIndex][1])
        {
        case 'c':

            //
            // Run the setpgid execve test child
            //

            ValidArguments = 1;
            Result = GetSetPgidExecveChild();
            goto ErrorExit;

        case 'v':

            //
            // This was already taken care of by LxtInitialize.
            //

            ++ArgvIndex;

            break;

        default:
            goto ErrorExit;
        }
    }

    //
    // If -c was not specified, just run the tests
    //

    ValidArguments = 1;
    LxtCheckResult(LxtInitialize(Argc, Argv, Args, LXT_NAME));
    LxtCheckResult(LxtRunVariations(Args, g_LxtVariations, LXT_COUNT_OF(g_LxtVariations)));

ErrorExit:
    if (ValidArguments == 0)
    {
        printf("\nuse: get_set_id <One of the below arguments>");
        printf("\t-c : Run getsetpgid execve test child (don't use directly)");
    }

    return Result;
}

int GetSetPgid(PLXT_ARGS Args)

/*++

Routine Description:

    This routine validates the getpgid and setpgid system calls.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    const int Processes = 2;

    pid_t Child;
    pid_t GroupId;
    pid_t NewGroup;
    int Process;
    pid_t ProcessIds[2];
    int Result;
    int Status;

    //
    // Check that a child process initially inherits our process group ID.
    //

    LxtCheckErrno(Child = fork());
    if (Child == 0)
    {
        _exit(GetSetPgidChildProcess());
    }

    LxtCheckResult(LxtWaitPidPoll(Child, 0));

    //
    // Create two processes; the first will be the group leader and the second
    // will use the group id of the first. The method used here reflects how
    // job control shells create groups for pipelines.
    //

    GroupId = 0;
    for (Process = 0; Process < Processes; Process += 1)
    {
        LxtCheckErrno(Child = fork());
        ProcessIds[Process] = Child;
        if (Child == 0)
        {
            _exit(GetSetPgidChildProcess2(GroupId));
        }

        if (GroupId == 0)
        {
            GroupId = Child;
        }

        LxtCheckErrnoZeroSuccess(setpgid(Child, GroupId));
        LxtCheckErrno(NewGroup = getpgid(Child));
        if (NewGroup != GroupId)
        {
            LxtLogError(
                "getpgid() return value does not match the "
                "value set by setpgid. Expected: %i; actual: %i",
                GroupId,
                NewGroup);

            Result = LXT_RESULT_FAILURE;
            goto ErrorExit;
        }
    }

    for (Process = 0; Process < Processes; Process += 1)
    {
        LxtCheckResult(LxtWaitPidPoll(ProcessIds[Process], 0));
    }

    //
    // After the child processes were waited on, the process group is no longer
    // valid, nor are the children pids. There is currently a race condition
    // that causes waitpid to return before the process group is cleaned up
    // so sleep before trying this.
    //

    sleep(1);
    LxtCheckErrnoFailure(setpgid(0, GroupId), EPERM);
    LxtCheckErrnoFailure(setpgid(ProcessIds[0], 0), ESRCH);
    LxtCheckErrnoFailure(getpgid(ProcessIds[0]), ESRCH);

    //
    // Getpgid for non-child process should succeed, setpgid should fail
    //

    LxtCheckErrno(getpgid(getppid()));
    LxtCheckErrnoFailure(setpgid(getppid(), 0), ESRCH);

    //
    // Cannot change process group of session leader.
    //

    LxtCheckResult(LxtSignalBlock(SIGUSR1));
    LxtCheckErrno(Child = fork());
    if (Child == 0)
    {
        LxtCheckErrno(setsid());
        LxtCheckErrnoFailure(setpgid(0, getppid()), EPERM);
        LxtCheckErrnoFailure(setpgid(0, 0), EPERM);
        LxtCheckErrnoZeroSuccess(kill(getppid(), SIGUSR1));
        LxtCheckResult(LxtSignalWaitBlocked(SIGUSR1, getppid(), 2));
        _exit(LXT_RESULT_SUCCESS);
    }

    LxtCheckResult(LxtSignalWaitBlocked(SIGUSR1, Child, 2));
    LxtCheckErrnoFailure(setpgid(Child, 0), EPERM);
    LxtCheckErrnoFailure(setpgid(Child, getpgid(0)), EPERM);

    //
    // Cannot change to process group which is in a different session.
    //

    LxtCheckErrnoFailure(setpgid(0, Child), EPERM);

    //
    // Tell the child to exit.
    //

    LxtCheckErrnoZeroSuccess(kill(Child, SIGUSR1));
    LxtCheckResult(LxtWaitPidPoll(Child, 0));

    //
    // Bogus pid and pgid values. The fact that setpgid returns different
    // errors for a negative pid if pgid==0 is consistent with Linux.
    //

    LxtCheckErrnoFailure(getpgid(-1), ESRCH);
    LxtCheckErrnoFailure(setpgid(-1, 1), ESRCH);
    LxtCheckErrnoFailure(setpgid(-1, 0), EINVAL);
    LxtCheckErrnoFailure(setpgid(0, -1), EINVAL);

ErrorExit:
    return Result;
}

int GetSetPgidChildProcess(void)

/*++

Routine Description:

    This routine runs the child process for GetSetPgid that checks its pgid.

Arguments:

    None.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    pid_t Parent;
    pid_t ParentGroupId;
    pid_t GroupId;
    int Result;

    Parent = getppid();
    LxtCheckErrno(ParentGroupId = getpgid(Parent));
    LxtCheckErrno(GroupId = getpgid(0));
    LxtLogInfo("Process %i pgid: %i, parent %i pgid: %i", getpid(), GroupId, Parent, ParentGroupId);

    Result = LXT_RESULT_FAILURE;
    if (GroupId == 0)
    {
        LxtLogError("Group ID should never be zero.");
        goto ErrorExit;
    }

    if (GroupId != ParentGroupId)
    {
        LxtLogError("Pgid %i did not match parent pgid %i", GroupId, ParentGroupId);

        goto ErrorExit;
    }

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    return Result;
}

int GetSetPgidChildProcess2(pid_t GroupId)

/*++

Routine Description:

    This routine runs the child processes for GetSetPgid that set their pgid.

Arguments:

    GroupId - The process group ID for this child process (0 if this is the
        new leader)

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int NewGroup;
    int Pid;
    int Result;
    LxtCheckErrnoZeroSuccess(setpgid(0, GroupId));

    //
    // If we were passed 0, the expected result should match the process ID.
    //

    Pid = getpid();
    if (GroupId == 0)
    {
        GroupId = Pid;
    }

    LxtCheckErrno(NewGroup = getpgid(0));
    if (NewGroup != GroupId)
    {
        LxtLogError(
            "getpgid(0) return value does not match the value set by "
            "setpgid. Expected: %i; actual: %i",
            GroupId,
            NewGroup);

        Result = LXT_RESULT_FAILURE;
        goto ErrorExit;
    }

    LxtLogInfo("Process %i pgid: %i", Pid, NewGroup);

    LxtCheckErrno(NewGroup = getpgid(Pid));
    if (NewGroup != GroupId)
    {
        LxtLogError(
            "getpgid(getpid()) return value does not match the value "
            "set by setpgid. Expected: %i; actual: %i",
            GroupId,
            NewGroup);

        Result = LXT_RESULT_FAILURE;
        goto ErrorExit;
    }

    //
    // The sleep is to keep the group alive until the second process
    // runs. Remove it once zombie processes are implemented.
    //

    sleep(1);

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    return Result;
}

int GetSetPgidExecve(PLXT_ARGS Args)

/*++

Routine Description:

    This routine checks whether setpgid returns the correct failure if it is
    called on a process that has already called execve.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    struct sockaddr_un Address;
    int ClientSocket;
    char* Argv[4];
    char* Envp[1];
    int Result;
    int ServerSocket;
    int Status;
    int Child;

    ClientSocket = -1;
    Child = -1;

    //
    // To make sure we call setpgid after the child process runs execve and
    // before it exits, we use a unix socket to let the child signal us when
    // it's running, and then we signal the child to exit after the test.
    //

    LxtCheckErrno(ServerSocket = socket(AF_UNIX, SOCK_STREAM, 0));

    //
    // Remove the path name in case it exists from a failed prior test run;
    // ignore errors.
    //

    unlink(g_SocketPath);

    //
    // Bind to the address and start listening
    //

    memset(&Address, 0, sizeof(Address));
    Address.sun_family = AF_UNIX;
    strncpy(Address.sun_path, g_SocketPath, sizeof(Address.sun_path) - 1);
    Address.sun_path[sizeof(Address.sun_path) - 1] = 0;
    LxtCheckErrnoZeroSuccess(bind(ServerSocket, (struct sockaddr*)&Address, sizeof(Address)));
    LxtCheckErrnoZeroSuccess(listen(ServerSocket, 1));

    //
    // Start the child process.
    //

    LxtCheckErrno(Child = fork());
    if (Child == 0)
    {
        Argv[0] = WSL_UNIT_TEST_BINARY; // adhere to new single test binary design
        Argv[1] = Args->Argv[0];
        Argv[2] = "-c";
        Argv[3] = Envp[0] = NULL;
        execve(Argv[0], Argv, Envp);
        LxtLogError("Execve failed, errno: %d (%s)", errno, strerror(errno));
        _exit(LXT_RESULT_FAILURE);
    }

    //
    // Wait for the client to tell us it's running; this guarantees we
    // call setpgid after the execve call.
    //

    LxtCheckErrno(ClientSocket = accept(ServerSocket, NULL, NULL));
    LxtCheckResult(LxtReceiveMessage(ClientSocket, "execve"));

    //
    // The child is now running inside the binary loaded by execve, so we
    // can try to call setpgid, which should fail with EACCES.
    //

    LxtCheckErrnoFailure(setpgid(Child, 0), EACCES);

    //
    // Tell the client it can exit.
    //

    LxtCheckResult(LxtSendMessage(ClientSocket, "exit")) LxtCheckResult(LxtWaitPidPoll(Child, 0));

ErrorExit:
    if (ClientSocket >= 0)
    {
        close(ClientSocket);
    }

    if (ServerSocket >= 0)
    {
        close(ServerSocket);
    }

    unlink(g_SocketPath);

    return Result;
}

int GetSetPgidExecveChild(void)

/*++

Routine Description:

    This routine runs the child process for GetSetPgidExecve.

Arguments:

    None.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    struct sockaddr_un Address;
    int ClientSocket;
    int Result;

    LxtLogInfo("Child executable running, pid = %i", getpid());
    LxtCheckErrno(ClientSocket = socket(AF_UNIX, SOCK_STREAM, 0));

    //
    // Connect to the parent process via AF_UNIX socket.
    //

    memset(&Address, 0, sizeof(Address));
    Address.sun_family = AF_UNIX;
    strncpy(Address.sun_path, g_SocketPath, sizeof(Address.sun_path) - 1);
    Address.sun_path[sizeof(Address.sun_path) - 1] = 0;
    LxtCheckErrnoZeroSuccess(connect(ClientSocket, (struct sockaddr*)&Address, sizeof(Address)));

    //
    // Tell the parent the process is running inside the execve'd binary.
    //

    LxtCheckResult(LxtSendMessage(ClientSocket, "execve"));

    //
    // Wait for the parent to tell this process it can exit so it has the
    // chance to call setpgid.
    //

    LxtCheckResult(LxtReceiveMessage(ClientSocket, "exit"));

    //
    // This process should be able to change its own process group.
    //

    LxtCheckErrnoZeroSuccess(setpgid(0, 0));
    LxtLogInfo("Child executable finished");
    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    if (ClientSocket >= 0)
    {
        close(ClientSocket);
    }

    return Result;
}

int GetSetSid(PLXT_ARGS Args)

/*++

Routine Description:

    This routine tests the getsid() and setsid() system calls..

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    pid_t ChildPid;
    pid_t ParentSid;
    int Result;
    pid_t Sid;

    LxtCheckResult(LxtSignalBlock(SIGUSR1));
    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {

        //
        // Initial values.
        //

        LxtCheckErrno(ParentSid = getsid(getppid()));
        LxtCheckNotEqual(ParentSid, 0, "%d") LxtCheckErrno(Sid = getsid(0));
        LxtCheckEqual(ParentSid, Sid, "%d");
        LxtCheckErrno(Sid = getsid(getpid()));
        LxtCheckEqual(ParentSid, Sid, "%d");

        //
        // Create a new session.
        //

        LxtCheckErrno(Sid = setsid());
        LxtCheckNotEqual(ParentSid, Sid, "%d");
        LxtCheckEqual(Sid, getpid(), "%d");
        LxtCheckEqual(Sid, getpgid(0), "%d");

        //
        // Tell the parent that a new session was created.
        //

        LxtCheckErrnoZeroSuccess(kill(getppid(), SIGUSR1));

        //
        // Wait for the signal to exit.
        //

        LxtCheckResult(LxtSignalWaitBlocked(SIGUSR1, getppid(), 2));
        _exit(LXT_RESULT_SUCCESS);
    }

    //
    // Wait until the child has created the session.
    //

    LxtCheckResult(LxtSignalWaitBlocked(SIGUSR1, ChildPid, 2));
    LxtCheckErrno(Sid = getsid(ChildPid));
    LxtCheckErrno(ParentSid = getsid(0));
    LxtCheckNotEqual(ParentSid, Sid, "%d");
    LxtCheckEqual(Sid, ChildPid, "%d");
    LxtCheckEqual(Sid, getpgid(ChildPid), "%d");

    //
    // Tell the child to exit.
    //

    LxtCheckErrnoZeroSuccess(kill(ChildPid, SIGUSR1));
    LxtWaitPidPoll(ChildPid, 0);

    //
    // If the process is a process group leader, it can't create a new
    // session. The test is already the process group leader if it is launched
    // from the shell but not from all test environments.
    //

    if (getpid() != getpgid(0))
    {
        LxtCheckResult(setpgid(getpid(), 0));
    }

    LxtCheckErrnoFailure(setsid(), EPERM);

    //
    // Getsid with invalid arguments.
    //

    LxtCheckErrnoFailure(getsid(-1), ESRCH);

ErrorExit:
    return Result;
}
