/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    execve.c

Abstract:

    This file contains the execve tests.

--*/

#include "lxtcommon.h"
#include "unittests.h"
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <linux/binfmts.h>
#include "lxtutil.h"

#define LXT_NAME "Execve"

#define LXT_EXECV_TEST_DIRECTORY "/data/test/execvDir"

int ExecveExecValidate(char* Path);

int ExecveValidate(int ExpectedPid);

int ExecveVariationArguments(PLXT_ARGS Args);

int ExecveVariationSingle(PLXT_ARGS Args);

int ExecveVariationMultipleWithThreads(PLXT_ARGS Args);

int ExecveWaitForChild(void);

void* ExecveWorkerThread(void* Arg);

void* ExecveWorkerThread2(void* Arg);

//
// Global constants
//

static const LXT_VARIATION g_LxtVariations[] = {
    {"Execve - Single", ExecveVariationSingle},
    {"Execve - Multiple with threads", ExecveVariationMultipleWithThreads},
    {"Execve - Arguments", ExecveVariationArguments}};

int ExecveTestEntry(int Argc, char* Argv[], char** Envp)

/*++
--*/

{

    LXT_ARGS Args;
    int Result;

    //
    // For a child invocation, validate that the PID/TID is as expected.
    //

    if ((Argc == 3) && (strcmp(Argv[1], "-c") == 0))
    {

        int Pid;

        Pid = atoi(Argv[2]);
        LxtCheckResult(ExecveValidate(Pid));
        goto ErrorExit;
    }

    //
    // For environment variable child validation, make sure a non-NULL pointer
    // is passed but there are no arguments.
    //

    if ((Argc == 2) && (strcmp(Argv[1], "-e") == 0))
    {
        LxtCheckTrue(Envp != NULL);

        char** Env;
        int Count = 0;
        for (Env = Envp; *Env != NULL; Env += 1)
        {
            Count += 1;
        }

        LxtCheckTrue(Count == 0);
        goto ErrorExit;
    }

    //
    // Run the master test.
    //

    LxtCheckResult(LxtInitialize(Argc, Argv, &Args, LXT_NAME));
    LxtCheckResult(LxtRunVariations(&Args, g_LxtVariations, LXT_COUNT_OF(g_LxtVariations)));

ErrorExit:
    LxtUninitialize();
    return !LXT_SUCCESS(Result);
}

int ExecveExecValidate(char* Path)

/*++
--*/

{

    char* ExecArgs[5];
    char ExpectedPid[16];
    int Pid;
    int Result;

    Pid = getpid();
    LxtLogInfo("Execve'ing validation for PID %d", Pid);

    sprintf(ExpectedPid, "%d", Pid);
    ExecArgs[0] = Path;
    ExecArgs[1] = "execve";
    ExecArgs[2] = "-c";
    ExecArgs[3] = ExpectedPid;
    ExecArgs[4] = NULL;
    execv(ExecArgs[0], ExecArgs);
    LxtCheckTrue(FALSE);
    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    return Result;
}

int ExecveValidate(int ExpectedPid)

/*++
--*/

{

    int Pid;
    int Result;
    int ThreadCount;
    int Tid;

    //
    // Check that the PID/TID matches the expected value.
    //

    Pid = getpid();
    Tid = gettid();
    LxtCheckTrue(Pid == ExpectedPid);
    LxtCheckTrue(Tid == ExpectedPid);
    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    return Result;
}

#define COMMAND_LINE_SIZE (((size_t)MAX_ARG_STRINGS) + 1)
#define LARGE_STRING_SIZE (((size_t)MAX_ARG_STRLEN) + 1)
#define MAX_STACK_SIZE ((size_t)(2 * 1024 * 1024))

int ExecveVariationArguments(PLXT_ARGS Args)

/*++
--*/

