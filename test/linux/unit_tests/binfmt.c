/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    binfmt.c

Abstract:

    This file contains tests for the binfmt file system.

--*/

#include "lxtcommon.h"
#include "unittests.h"
#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>

#define LXT_NAME "BinFmt"

#define BINFMT_MNT "/proc/sys/fs/binfmt_misc"
#define BINFMT_TEST_FILE "/data/test/lxt_binfmt_test"
#define BINFMT_TIMEOUT 60

#define BINFMT_DISABLE_STRING "0"
#define BINFMT_ENABLE_STRING "1"
#define BINFMT_REMOVE_STRING "-1"

#define BINFMT_REGISTER_NAME "Test"
#define BINFMT_INTERPRETER_SCRIPT "/data/test/lxt_binfmt_interpreter.sh"
#define BINFMT_REGISTER_SCRIPT_STRING ":" BINFMT_REGISTER_NAME ":M::\\xff\\xff\\xff\\xff::" BINFMT_INTERPRETER_SCRIPT ":"

#define BINFMT_INTERPRETER_SCRIPT_CONTENTS \
    "#!/bin/bash\n" \
    "# " BINFMT_INTERPRETER_SCRIPT " - the wrapper for WSL binfmt_misc testing\n" WSL_UNIT_TEST_BINARY " binfmt -a -i \"$@\""

#define BINFMT_INTERPRETER_BINARY "/data/test/lxt_binfmt_interpreter_binary"
#define BINFMT_INTERPRETER_BINARY_SOURCEFILE "/data/test/lxt_binfmt_interpreter_binary.c"

//
// N.B. These UID and GID values must be kept in-sync with the values in the
// source below.
//

#define BINFMT_CALLER_UID 0
#define BINFMT_CALLER_GID 0
#define BINFMT_BINARY_UID 1044
#define BINFMT_BINARY_GID 1044
#define BINFMT_P_FLAG_ARG "foo"

#define BINFMT_INTERPRETER_BINARY_SOURCE_BEGIN \
    "#define _GNU_SOURCE\n" \
    "#include <stdio.h>\n" \
    "#include <string.h>\n" \
    "#include <stdlib.h>\n" \
    "#include <fcntl.h>\n" \
    "#include <unistd.h>\n" \
    "#include <errno.h>\n" \
    "#include <sys/auxv.h>\n" \
    "#define BINFMT_CALLER_UID 0\n" \
    "#define BINFMT_CALLER_GID 0\n" \
    "#define BINFMT_BINARY_UID 1044\n" \
    "#define BINFMT_BINARY_GID 1044\n" \
    "#define BINFMT_P_FLAG_ARG \"foo\"\n" \
    "#define BINFMT_INTERPRETER_BINARY \"/data/test/lxt_binfmt_interpreter_binary\"\n" \
    "#define BINFMT_TEST_FILE \"/data/test/lxt_binfmt_test\"\n" \
    "\n" \
    "int main(int Argc, char** Argv)\n" \
    "{\n" \
    "    struct stat Buffer;\n" \
    "    int Fd;\n" \
    "    int Index;\n" \
    "    uid_t Real, Effective, Saved;\n" \
    "    printf(\"Pid = %d\\n\", getpid());\n" \
    "    Fd = getauxval(AT_EXECFD);\n" \
    "    printf(\"AT_EXECFD = %d errno = %d\\n\", Fd, errno);\n" \
    "    getresuid(&Real, &Effective, &Saved);\n" \
    "    printf(\"Real %d Effective %d Saved %d\\n\", Real, Effective, Saved);\n" \
    "    printf(\"Argc = %d\\n\", Argc);\n" \
    "    for (Index = 0; Index < Argc; Index += 1) {\n" \
    "        printf(\"Argv[%d] = %s\\n\", Index, Argv[Index]);\n" \
    "    }\n"

#define BINFMT_INTERPRETER_BINARY_SOURCE_VERIFY_TWO_ARGS \
    "    if (Argc != 2) {\n" \
    "        return -1;\n" \
    "    }\n" \
    "    if (strcmp(Argv[0], BINFMT_INTERPRETER_BINARY) != 0) {\n" \
    "        return -1;\n" \
    "    }\n" \
    "    if (strcmp(Argv[1], BINFMT_TEST_FILE) != 0) {\n" \
    "        return -1;\n" \
    "    }\n"

#define BINFMT_INTERPRETER_BINARY_SOURCE_MIDDLE_C_FLAG \
    "    if ((Fd == 0) && (errno == ENOENT)) {\n" \
    "        return -1;\n" \
    "    }\n" \
    "    if (fcntl(Fd, F_GETFD) != 0) {\n" \
    "        return -1;\n" \
    "    }\n" \
    "    if ((Real != BINFMT_CALLER_UID) ||\n" \
    "        (Effective != BINFMT_BINARY_UID) ||\n" \
    "        (Saved != BINFMT_BINARY_UID)) {\n" \
    "            return -1;\n" \
    "    }\n" BINFMT_INTERPRETER_BINARY_SOURCE_VERIFY_TWO_ARGS

