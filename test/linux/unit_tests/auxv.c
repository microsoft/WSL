/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    auxv.c

Abstract:

    This file is a test for the auxiliary vector functionality.

--*/

#include <stdio.h>
#include <stdlib.h>
#include <sys/auxv.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include "lxtcommon.h"
#include "unittests.h"

#define AUXV_UID 1004
#define AUXV_GID 1004

#define LXT_NAME "auxv"

#define AUXV_TEST_SCRIPT "auxv_test_script.sh"
#define AUXV_TEST_SCRIPT_SOURCE "#!" AUXV_TEST_PROGRAM_PATH
#define AUXV_TEST_PROGRAM "auxv_test_program"
#define AUXV_TEST_PROGRAM_PATH "/data/test/" AUXV_TEST_PROGRAM
#define AUXV_TEST_PROGRAM_SOURCE_FILE "auxv_test_program.c"
#define AUXV_TEST_PROGRAM_SOURCE \
    "#include <stdio.h>\n" \
    "#include <string.h>\n" \
    "#include <stdlib.h>\n" \
    "#include <sys/auxv.h>\n" \
    "\n" \
    "int main(int Argc, char** Argv)\n" \
    "{\n" \
    "    int Index;\n" \
    "    char* Filename = (char*)getauxval(AT_EXECFN);\n" \
    "    char* Platform = (char*)getauxval(AT_PLATFORM);\n" \
    "    printf(\"AT_EXECFN:   %%s {%%p}\\n\", Filename, Filename);\n" \
    "    printf(\"AT_PLATFORM: %%s {%%p}\\n\", Platform, Platform);\n" \
    "    for (Index = 0; Index < Argc; Index += 1) {\n" \
    "        printf(\"Argv[%%d] = %%s\\n\", Index, Argv[Index]);\n" \
    "    }\n" \
    "    if (Platform > Filename) {\n" \
    "        return -1;\n" \
    "    }\n" \
    "    if ((strcmp(Platform, \"%s\") != 0) ||\n" \
    "        (strcmp(Filename, \"%s\") != 0)) {\n" \
    "        return -1;\n" \
    "    }\n" \
    "    return 0;\n" \
    "}"

int AuxvAtExecfn(PLXT_ARGS Args);

int AuxvGetAuxv(PLXT_ARGS Args);

int AuxvGetAuxvChild(void);

static const LXT_VARIATION g_LxtVariations[] = {{"getauxv", AuxvGetAuxv}, {"AT_EXECFN", AuxvAtExecfn}};

int AuxvTestEntry(int Argc, char* Argv[])
{

    LXT_ARGS Args;
    int ArgvIndex;
    int Result;

    Result = LXT_RESULT_FAILURE;

    //
    // Parse the arguments.
    //

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
            // Run the getauxv child variation.
            //

            return AuxvGetAuxvChild();

            //
            // The below arguments are taken care of by LxtInitialize.
            //

        case 'a':
            break;

        case 'v':
        case 'l':
            ++ArgvIndex;

            break;

        default:
            goto ErrorExit;
        }
    }

    //
    // If -c was not specified, just run the tests
    //

    LxtCheckResult(LxtInitialize(Argc, Argv, &Args, LXT_NAME));
    LxtCheckResult(LxtRunVariations(&Args, g_LxtVariations, LXT_COUNT_OF(g_LxtVariations)));

ErrorExit:
    LxtUninitialize();
    return 0;
}

int AuxvGetAuxv(PLXT_ARGS Args)