{

    int ChildPid;
    char* ExecArgs[4];
    char** ExecArgsLong;
    size_t Index;
    char* LongString;
    int Result;
    int Status;
    size_t TotalSize;

    ExecArgsLong = NULL;
    LongString = NULL;

    //
    // Test a null environment block.
    //

    LxtCheckResult(ChildPid = fork());
    if (ChildPid == 0)
    {
        ExecArgs[0] = "/bin/true";
        ExecArgs[1] = NULL;
        LxtCheckErrno(LxtExecve(ExecArgs[0], ExecArgs, NULL));

        //
        // The parent waits for the child to exit successfully.
        //
    }
    else
    {
        LxtCheckResult(ExecveWaitForChild());
    }

    //
    // Test exec args with spaces and path.
    //

    LxtCheckResult(ChildPid = fork());
    if (ChildPid == 0)
    {
        ExecArgs[0] = "/bin/true with a space";
        ExecArgs[1] = NULL;
        LxtCheckErrno(LxtExecve("/bin/true", ExecArgs, NULL));

        //
        // The parent waits for the child to exit successfully.
        //
    }
    else
    {
        LxtCheckResult(ExecveWaitForChild());
    }

    //
    // Validate that a null environment block results in zero entries for the
    // environment argument to the main function.
    //

    LxtCheckResult(ChildPid = fork());
    if (ChildPid == 0)
    {
        ExecArgs[0] = WSL_UNIT_TEST_BINARY;
        ExecArgs[1] = "execve";
        ExecArgs[2] = "-e";
        ExecArgs[3] = NULL;
        LxtCheckErrno(LxtExecve(ExecArgs[0], ExecArgs, NULL));

        //
        // The parent waits for the child to exit successfully.
        //
    }
    else
    {
        LxtCheckResult(ExecveWaitForChild());
    }

    //
    // Allocate a very long string of 'a' and null terminate to
    // validate command line parsing limits.
    //

    LongString = malloc(LARGE_STRING_SIZE);
    if (LongString == NULL)
    {
        LxtLogError("malloc");
        goto ErrorExit;
    }

    memset(LongString, 'a', LARGE_STRING_SIZE);
    LongString[LARGE_STRING_SIZE - 1] = '\0';

    //
    // Create a child and verify that exec fails with too long of a string
    // in the command line or environment variable array.
    //

    LxtCheckResult(ChildPid = fork());
    if (ChildPid == 0)
    {
        ExecArgs[0] = "/bin/false";
        ExecArgs[1] = LongString;
        ExecArgs[2] = NULL;
        LxtCheckErrnoFailure(LxtExecve(ExecArgs[0], ExecArgs, NULL), E2BIG);
        LxtCheckErrnoFailure(LxtExecve(ExecArgs[0], NULL, &ExecArgs[1]), E2BIG);

        //
        // Shorten the string and verify the exec call succeeds.
        //

        ExecArgs[0] = "/bin/true";
        ExecArgs[1] = LongString;
        ExecArgs[2] = NULL;
        LongString[LARGE_STRING_SIZE - 2] = '\0';
        LxtCheckErrno(LxtExecve(ExecArgs[0], ExecArgs, NULL));
    }
    else
    {
        LxtCheckResult(ExecveWaitForChild());
    }

    //
    // Allocate a huge argument array.
    //

    ExecArgsLong = malloc(MAX_STACK_SIZE * sizeof(char*));
    if (ExecArgsLong == NULL)
    {
        LxtLogError("malloc of %zd size buffer", (MAX_STACK_SIZE * sizeof(char*)));

        goto ErrorExit;
    }

    for (Index = 0; Index < MAX_STACK_SIZE; Index += 1)
    {
        ExecArgsLong[Index] = "a";
    }

    ExecArgsLong[MAX_STACK_SIZE - 1] = NULL;

    //
    // Create a child and verify that exec fails with too many arguments in the
    // command line or environment variable array.
    //

    LxtCheckResult(ChildPid = fork());
    if (ChildPid == 0)
    {
        ExecArgsLong[0] = "/bin/false";
        ExecArgsLong[MAX_STACK_SIZE - 1] = NULL;
        LxtCheckErrnoFailure(LxtExecve(ExecArgsLong[0], ExecArgsLong, NULL), E2BIG);
        LxtCheckErrnoFailure(LxtExecve(ExecArgsLong[0], NULL, &ExecArgsLong[1]), E2BIG);

        //
        // Shorten the argument list and verify that the command succeeds.
        //

        ExecArgsLong[0] = "/bin/true";
        ExecArgsLong[(MAX_STACK_SIZE / 4)] = NULL;
        LxtCheckErrno(LxtExecve(ExecArgsLong[0], ExecArgsLong, NULL));
    }
    else
    {
        LxtCheckResult(ExecveWaitForChild());
    }

    //
    // Test an empty command line array.
    //

    LxtCheckResult(ChildPid = fork());
    if (ChildPid == 0)
    {
        ExecArgs[0] = NULL;
        LxtCheckErrno(LxtExecve("/bin/echo", ExecArgs, NULL));

        //
        // The parent waits for the child to exit with SIGABRT.
        //
    }
    else
    {
        LxtLogInfo("Waiting for child to exit");
        wait(&Status);
        LxtLogInfo("Status %d", Status);
        LxtCheckTrue((WIFSIGNALED(Status)) && (WTERMSIG(Status) == SIGABRT));
        LxtLogInfo("Child exited with SIGABRT");
    }

    //
    // Test a null command line pointer.
    //

    LxtCheckResult(ChildPid = fork());
    if (ChildPid == 0)
    {
        LxtCheckErrno(LxtExecve("/bin/echo", NULL, NULL));

        //
        // The parent waits for the child to exit SIGABRT.
        //
    }
    else
    {
        LxtLogInfo("Waiting for child to exit");
        wait(&Status);
        LxtLogInfo("Status %d", Status);
        LxtCheckTrue((WIFSIGNALED(Status)) && (WTERMSIG(Status) == SIGABRT));
        LxtLogInfo("Child exited with SIGABRT");
    }

    //
    // Check that executing a directory fails with the expected error code.
    //

    mkdir(LXT_EXECV_TEST_DIRECTORY, 0777);
    ExecArgs[0] = LXT_EXECV_TEST_DIRECTORY;
    ExecArgs[1] = NULL;
    LxtCheckErrnoFailure(LxtExecve(ExecArgs[0], ExecArgs, NULL), EACCES);

    //
    // Verify that exec with a NULL filename fails.
    //

    LxtCheckErrnoFailure(LxtExecve(NULL, NULL, NULL), EFAULT);
    ExecArgs[0] = "/bin/echo";
    ExecArgs[1] = NULL;
    LxtCheckErrnoFailure(LxtExecve(NULL, ExecArgs, NULL), EFAULT);

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    rmdir(LXT_EXECV_TEST_DIRECTORY);
    if (LongString != NULL)
    {
        free(LongString);
    }

    if (ExecArgsLong != NULL)
    {
        free(ExecArgsLong);
    }

    if (ChildPid == 0)
    {
        _exit(Result);
    }

    return Result;
}