#define BINFMT_INTERPRETER_BINARY_SOURCE_MIDDLE_O_FLAG \
    "    if ((Fd == 0) && (errno == ENOENT)) {\n" \
    "        return -1;\n" \
    "    }\n" \
    "    if (fcntl(Fd, F_GETFD) != 0) {\n" \
    "        return -1;\n" \
    "    }\n" \
    "    if ((Real != BINFMT_CALLER_UID) ||\n" \
    "        (Effective != BINFMT_CALLER_UID) ||\n" \
    "        (Saved != BINFMT_CALLER_UID)) {\n" \
    "            return -1;\n" \
    "    }\n" BINFMT_INTERPRETER_BINARY_SOURCE_VERIFY_TWO_ARGS

#define BINFMT_INTERPRETER_BINARY_SOURCE_MIDDLE_P_FLAG \
    "    if ((Fd != 0) || (errno != ENOENT)) {\n" \
    "        return -1;\n" \
    "    }\n" \
    "    if (Argc != 4) {\n" \
    "        return -1;\n" \
    "    }\n" \
    "    if (strcmp(Argv[0], BINFMT_INTERPRETER_BINARY) != 0) {\n" \
    "        return -1;\n" \
    "    }\n" \
    "    if (strcmp(Argv[1], BINFMT_TEST_FILE) != 0) {\n" \
    "        return -1;\n" \
    "    }\n" \
    "    if (strcmp(Argv[2], BINFMT_TEST_FILE) != 0) {\n" \
    "        return -1;\n" \
    "    }\n" \
    "    if (strcmp(Argv[3], BINFMT_P_FLAG_ARG) != 0) {\n" \
    "        return -1;\n" \
    "    }\n"

#define BINFMT_INTERPRETER_BINARY_SOURCE_MIDDLE_NO_FLAGS \
    "    if ((Fd != 0) || (errno != ENOENT)) {\n" \
    "        return -1;\n" \
    "    }\n"

#define BINFMT_INTERPRETER_BINARY_SOURCE_END \
    "    return 0;\n" \
    "}"

#define BINFMT_INTERPRETER_BINARY_SOURCE_C_FLAG \
    BINFMT_INTERPRETER_BINARY_SOURCE_BEGIN \
    BINFMT_INTERPRETER_BINARY_SOURCE_MIDDLE_C_FLAG \
    BINFMT_INTERPRETER_BINARY_SOURCE_END

#define BINFMT_INTERPRETER_BINARY_SOURCE_O_FLAG \
    BINFMT_INTERPRETER_BINARY_SOURCE_BEGIN \
    BINFMT_INTERPRETER_BINARY_SOURCE_MIDDLE_O_FLAG \
    BINFMT_INTERPRETER_BINARY_SOURCE_END

#define BINFMT_INTERPRETER_BINARY_SOURCE_P_FLAG \
    BINFMT_INTERPRETER_BINARY_SOURCE_BEGIN \
    BINFMT_INTERPRETER_BINARY_SOURCE_MIDDLE_P_FLAG \
    BINFMT_INTERPRETER_BINARY_SOURCE_END

#define BINFMT_INTERPRETER_BINARY_SOURCE_NO_FLAGS \
    BINFMT_INTERPRETER_BINARY_SOURCE_BEGIN \
    BINFMT_INTERPRETER_BINARY_SOURCE_MIDDLE_NO_FLAGS \
    BINFMT_INTERPRETER_BINARY_SOURCE_END

#define BINFMT_OFFSET_TEST "/data/test/binfmt_offset"
#define BINFMT_OFFSET_TEST_PATTERN "GSH"

#define BINFMT_REGISTER_BINARY_STRING ":" BINFMT_REGISTER_NAME ":M::\\xff\\xff\\xff\\xff::" BINFMT_INTERPRETER_BINARY ":"
#define BINFMT_REGISTER_BINARY_STRING_C BINFMT_REGISTER_BINARY_STRING "C"
#define BINFMT_REGISTER_BINARY_STRING_O BINFMT_REGISTER_BINARY_STRING "O"
#define BINFMT_REGISTER_BINARY_STRING_P BINFMT_REGISTER_BINARY_STRING "P"

#define BINFMT_STATUS_ENABLED "enabled\n"
#define BINFMT_STATUS_DISABLED "disabled\n"

#define BINFMT_REGISTRATION_ENABLED_STRING \
    BINFMT_STATUS_ENABLED \
    "interpreter " BINFMT_INTERPRETER_SCRIPT \
    "\n" \
    "flags: \n" \
    "offset 0\n" \
    "magic ffffffff\n"

#define BINFMT_REGISTRATION_DISABLED_STRING \
    BINFMT_STATUS_DISABLED \
    "interpreter " BINFMT_INTERPRETER_SCRIPT \
    "\n" \
    "flags: \n" \
    "offset 0\n" \
    "magic ffffffff\n"

typedef struct _LXT_BINFMT_REGISTRATION
{
    char* RegistrationString;
    char Magic[4];
    char* TestFile;
} LXT_BINFMT_REGISTRATION, PLXT_BINFMT_REGISTRATION;