{

    char* Argv[4];
    struct stat Buffer;
    int ChildPid;
    int Mode;
    int OriginalMode;
    gid_t OriginalGid;
    uid_t OriginalUid;
    int Result;
    unsigned long Value;

    ChildPid = -1;
    OriginalMode = 0;
    OriginalUid = 0;
    OriginalGid = 0;
    Value = getauxval(AT_SECURE);
    LxtLogInfo("Parent AT_SECURE = %u", Value);
    LxtCheckEqual(Value, 0, "%u");

    // change Args->Argv[0] so that it points to the new single test binary design
    Args->Argv[0] = Argv[0] = WSL_UNIT_TEST_BINARY;
    LxtLogInfo("calling stat(%s)", Args->Argv[0]);
    LxtCheckErrno(stat(Args->Argv[0], &Buffer));
    OriginalMode = Buffer.st_mode;
    OriginalGid = Buffer.st_gid;
    OriginalUid = Buffer.st_uid;

    LxtLogInfo("Setting the set-user-ID bit");
    Mode = OriginalMode | S_ISUID;

    LxtCheckErrno(chown(Args->Argv[0], AUXV_UID, AUXV_UID));
    LxtCheckErrno(chmod(Args->Argv[0], Mode));

    //
    // Start a child process to verify the value of AT_SECURE.
    //

    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        Argv[1] = "auxv";
        Argv[2] = "-c";
        Argv[3] = NULL;
        execve(Args->Argv[0], Argv, NULL);
        LxtLogError("Execve failed, errno: %d (%s)", errno, strerror(errno));
        _exit(LXT_RESULT_FAILURE);
    }

    //
    // Wait for the child to exit.
    //

    LxtCheckResult(LxtWaitPidPoll(ChildPid, LXT_RESULT_SUCCESS));

    LxtLogInfo("Setting the set-group-ID bit");
    Mode = OriginalMode | S_ISGID;
    LxtCheckErrno(chmod(Args->Argv[0], Mode));

    //
    // Start a child process to verify the value of AT_SECURE.
    //

    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        Argv[1] = "auxv";
        Argv[2] = "-c";
        Argv[3] = NULL;
        execve(Argv[0], Argv, NULL);
        LxtLogError("Execve failed, errno: %d (%s)", errno, strerror(errno));
        _exit(LXT_RESULT_FAILURE);
    }

    //
    // Wait for the child to exit.
    //

    LxtCheckResult(LxtWaitPidPoll(ChildPid, LXT_RESULT_SUCCESS));
    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    if (OriginalMode != 0)
    {
        chmod(Args->Argv[0], OriginalMode);
        chown(Args->Argv[0], OriginalUid, OriginalGid);
    }

    if (ChildPid == 0)
    {
        _exit(Result);
    }

    return Result;
}

int AuxvGetAuxvChild(void)

{

    int ChildPid;
    int Result;
    unsigned long Value;

    ChildPid = -1;
    Value = getauxval(AT_SECURE);
    LxtLogInfo("child AT_SECURE = %u", Value);
    LxtCheckEqual(Value, 1, "%u");

    //
    // Start a child process to verify the value of AT_SECURE.
    //

    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        Value = getauxval(AT_SECURE);
        LxtLogInfo("child fork AT_SECURE = %u", Value);
        LxtCheckEqual(Value, 1, "%u");
        Result = LXT_RESULT_SUCCESS;
        goto ErrorExit;
    }

    //
    // Wait for the child to exit.
    //

    LxtCheckResult(LxtWaitPidPoll(ChildPid, LXT_RESULT_SUCCESS));
    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    if (ChildPid == 0)
    {
        _exit(Result);
    }

    return Result;
}

int AuxvAtExecfnCompile(char* Filename)

{

    char* Argv[5];
    char Buffer[1024];
    int ByteCount;
    int ChildPid;
    int Fd;
    char* Platform;
    int Result;

    ChildPid = -1;
    Fd = -1;
    Platform = (char*)getauxval(AT_PLATFORM);

    ByteCount = snprintf(Buffer, sizeof(Buffer), AUXV_TEST_PROGRAM_SOURCE, Platform, Filename);

    if (ByteCount < 0)
    {
        Result = LXT_RESULT_FAILURE;
        LxtLogError("Formatting test source failed %d", errno);
        goto ErrorExit;
    }

    //
    // Create the source file to be compiled.
    //

    LxtCheckErrno(Fd = creat(AUXV_TEST_PROGRAM_SOURCE_FILE, 0755));
    LxtCheckErrno(ByteCount = write(Fd, Buffer, ByteCount));

    //
    // Compile the binary
    //

    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        Filename = "/usr/bin/gcc";
        Argv[0] = "gcc";
        Argv[1] = AUXV_TEST_PROGRAM_SOURCE_FILE;
        Argv[2] = "-o";
        Argv[3] = AUXV_TEST_PROGRAM_PATH;
        Argv[4] = NULL;
        LxtCheckErrno(execv(Filename, Argv));
        goto ErrorExit;
    }

    //
    // Wait for the child to exit.
    //

    LxtCheckResult(LxtWaitPidPollOptions(ChildPid, LXT_RESULT_SUCCESS, 0, 30));

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    if (Fd != -1)
    {
        LxtClose(Fd);
    }

    if (ChildPid == 0)
    {
        _exit(Result);
    }

    unlink(AUXV_TEST_PROGRAM_SOURCE_FILE);
    return Result;
}

int AuxvAtExecfn(PLXT_ARGS Args)