int ExecveVariationSingle(PLXT_ARGS Args)

/*++
--*/

{

    int ChildPid;
    int Result;

    LxtLogInfo("Forking single child");
    LxtCheckResult(ChildPid = fork());

    //
    // The child process executes the validation program.
    //

    if (ChildPid == 0)
    {
        LxtCheckResult(ExecveExecValidate(WSL_UNIT_TEST_BINARY));

        //
        // The parent waits for the child to exit successfully.
        //
    }
    else
    {
        LxtCheckResult(ExecveWaitForChild());
    }

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    return Result;
}

int ExecveVariationMultipleWithThreads(PLXT_ARGS Args)

/*++
--*/

{

#define NUM_CHILDREN 32

    int ChildPid;
    int ProcessIndex;
    int Result;

    //
    // Launch all child processes.
    //

    for (ProcessIndex = 0; ProcessIndex < NUM_CHILDREN; ProcessIndex += 1)
    {
        LxtLogInfo("Forking child (#%d)", ProcessIndex);
        LxtCheckResult(ChildPid = fork());

        //
        // In the child, create worker threads and then exec the validation
        // step from the leader.
        //

        if (ChildPid == 0)
        {

            pthread_t Thread;
            int ThreadCount;
            int ThreadIndex;

            ThreadCount = ProcessIndex + 1;
            LxtLogInfo("Creating %d thread(s) for PID %d", ThreadCount, getpid());
            for (ThreadIndex = 0; ThreadIndex < ThreadCount; ThreadIndex += 1)
            {
                LxtCheckResultError(pthread_create(&Thread, NULL, ExecveWorkerThread, NULL));
            }

            //
            // Sleep for 100ms and then execute the validation step.
            //

            usleep(100000);
            LxtCheckResult(ExecveExecValidate(WSL_UNIT_TEST_BINARY));
        }
    }

    //
    // Launch again, this time calling exec from the non-leader thread.
    //

    for (ProcessIndex = 0; ProcessIndex < NUM_CHILDREN; ProcessIndex += 1)
    {
        LxtLogInfo("Forking child (#%d)", ProcessIndex);
        LxtCheckResult(ChildPid = fork());
        if (ChildPid == 0)
        {
            pthread_t Thread;
            int ThreadCount;
            int ThreadIndex;

            ThreadCount = ProcessIndex + 1;
            LxtLogInfo("Creating %d thread(s) for PID %d", ThreadCount, getpid());
            for (ThreadIndex = 0; ThreadIndex < ThreadCount; ThreadIndex += 1)
            {
                LxtCheckResultError(pthread_create(&Thread, NULL, ExecveWorkerThread2, Args));
            }

            //
            // Continuously sleep for 100ms.
            //

            for (;;)
            {
                usleep(100000);
            }
        }
    }

    //
    // Wait for all child processes to exit.
    //

    LxtLogInfo("Waiting for children to exit");
    for (ProcessIndex = 0; ProcessIndex < (2 * NUM_CHILDREN); ProcessIndex += 1)
    {

        LxtCheckResult(ExecveWaitForChild());
        LxtLogInfo("Child exited (#%d)", ProcessIndex);
    }

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    return Result;
}

int ExecveWaitForChild(void)

/*++
--*/

{

    int Result;
    int Status;

    wait(&Status);
    LxtCheckTrue((WIFEXITED(Status)) && (WEXITSTATUS(Status) == 0));
    Result = 0;

ErrorExit:
    return Result;
}

void* ExecveWorkerThread(void* Arg)

/*++
--*/

{

    //
    // Continuously sleep for 100ms.
    //

    for (;;)
    {
        usleep(100000);
    }

    return NULL;
}

void* ExecveWorkerThread2(void* Arg)

/*++
--*/

{

    int Result;

    PLXT_ARGS Args = (PLXT_ARGS)Arg;

    //
    // Sleep for 100ms and then execute the validation step.
    //

    usleep(100000);
    LxtCheckResult(ExecveExecValidate(WSL_UNIT_TEST_BINARY));

ErrorExit:
    return (void*)(long)Result;
}