LXT_BINFMT_REGISTRATION g_BinfmtRegistrations[] = {
    {":binfmt_1:M::\\x01\x01\x01\x01::/data/test/lxt_binfmt_2:", {0x1, 0x1, 0x1, 0x1}, "/data/test/lxt_binfmt_1"},
    {":binfmt_2:M::\\x02\x02\x02\x02::/data/test/lxt_binfmt_3:", {0x2, 0x2, 0x2, 0x2}, "/data/test/lxt_binfmt_2"},
    {":binfmt_3:M::\\x03\x03\x03\x03::/data/test/lxt_binfmt_4:", {0x3, 0x3, 0x3, 0x3}, "/data/test/lxt_binfmt_3"},
    {":binfmt_4:M::\\x04\x04\x04\x04::/data/test/lxt_binfmt_5:", {0x4, 0x4, 0x4, 0x4}, "/data/test/lxt_binfmt_4"},
    {":binfmt_5:M::\\x05\x05\x05\x05::/data/test/lxt_binfmt_6:", {0x5, 0x5, 0x5, 0x5}, "/data/test/lxt_binfmt_5"},
    {":binfmt_6:M::\\x06\x06\x06\x06::/data/test/lxt_binfmt_7:", {0x6, 0x6, 0x6, 0x6}, "/data/test/lxt_binfmt_6"},
    {":binfmt_7:M::\\x07\x07\x07\x07::/bin/echo:", {0x7, 0x7, 0x7, 0x7}, "/data/test/lxt_binfmt_7"}};

void BimFmtCleanup(void);

int BinFmtExtension(PLXT_ARGS Args);

int BinFmtInvalidParam(PLXT_ARGS Args);

int BinFmtOffset(PLXT_ARGS Args);

int BinFmtOptions(PLXT_ARGS Args);

int BinFmtRegister(PLXT_ARGS Args);

int BinFmtRoot(PLXT_ARGS Args);

int BinFmtStatus(PLXT_ARGS Args);

int BinFmtInterpreterEntry(PLXT_ARGS Args);

//
// Global constants
//

static const LXT_VARIATION g_LxtVariations[] = {
    {"BinFmt - " BINFMT_MNT " root", BinFmtRoot},
    {"BinFmt - " BINFMT_MNT "/register", BinFmtRegister},
    {"BinFmt - " BINFMT_MNT "/status", BinFmtStatus},
    {"BinFmt - Extensions", BinFmtExtension},
    {"BinFmt - Options", BinFmtOptions},
    {"BinFmt - Offset", BinFmtOffset},
    {"BinFmt - Invalid Parameter", BinFmtInvalidParam}};

static const LXT_CHILD_INFO g_BinFmtRootChildren[] = {{"register", DT_REG}, {"status", DT_REG}};

static const char* g_BinFmtRegisterInvalid[] = {
    "::M::BACON::/usr/bin/test:",
    ":Test:B::BACON::/usr/bin/test:",
    ":Test:M::BACON:BACONISAWESOME:/usr/bin/test:",
    ":Test:M::BACON:\\xff:/usr/bin/test:",
    ":Test:M::BACON:\\xff\\xff\\xff\\xff\\xf:/usr/bin/test:",
    ":Test:M::BACON:::",
    ":Test:M::BACON::/usr/bin/test:B",
    ":Test:M::BACON::/usr/bin/test: ",
    ":Test:M::BACON::/usr/bin/test:\nO",
    ":Test:E::B/ACON::/usr/bin/test:",

    /*
     ":aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa:M::BACON::/usr/bin/aaaaaaaaa:",
     ":aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa:M::BACON::/usr/bin/aaaaaaaaaaaaaaa:",
     ":aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa:M::BACON::/usr/bin/aaaaaaaaaaaaaaaaaaaaaaaa:",
     ":aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa:M::BACON::/usr/bin/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa:",
     ":aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa:M::BACON::/usr/bin/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa:",
    */

    ":::::::::::::::::",
    "",
    "\0"};

int BinFmtTestEntry(int Argc, char* Argv[])

/*++

Routine Description:

    This routine is the main entry point for the binfmt tests.

Arguments:

    Argc - Supplies the number of command line arguments.

    Argv - Supplies the command line arguments.

Return Value:    LxtCheckResult(LxtInitialize(Argc, Argv, &Args, LXT_NAME));

    Returns 0 on success, -1 on failure.

--*/

{

    LXT_ARGS Args;
    int Opt;
    int Result;

    LxtCheckResult(LxtInitialize(Argc, Argv, &Args, LXT_NAME));
    optind = 0;
    opterr = 0;
    while ((Opt = getopt(Argc, Argv, "i")) != -1)
    {
        switch (Opt)
        {
        case 'i':
            Result = BinFmtInterpreterEntry(&Args);
            goto ErrorExit;
        }
    }

    LxtCheckResult(LxtRunVariations(&Args, g_LxtVariations, LXT_COUNT_OF(g_LxtVariations)));

ErrorExit:
    LxtUninitialize();
    return !LXT_SUCCESS(Result);
}

void BimFmtCleanup(void)

{

    int Fd;
    int Result;
    int Size;

    //
    // Remove the test entry via the registration file.
    //

    Fd = open(BINFMT_MNT "/" BINFMT_REGISTER_NAME, O_RDWR);
    if (Fd >= 0)
    {
        Size = strlen(BINFMT_REMOVE_STRING);
        LxtCheckErrno(Size = write(Fd, BINFMT_REMOVE_STRING, Size));
    }

ErrorExit:
    if (Fd != -1)
    {
        LxtClose(Fd);
    }

    return;
}

int BinFmtExtension(PLXT_ARGS Args)