{

    char* Argv[2];
    int ByteCount;
    int ChildPid;
    char* Environment[2];
    int Fd;
    char* Filename;
    int Result;

    ChildPid = -1;
    Fd = -1;
    Filename = AUXV_TEST_PROGRAM_PATH;
    LxtCheckResult(AuxvAtExecfnCompile(Filename));

    //
    // Run the binary with a non-null argument array.
    //

    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        Argv[0] = AUXV_TEST_PROGRAM_PATH;
        Argv[1] = NULL;
        LxtCheckErrno(execv(Filename, Argv));
        goto ErrorExit;
    }

    LxtCheckResult(LxtWaitPidPoll(ChildPid, LXT_RESULT_SUCCESS));

    //
    // Run the binary with a null argument array.
    //

    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        Environment[0] = "FOO=bar";
        Environment[1] = NULL;
        LxtCheckErrno(LxtExecve(Filename, NULL, Environment));
        goto ErrorExit;
    }

    //
    // Wait for the child to exit.
    //

    LxtCheckResult(LxtWaitPidPoll(ChildPid, LXT_RESULT_SUCCESS));

    //
    // Run the binary with an Argv[0] that does not match the filename.
    //

    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        Argv[0] = "FOO";
        Argv[1] = NULL;
        LxtCheckErrno(execv(Filename, Argv));
        goto ErrorExit;
    }

    //
    // Wait for the child to exit.
    //

    LxtCheckResult(LxtWaitPidPoll(ChildPid, LXT_RESULT_SUCCESS));

    //
    // Run the binary with an empty command line.
    //

    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        Environment[0] = "FOO=bar";
        Environment[1] = NULL;
        LxtCheckErrno(LxtExecve(Filename, NULL, Environment));
        goto ErrorExit;
    }

    //
    // Wait for the child to exit.
    //

    LxtCheckResult(LxtWaitPidPoll(ChildPid, LXT_RESULT_SUCCESS));

    //
    // Run the binary with null argument and environment arrays.
    //

    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        LxtCheckErrno(LxtExecve(Filename, NULL, NULL));
        goto ErrorExit;
    }

    //
    // Wait for the child to exit.
    //

    LxtCheckResult(LxtWaitPidPoll(ChildPid, LXT_RESULT_SUCCESS));

    //
    // Create a script that uses #! to launch the test binary.
    //

    LxtCheckErrno(Fd = creat(AUXV_TEST_SCRIPT, 0755));
    LxtCheckErrno(ByteCount = write(Fd, AUXV_TEST_SCRIPT_SOURCE, sizeof(AUXV_TEST_SCRIPT_SOURCE)));
    LxtClose(Fd);
    Fd = -1;

    //
    // Recompile the binary with the script as the expected AT_EXECFN.
    //

    Filename = AUXV_TEST_SCRIPT;
    LxtCheckResult(AuxvAtExecfnCompile(Filename));

    //
    // Run the script with a non-null argument array.
    //

    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        Argv[0] = Filename;
        Argv[1] = NULL;
        LxtCheckErrno(execv(Filename, Argv));
        goto ErrorExit;
    }

    LxtCheckResult(LxtWaitPidPoll(ChildPid, LXT_RESULT_SUCCESS));

    //
    // Run the script with a null argument array.
    //

    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        Environment[0] = "FOO=bar";
        Environment[1] = NULL;
        LxtCheckErrno(LxtExecve(Filename, NULL, Environment));
        goto ErrorExit;
    }

    //
    // Wait for the child to exit.
    //

    LxtCheckResult(LxtWaitPidPoll(ChildPid, LXT_RESULT_SUCCESS));

    //
    // Run the script with an Argv[0] that does not match the filename.
    //

    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        Argv[0] = "FOO";
        Argv[1] = NULL;
        LxtCheckErrno(execv(Filename, Argv));
        goto ErrorExit;
    }

    //
    // Wait for the child to exit.
    //

    LxtCheckResult(LxtWaitPidPoll(ChildPid, LXT_RESULT_SUCCESS));

    //
    // Run the script with an empty command line.
    //

    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        Environment[0] = "FOO=bar";
        Environment[1] = NULL;
        LxtCheckErrno(LxtExecve(Filename, NULL, Environment));
        goto ErrorExit;
    }

    //
    // Wait for the child to exit.
    //

    LxtCheckResult(LxtWaitPidPoll(ChildPid, LXT_RESULT_SUCCESS));

    //
    // Run the script with null argument and environment arrays.
    //

    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        LxtCheckErrno(LxtExecve(Filename, NULL, NULL));
        goto ErrorExit;
    }

    //
    // Wait for the child to exit.
    //

    LxtCheckResult(LxtWaitPidPoll(ChildPid, LXT_RESULT_SUCCESS));

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    if (Fd != -1)
    {
        LxtClose(Fd);
    }

    if (ChildPid == 0)
    {
        _exit(Result);
    }

    unlink(AUXV_TEST_PROGRAM_PATH);
    unlink(AUXV_TEST_SCRIPT);
    return Result;
}