/*++

Description:

    This routine tests binformat extensions.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    char Buffer[16];
    size_t BytesWritten;
    int ChildPid;
    char* ExecArgs[2];
    int Fd = -1;
    int Index;
    int RegisterFd;
    LXT_CHILD_INFO Registration;
    int Result;
    ssize_t Size;
    int Status;

    //
    // Clean any binfmt interpreters from a previous iteration of the test.
    //

    BimFmtCleanup();

    //
    // Create the binfmt interpreter.
    //

    LxtCheckErrno(Fd = creat(BINFMT_INTERPRETER_SCRIPT, 0777));
    LxtCheckErrno(BytesWritten = write(Fd, BINFMT_INTERPRETER_SCRIPT_CONTENTS, (sizeof(BINFMT_INTERPRETER_SCRIPT_CONTENTS) - 1)));

    LxtClose(Fd);
    Fd = -1;

    //
    // Register a binfmt extension.
    //

    LxtCheckErrno(RegisterFd = open(BINFMT_MNT "/register", O_WRONLY));
    Size = strlen(BINFMT_REGISTER_SCRIPT_STRING);
    LxtCheckErrno(Size = write(RegisterFd, BINFMT_REGISTER_SCRIPT_STRING, Size));

    //
    // Create a file that will be handled by the binfmt extension.
    //

    LxtCheckErrno(Fd = creat(BINFMT_TEST_FILE, 0777));
    memset(Buffer, 0xff, sizeof(Buffer));
    LxtCheckErrno(BytesWritten = write(Fd, Buffer, sizeof(Buffer)));
    LxtClose(Fd);
    Fd = -1;

    //
    // Fork and exec the file.
    //

    LxtCheckResult(ChildPid = fork());
    if (ChildPid == 0)
    {
        ExecArgs[0] = BINFMT_TEST_FILE;
        ExecArgs[1] = NULL;
        LxtCheckErrno(LxtExecve(ExecArgs[0], ExecArgs, NULL));

        //
        // The parent waits for the child to exit successfully.
        //
    }
    else
    {
        LxtCheckResult(LxtWaitPidPoll(ChildPid, 0));
    }

    //
    // Remove the new entry via the registration file.
    //

    LxtCheckErrno(Fd = open(BINFMT_MNT "/" BINFMT_REGISTER_NAME, O_RDWR));
    Size = strlen(BINFMT_REMOVE_STRING);
    LxtCheckErrno(Size = write(Fd, BINFMT_REMOVE_STRING, Size));
    LxtClose(Fd);
    Fd = -1;

    //
    // Create many registrations and test files.
    //

    for (Index = 0; Index < LXT_COUNT_OF(g_BinfmtRegistrations); Index += 1)
    {
        Size = strlen(g_BinfmtRegistrations[Index].RegistrationString);
        LxtCheckErrno(Size = write(RegisterFd, g_BinfmtRegistrations[Index].RegistrationString, Size));
        LxtCheckErrno(Fd = creat(g_BinfmtRegistrations[Index].TestFile, 0777));
        LxtCheckErrno(BytesWritten = write(Fd, g_BinfmtRegistrations[Index].Magic, sizeof(g_BinfmtRegistrations[Index].Magic)));
        LxtClose(Fd);
        Fd = -1;
    }

    //
    // Fork and exec the file to test the interpreter depth.
    //

    Index = 2;
    LxtCheckResult(ChildPid = fork());
    if (ChildPid == 0)
    {
        ExecArgs[0] = g_BinfmtRegistrations[Index].TestFile;
        ExecArgs[1] = NULL;
        LxtCheckErrno(LxtExecve(ExecArgs[0], ExecArgs, NULL));

        //
        // The parent waits for the child to exit successfully.
        //
    }
    else
    {
        LxtCheckResult(LxtWaitPidPoll(ChildPid, 0));
    }

    //
    // Test max interpreter link depth (should fail).
    //

    Index = 1;
    LxtCheckResult(ChildPid = fork());
    if (ChildPid == 0)
    {
        ExecArgs[0] = g_BinfmtRegistrations[Index].TestFile;
        ExecArgs[1] = NULL;
        LxtCheckErrnoFailure(LxtExecve(ExecArgs[0], ExecArgs, NULL), ELOOP);
        exit(0);

        //
        // The parent waits for the child to exit.
        //
    }
    else
    {
        LxtCheckResult(LxtWaitPidPoll(ChildPid, 0));
    }

    //
    // Remove the entries via the status file.
    //

    LxtCheckErrno(Fd = open(BINFMT_MNT "/status", O_RDWR));
    Size = strlen(BINFMT_REMOVE_STRING);
    LxtCheckErrno(Size = write(Fd, BINFMT_REMOVE_STRING, Size));
    LxtClose(Fd);
    Fd = -1;

ErrorExit:
    if (RegisterFd > 0)
    {
        LxtClose(RegisterFd);
    }

    if (Fd > 0)
    {
        LxtClose(Fd);
    }

    if (ChildPid == 0)
    {
        _exit(Result);
    }

    return Result;
}

int BinFmtInvalidParam(PLXT_ARGS Args)

/*++

Description:

    This routine tests invalid argument handling for the binfmt register file.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int Fd;
    int Index;
    int Result;
    ssize_t Size;

    LxtCheckErrno(Fd = open(BINFMT_MNT "/register", O_WRONLY));
    for (Index = 0; Index < LXT_COUNT_OF(g_BinFmtRegisterInvalid); Index += 1)
    {
        LxtLogInfo("Index[%d] %s", Index, g_BinFmtRegisterInvalid[Index]);
        Size = strlen(g_BinFmtRegisterInvalid[Index]);
        LxtCheckErrnoFailure(Size = write(Fd, g_BinFmtRegisterInvalid[Index], Size), EINVAL);
    }

ErrorExit:
    if (Fd > 0)
    {
        LxtClose(Fd);
    }

    return Result;
}

int BinFmtOffset(PLXT_ARGS Args)

/*++

Description:

    This routine tests binformat interpreter options.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int ChildPid;
    char* ExecArgs[2];
    int Fd;
    char RegisterString[] = ":" BINFMT_REGISTER_NAME ":M:2:" BINFMT_OFFSET_TEST_PATTERN "::/bin/true:";
    int Result;
    ssize_t Size;

    //
    // Register an interpreter with a known string at a two byte offset.
    //

    LxtCheckErrno(Fd = open(BINFMT_MNT "/register", O_WRONLY));
    Size = strlen(RegisterString);
    LxtCheckErrno(Size = write(Fd, RegisterString, Size));

    //
    // Create a test file that matches this pattern.
    //

    LxtClose(Fd);
    LxtCheckErrno(Fd = creat(BINFMT_OFFSET_TEST, 0777));
    LxtCheckErrno(Size = write(Fd, "00" BINFMT_OFFSET_TEST_PATTERN, sizeof(BINFMT_OFFSET_TEST_PATTERN) + 2));
    LxtClose(Fd);
    Fd = -1;

    //
    // Fork and exec the file and ensure that the binfmt interpreter is invoked.
    //

    LxtCheckResult(ChildPid = fork());
    if (ChildPid == 0)
    {
        ExecArgs[0] = BINFMT_OFFSET_TEST;
        ExecArgs[1] = NULL;
        LxtCheckErrno(LxtExecve(ExecArgs[0], ExecArgs, NULL));
        goto ErrorExit;
    }

    //
    // Wait for the child to exit.
    //

    LxtCheckResult(LxtWaitPidPoll(ChildPid, 0));

    //
    // Create a test file that does not match the pattern.
    //

    LxtCheckErrno(Fd = creat(BINFMT_OFFSET_TEST, 0777));
    LxtCheckErrno(Size = write(Fd, BINFMT_OFFSET_TEST_PATTERN, sizeof(BINFMT_OFFSET_TEST_PATTERN)));
    LxtClose(Fd);
    Fd = -1;

    //
    // Fork and exec the file and ensure that the exec fails.
    //

    LxtCheckResult(ChildPid = fork());
    if (ChildPid == 0)
    {
        ExecArgs[0] = BINFMT_OFFSET_TEST;
        ExecArgs[1] = NULL;
        LxtCheckErrnoFailure(LxtExecve(ExecArgs[0], ExecArgs, NULL), ENOEXEC);
        goto ErrorExit;
    }

    //
    // Wait for the child to exit.
    //

    LxtCheckResult(LxtWaitPidPoll(ChildPid, 0));

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    if (Fd > 0)
    {
        LxtClose(Fd);
    }

    if (ChildPid == 0)
    {
        _exit(Result);
    }

    //
    // Unregister the interpreter and delete the test file.
    //

    BimFmtCleanup();
    unlink(BINFMT_OFFSET_TEST);

    return Result;
}

int BinFmtOptions(PLXT_ARGS Args)

/*++

Description:

    This routine tests binformat interpreter options.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    char* Argv[5];
    char Buffer[16];
    size_t BytesWritten;
    int ChildPid;
    char* ExecArgs[3];
    int Fd = -1;
    int Index;
    int RegisterFd;
    LXT_CHILD_INFO Registration;
    int Result;
    ssize_t Size;
    int Status;

    //
    // Clean any binfmt interpreters from a previous iteration of the test.
    //

    BimFmtCleanup();

    //
    // Create a file that will be handled by the binfmt extension.
    //

    LxtCheckErrno(Fd = creat(BINFMT_TEST_FILE, 0777));
    memset(Buffer, 0xff, sizeof(Buffer));
    LxtCheckErrno(BytesWritten = write(Fd, Buffer, sizeof(Buffer)));
    LxtCheckErrno(fchown(Fd, BINFMT_BINARY_UID, BINFMT_BINARY_GID));
    LxtCheckErrno(fchmod(Fd, 0777 | S_ISUID));
    LxtClose(Fd);
    Fd = -1;

    //
    // Create a binfmt interpreter without any flags.
    //

    LxtLogInfo("Testing no flags");
    LxtCheckErrno(Fd = creat(BINFMT_INTERPRETER_BINARY_SOURCEFILE, 0777));
    LxtCheckErrno(BytesWritten = write(Fd, BINFMT_INTERPRETER_BINARY_SOURCE_NO_FLAGS, (sizeof(BINFMT_INTERPRETER_BINARY_SOURCE_NO_FLAGS) - 1)));

    LxtClose(Fd);
    Fd = -1;

    //
    // Compile the binary
    //

    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        Argv[0] = "gcc";
        Argv[1] = BINFMT_INTERPRETER_BINARY_SOURCEFILE;
        Argv[2] = "-o";
        Argv[3] = BINFMT_INTERPRETER_BINARY;
        Argv[4] = NULL;
        LxtCheckErrno(execv("/usr/bin/gcc", Argv));
        goto ErrorExit;
    }

    //
    // Wait for the child to exit.
    //

    LxtCheckResult(LxtWaitPidPollOptions(ChildPid, LXT_RESULT_SUCCESS, 0, BINFMT_TIMEOUT));

    //
    // Register a binfmt extension without flags.
    //

    LxtCheckErrno(RegisterFd = open(BINFMT_MNT "/register", O_WRONLY));
    Size = strlen(BINFMT_REGISTER_BINARY_STRING);
    LxtCheckErrno(Size = write(RegisterFd, BINFMT_REGISTER_BINARY_STRING, Size));

    //
    // Fork and exec the file.
    //

    LxtCheckResult(ChildPid = fork());
    if (ChildPid == 0)
    {
        LxtCheckErrno(LxtSetresuid(BINFMT_CALLER_UID, BINFMT_CALLER_UID, BINFMT_CALLER_UID));
        ExecArgs[0] = BINFMT_TEST_FILE;
        ExecArgs[1] = NULL;
        LxtCheckErrno(LxtExecve(ExecArgs[0], ExecArgs, NULL));
    }

    //
    // Wait for the child to exit.
    //

    LxtCheckResult(LxtWaitPidPollOptions(ChildPid, 0, 0, BINFMT_TIMEOUT));

    //
    // Unregister the interpreter.
    //

    BimFmtCleanup();

    //
    // Create the binfmt interpreter that handles the 'O' flag.
    //

    LxtLogInfo("Testing 'O' flag");
    LxtCheckErrno(Fd = creat(BINFMT_INTERPRETER_BINARY_SOURCEFILE, 0777));
    LxtCheckErrno(BytesWritten = write(Fd, BINFMT_INTERPRETER_BINARY_SOURCE_O_FLAG, (sizeof(BINFMT_INTERPRETER_BINARY_SOURCE_O_FLAG) - 1)));

    LxtClose(Fd);
    Fd = -1;

    //
    // Compile the binary
    //

    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        Argv[0] = "gcc";
        Argv[1] = BINFMT_INTERPRETER_BINARY_SOURCEFILE;
        Argv[2] = "-o";
        Argv[3] = BINFMT_INTERPRETER_BINARY;
        Argv[4] = NULL;
        LxtCheckErrno(execv("/usr/bin/gcc", Argv));
        goto ErrorExit;
    }

    //
    // Wait for the child to exit.
    //

    LxtCheckResult(LxtWaitPidPollOptions(ChildPid, LXT_RESULT_SUCCESS, 0, BINFMT_TIMEOUT));

    //
    // Register a binfmt extension.
    //

    LxtCheckErrno(RegisterFd = open(BINFMT_MNT "/register", O_WRONLY));
    Size = strlen(BINFMT_REGISTER_BINARY_STRING_O);
    LxtCheckErrno(Size = write(RegisterFd, BINFMT_REGISTER_BINARY_STRING_O, Size));

    //
    // Fork and exec the file.
    //

    LxtCheckResult(ChildPid = fork());
    if (ChildPid == 0)
    {
        LxtCheckErrno(LxtSetresuid(BINFMT_CALLER_UID, BINFMT_CALLER_UID, BINFMT_CALLER_UID));
        ExecArgs[0] = BINFMT_TEST_FILE;
        ExecArgs[1] = NULL;
        LxtCheckErrno(LxtExecve(ExecArgs[0], ExecArgs, NULL));
    }

    //
    // Wait for the child to exit.
    //

    LxtCheckResult(LxtWaitPidPollOptions(ChildPid, 0, 0, BINFMT_TIMEOUT));

    //
    // Unregister the interpreter.
    //

    BimFmtCleanup();

    //
    // Create the binfmt interpreter that handles the 'C' flag.
    //

    LxtLogInfo("Testing 'C' flag");
    LxtCheckErrno(Fd = creat(BINFMT_INTERPRETER_BINARY_SOURCEFILE, 0777));
    LxtCheckErrno(BytesWritten = write(Fd, BINFMT_INTERPRETER_BINARY_SOURCE_C_FLAG, (sizeof(BINFMT_INTERPRETER_BINARY_SOURCE_C_FLAG) - 1)));

    LxtClose(Fd);
    Fd = -1;

    //
    // Compile the binary
    //

    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        Argv[0] = "gcc";
        Argv[1] = BINFMT_INTERPRETER_BINARY_SOURCEFILE;
        Argv[2] = "-o";
        Argv[3] = BINFMT_INTERPRETER_BINARY;
        Argv[4] = NULL;
        LxtCheckErrno(execv("/usr/bin/gcc", Argv));
        goto ErrorExit;
    }

    //
    // Wait for the child to exit.
    //

    LxtCheckResult(LxtWaitPidPollOptions(ChildPid, LXT_RESULT_SUCCESS, 0, BINFMT_TIMEOUT));

    //
    // Register a binfmt extension.
    //

    LxtCheckErrno(RegisterFd = open(BINFMT_MNT "/register", O_WRONLY));
    Size = strlen(BINFMT_REGISTER_BINARY_STRING_C);
    LxtCheckErrno(Size = write(RegisterFd, BINFMT_REGISTER_BINARY_STRING_C, Size));

    //
    // Fork and exec the file.
    //

    LxtCheckResult(ChildPid = fork());
    if (ChildPid == 0)
    {
        LxtCheckErrno(LxtSetresuid(BINFMT_CALLER_UID, BINFMT_CALLER_UID, BINFMT_CALLER_UID));
        ExecArgs[0] = BINFMT_TEST_FILE;
        ExecArgs[1] = NULL;
        LxtCheckErrno(LxtExecve(ExecArgs[0], ExecArgs, NULL));
    }

    //
    // Wait for the child to exit.
    //

    LxtCheckResult(LxtWaitPidPollOptions(ChildPid, 0, 0, BINFMT_TIMEOUT));

    //
    // Unregister the interpreter.
    //

    BimFmtCleanup();

    //
    // Create the binfmt interpreter that handles the 'P' flag.
    //

    LxtLogInfo("Testing 'P' flag");
    LxtCheckErrno(Fd = creat(BINFMT_INTERPRETER_BINARY_SOURCEFILE, 0777));
    LxtCheckErrno(BytesWritten = write(Fd, BINFMT_INTERPRETER_BINARY_SOURCE_P_FLAG, (sizeof(BINFMT_INTERPRETER_BINARY_SOURCE_P_FLAG) - 1)));

    LxtClose(Fd);
    Fd = -1;

    //
    // Compile the binary
    //

    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        Argv[0] = "gcc";
        Argv[1] = BINFMT_INTERPRETER_BINARY_SOURCEFILE;
        Argv[2] = "-o";
        Argv[3] = BINFMT_INTERPRETER_BINARY;
        Argv[4] = NULL;
        LxtCheckErrno(execv("/usr/bin/gcc", Argv));
        goto ErrorExit;
    }

    //
    // Wait for the child to exit.
    //

    LxtCheckResult(LxtWaitPidPollOptions(ChildPid, LXT_RESULT_SUCCESS, 0, BINFMT_TIMEOUT));

    //
    // Register a binfmt extension.
    //

    LxtCheckErrno(RegisterFd = open(BINFMT_MNT "/register", O_WRONLY));
    Size = strlen(BINFMT_REGISTER_BINARY_STRING_P);
    LxtCheckErrno(Size = write(RegisterFd, BINFMT_REGISTER_BINARY_STRING_P, Size));

    //
    // Fork and exec the file.
    //

    LxtCheckResult(ChildPid = fork());
    if (ChildPid == 0)
    {
        ExecArgs[0] = BINFMT_TEST_FILE;
        ExecArgs[1] = BINFMT_P_FLAG_ARG;
        ExecArgs[2] = NULL;
        LxtCheckErrno(LxtExecve(ExecArgs[0], ExecArgs, NULL));
    }

    //
    // Wait for the child to exit.
    //

    LxtCheckResult(LxtWaitPidPollOptions(ChildPid, 0, 0, BINFMT_TIMEOUT));

    //
    // Unregister the interpreter.
    //

    BimFmtCleanup();

ErrorExit:
    if (RegisterFd > 0)
    {
        LxtClose(RegisterFd);
    }

    if (Fd > 0)
    {
        LxtClose(Fd);
    }

    if (ChildPid == 0)
    {
        _exit(Result);
    }

    return Result;
}

int BinFmtRoot(PLXT_ARGS Args)

/*++

Description:

    This routine tests the contents of the binfmt directory.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int Result;

    // LxtCheckResult(LxtCheckStat(BINFMT_MNT, 1, DT_DIR));
    LxtCheckResult(LxtCheckDirectoryContents(BINFMT_MNT, g_BinFmtRootChildren, LXT_COUNT_OF(g_BinFmtRootChildren)));

ErrorExit:
    return Result;
}

int BinFmtRegister(PLXT_ARGS Args)

/*++

Description:

    This routine tests the binfmt register file.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    char Buffer[128];
    int Fd;
    LXT_CHILD_INFO Registration;
    int Result;
    ssize_t Size;

    //
    // Clean up any previously registered interpreters.
    //

    BimFmtCleanup();

    //
    // Open the register file and verify that binfmt registrations are able to be registered.
    //

    LxtCheckErrno(Fd = open(BINFMT_MNT "/register", O_WRONLY));

    Size = strlen(BINFMT_REGISTER_SCRIPT_STRING);
    LxtCheckErrno(Size = write(Fd, BINFMT_REGISTER_SCRIPT_STRING, Size));
    LxtClose(Fd);
    Fd = -1;

    Registration.Name = BINFMT_REGISTER_NAME;
    Registration.FileType = DT_REG;

    LxtCheckResult(LxtCheckDirectoryContents(BINFMT_MNT, &Registration, 1));

    //
    // Open the registration file and verify that it behaves as expected.
    // Status should initially be enabled.
    //

    LxtCheckErrno(Fd = open(BINFMT_MNT "/" BINFMT_REGISTER_NAME, O_RDWR));
    LxtCheckErrno(Size = read(Fd, Buffer, sizeof(Buffer) - 1));
    Buffer[Size] = '\0';
    LxtClose(Fd);
    Fd = -1;

    LxtCheckStringEqual(Buffer, BINFMT_REGISTRATION_ENABLED_STRING);

    //
    // Disable the registration and verify the string changes.
    //

    LxtCheckErrno(Fd = open(BINFMT_MNT "/" BINFMT_REGISTER_NAME, O_RDWR));
    Size = strlen(BINFMT_DISABLE_STRING);
    LxtCheckErrno(Size = write(Fd, BINFMT_DISABLE_STRING, Size));
    LxtClose(Fd);
    Fd = -1;

    LxtCheckErrno(Fd = open(BINFMT_MNT "/" BINFMT_REGISTER_NAME, O_RDWR));
    LxtCheckErrno(Size = read(Fd, Buffer, sizeof(Buffer) - 1));
    Buffer[Size] = '\0';
    LxtClose(Fd);
    Fd = -1;

    LxtCheckStringEqual(Buffer, BINFMT_REGISTRATION_DISABLED_STRING);

    //
    // Enable and verify the string changes.
    //

    LxtCheckErrno(Fd = open(BINFMT_MNT "/" BINFMT_REGISTER_NAME, O_RDWR));
    Size = strlen(BINFMT_ENABLE_STRING);
    LxtCheckErrno(Size = write(Fd, BINFMT_ENABLE_STRING, Size));
    LxtClose(Fd);
    Fd = -1;

    LxtCheckErrno(Fd = open(BINFMT_MNT "/" BINFMT_REGISTER_NAME, O_RDWR));
    LxtCheckErrno(Size = read(Fd, Buffer, sizeof(Buffer) - 1));
    Buffer[Size] = '\0';
    LxtClose(Fd);
    Fd = -1;

    LxtCheckStringEqual(Buffer, BINFMT_REGISTRATION_ENABLED_STRING);

    //
    // Remove the new entry via the registration file.
    //

    LxtCheckErrno(Fd = open(BINFMT_MNT "/" BINFMT_REGISTER_NAME, O_RDWR));
    Size = strlen(BINFMT_REMOVE_STRING);
    LxtCheckErrno(Size = write(Fd, BINFMT_REMOVE_STRING, Size));
    LxtClose(Fd);
    Fd = -1;

    //
    // Attempt to open the file (should fail);
    //

    LxtCheckErrnoFailure(Fd = open(BINFMT_MNT "/" BINFMT_REGISTER_NAME, O_RDWR), ENOENT);

ErrorExit:
    if (Fd > 0)
    {
        LxtClose(Fd);
    }

    return Result;
}

int BinFmtStatus(PLXT_ARGS Args)

/*++

Description:

    This routine tests the binfmt status file.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    char Buffer[64];
    int Fd;
    LXT_CHILD_INFO Registration;
    int Result;
    ssize_t Size;

    //
    // Open the status file and verify that status behaves as expected.
    // Status should initially be enabled.
    //

    LxtCheckErrno(Fd = open(BINFMT_MNT "/status", O_RDWR));
    LxtCheckErrno(Size = read(Fd, Buffer, sizeof(Buffer) - 1));
    Buffer[Size] = '\0';
    LxtClose(Fd);
    Fd = -1;

    LxtCheckStringEqual(Buffer, BINFMT_STATUS_ENABLED);

    //
    // Disable status and verify the string changes.
    //

    LxtCheckErrno(Fd = open(BINFMT_MNT "/status", O_RDWR));
    Size = strlen(BINFMT_DISABLE_STRING);
    LxtCheckErrno(Size = write(Fd, BINFMT_DISABLE_STRING, Size));
    LxtClose(Fd);
    Fd = -1;

    LxtCheckErrno(Fd = open(BINFMT_MNT "/status", O_RDWR));
    LxtCheckErrno(Size = read(Fd, Buffer, sizeof(Buffer) - 1));
    Buffer[Size] = '\0';
    LxtClose(Fd);
    Fd = -1;

    LxtCheckStringEqual(Buffer, BINFMT_STATUS_DISABLED);

    //
    // Enable and verify the string changes.
    //

    LxtCheckErrno(Fd = open(BINFMT_MNT "/status", O_RDWR));
    Size = strlen(BINFMT_ENABLE_STRING);
    LxtCheckErrno(Size = write(Fd, BINFMT_ENABLE_STRING, Size));
    LxtClose(Fd);
    Fd = -1;

    LxtCheckErrno(Fd = open(BINFMT_MNT "/status", O_RDWR));
    LxtCheckErrno(Size = read(Fd, Buffer, sizeof(Buffer) - 1));
    Buffer[Size] = '\0';
    LxtClose(Fd);
    Fd = -1;

    LxtCheckStringEqual(Buffer, BINFMT_STATUS_ENABLED);

    //
    // Register a binfmt extension and verify that it is removed when
    // -1 is written to the status file.
    //

    LxtCheckErrno(Fd = open(BINFMT_MNT "/register", O_RDWR));
    Size = strlen(BINFMT_REGISTER_SCRIPT_STRING);
    LxtCheckErrno(Size = write(Fd, BINFMT_REGISTER_SCRIPT_STRING, Size));
    LxtClose(Fd);
    Fd = -1;

    Registration.Name = BINFMT_REGISTER_NAME;
    Registration.FileType = DT_REG;

    LxtCheckResult(LxtCheckDirectoryContents(BINFMT_MNT, &Registration, 1));

    //
    // Remove the new entry via the status file.
    //

    LxtCheckErrno(Fd = open(BINFMT_MNT "/status", O_RDWR));
    Size = strlen(BINFMT_REMOVE_STRING);
    LxtCheckErrno(Size = write(Fd, BINFMT_REMOVE_STRING, Size));
    LxtClose(Fd);
    Fd = -1;

    //
    // Attempt to open the file (should fail);
    //

    LxtCheckErrnoFailure(Fd = open(BINFMT_MNT "/" BINFMT_REGISTER_NAME, O_RDWR), ENOENT);

ErrorExit:
    if (Fd > 0)
    {
        LxtClose(Fd);
    }

    return Result;
}

int BinFmtInterpreterEntry(PLXT_ARGS Args)

/*++

Description:

    This routine implements the entry point for the test binfmt interpreter.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int Index;
    int Result;
    printf("Pid = %d\n", getpid());
    for (Index = 0; Index < Args->Argc; Index += 1)
    {
        printf("Argv[%d]: %s\n", Index, Args->Argv[Index]);
    }

    return 0;
}
