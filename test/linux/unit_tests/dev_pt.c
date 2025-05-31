/*++

Copyright (c) Microsoft Corporation. All rights reserved.

Module Name:

    dev_pt.c

Abstract:

    This file is a test for the Pseudo Terminals: /dev/ptmx, /dev/pts/<n>
    devices.

--*/

#include "dev_pt_common.h"

#define LXT_NAME "dev_pt"

//
// Currently the max pseudo terminals that is supported by LXSS
// is set to 10.
// TODO_LX_PTYT: Query this from '/proc/sys/kernel/pty/nr' after
//     the integration with procfs.
//

#define PTY_MAX_OPEN_LIMIT 10

//
// Configuration to be used for the stress test.
// Total number of cycles =
//     (STRESS_NUM_PT * STRESS_NUM_THREAD * STRESS_NUM_ITERATION)
//

#define STRESS_NUM_PT 5
#define STRESS_NUM_THREAD 100
#define STRESS_NUM_ITERATION 6400

#define IS_CONTROL_CHAR_ECHO_STRING(s, c) (((s)[0] == '^') && (((s)[1] > 0x40)) && (((s)[1] - 0x40) == (c)))

//
// Globals.
//

pthread_mutex_t DevPtStressMutex = PTHREAD_MUTEX_INITIALIZER;

//
// struct that defines the argument passed to a stress thread.
//

typedef struct _StressThreadArg
{
    int PtmFd;
    int PtsFd;
    int LoopCount;
} StressThreadArg;

//
// Functions.
//

void* PerformIoStressThread(void* Config);

//
// Test cases.
//

int PtBasic(PLXT_ARGS Args);

int PtBasic2(PLXT_ARGS Args);

int PtBasic3(PLXT_ARGS Args);

int PtBasic4(PLXT_ARGS Args);

int PtBasic5(PLXT_ARGS Args);

int PtCheck1(PLXT_ARGS Args);

int PtCheck2(PLXT_ARGS Args);

int PtCheck3(PLXT_ARGS Args);

int PtCheck4(PLXT_ARGS Args);

int PtControlCharCheck(PLXT_ARGS Args);

int PtControlCharCheck2(PLXT_ARGS Args);

int PtControlCharCheck3(PLXT_ARGS Args);

int PtControlCharCheck4(PLXT_ARGS Args);

int PtControlCharCheck5(PLXT_ARGS Args);

int PtControlCharCheck6(PLXT_ARGS Args);

int PtDisassociateTty(PLXT_ARGS Args);

int PtEmbeddedNullReadWrite(PLXT_ARGS Args);

int PtEraseCheck(PLXT_ARGS Args);

int PtEraseCheck2(PLXT_ARGS Args);

int PtEraseCheck3(PLXT_ARGS Args);

int PtEraseCheck4(PLXT_ARGS Args);

int PtGlibcForkPtyBasic(PLXT_ARGS Args);

int PtLateOpen1(PLXT_ARGS Args);

int PtLateOpen2(PLXT_ARGS Args);

int PtLineDiscipline(PLXT_ARGS Args);

int PtLineBreakCheck(PLXT_ARGS Args);

int PtLineBreakCheck2(PLXT_ARGS Args);

int PtLineBreakCheck3(PLXT_ARGS Args);

int PtLineBreakCheck4(PLXT_ARGS Args);

int PtLineBreakCheck5(PLXT_ARGS Args);

int PtLineBreakCheck6(PLXT_ARGS Args);

int PtLineBreakCheck7(PLXT_ARGS Args);

int PtLineBreakCheck8(PLXT_ARGS Args);

int PtLineBreakCheck9(PLXT_ARGS Args);

int PtLineBreakCheck10(PLXT_ARGS Args);

int PtMasterFillBuffer(PLXT_ARGS Args);

int PtMasterHangup1(PLXT_ARGS Args);

int PtMasterHangup2(PLXT_ARGS Args);

int PtMasterHangup3(PLXT_ARGS Args);

int PtMasterHangup4(PLXT_ARGS Args);

int PtMoreThanOne(PLXT_ARGS Args);

int PtMultiMessageReadWrite(PLXT_ARGS Args);

int PtReadNoSub1(PLXT_ARGS Args);

int PtReadNoSub2(PLXT_ARGS Args);

int PtReadNoSub3(PLXT_ARGS Args);

int PtReadNoSub4(PLXT_ARGS Args);

int PtSessionBasic(PLXT_ARGS Args);

int PtSessionNoTerminal(PLXT_ARGS Args);

int PtStressIo(PLXT_ARGS Args);

int PtUTF8Basic(PLXT_ARGS Args);

int PtUTF8Basic2(PLXT_ARGS Args);

int PtUTF8Basic3(PLXT_ARGS Args);

int PtUTF8Basic4(PLXT_ARGS Args);

int PtUTF8Basic5(PLXT_ARGS Args);

int PtUTF8Basic6(PLXT_ARGS Args);

int PtUTF8Basic7(PLXT_ARGS Args);

int PtUTF8Basic8(PLXT_ARGS Args);

int PtUTF8Malformed(PLXT_ARGS Args);

int PtUTF8Malformed2(PLXT_ARGS Args);

int PtUTF8Malformed3(PLXT_ARGS Args);

int PtUTF8Malformed4(PLXT_ARGS Args);

int PtWindowSizeCheck(PLXT_ARGS Args);

int PtWriteNoSub1(PLXT_ARGS Args);

int PtWriteNoSub2(PLXT_ARGS Args);

int PtWriteToSubReadFromMaster1(PLXT_ARGS Args);

void TestFun(void);

//
// Global constants
//

//
// N.B. LXT_VARIATION is capped at 64 in order to support the variation mask.
//      Additional tests can be found in dev_pt2.c. This also keeps the files
//      from becoming overly large.
//

static const LXT_VARIATION g_LxtVariations[] = {
    {"PT Basic", PtBasic},
    {"PT Basic2", PtBasic2},
    {"PT Basic3", PtBasic3},
    {"PT Basic4", PtBasic4},
    {"PT Basic5", PtBasic5},
    {"Miscellaneous checks (part 1)", PtCheck1},
    {"Miscellaneous checks (part 2)", PtCheck2},
    {"Multiple open on the same subordinate ", PtCheck3},
    {"re-open subordinate and read pending data", PtCheck4},
    {"check control character behavior (part 1)", PtControlCharCheck},
    {"check control character behavior (part 2)", PtControlCharCheck2},
    {"check control character behavior (part 3)", PtControlCharCheck3},
    {"check control character behavior (part 4)", PtControlCharCheck4},
    {"check control character behavior (part 5)", PtControlCharCheck5},
    {"check control character behavior (part 6)", PtControlCharCheck6},
    {"Disassociate from a controlling terminal", PtDisassociateTty},
    {"send a message with an embedded NULL", PtEmbeddedNullReadWrite},
    {"PT Erase character handling (part 1)", PtEraseCheck},
    {"PT Erase character handling (part 2)", PtEraseCheck2},
    {"PT Erase character handling (part 3)", PtEraseCheck3},
    {"PT Erase character handling (part 4)", PtEraseCheck4},
    {"Sanity check of forkpty", PtGlibcForkPtyBasic},
    {"Open subordinate after closing master (part 1)", PtLateOpen1},
    {"Open subordinate after closing master (part 2)", PtLateOpen2},
    {"PT line-break handling (part 1)", PtLineBreakCheck},
    {"PT line-break handling (part 2)", PtLineBreakCheck2},
    {"PT line-break handling (part 3)", PtLineBreakCheck3},
    {"PT line-break handling (part 4)", PtLineBreakCheck4},
    {"PT line-break handling (part 5)", PtLineBreakCheck5},
    {"PT line-break handling (part 6)", PtLineBreakCheck6},
    {"PT line-break handling (part 7)", PtLineBreakCheck7},
    {"PT line-break handling (part 8)", PtLineBreakCheck8},
    {"PT line-break handling (part 9)", PtLineBreakCheck9},
    {"PT line-break handling (part 10)", PtLineBreakCheck10},
    {"Tests with the master buffer full", PtMasterFillBuffer},
    {"Master hangup on subordinate (part 1)", PtMasterHangup1},
    {"Master hangup on subordinate (part 2)", PtMasterHangup2},
    {"Master hangup on subordinate (part 3)", PtMasterHangup3},
    {"Master hangup on subordinate (part 4)", PtMasterHangup4},
    {">1 pseudo terminal support", PtMoreThanOne},
    {"Multimessage read/write", PtMultiMessageReadWrite},
    {"Read from master with no sub (part 1)", PtReadNoSub1},
    {"Read from master with no sub (part 2)", PtReadNoSub2},
    {"Read from master with no sub (part 3)", PtReadNoSub3},
    {"Session with basic controlling terminal IO", PtSessionBasic},
    {"Session with no controlling terminal IO", PtSessionNoTerminal},
    {"PT UTF-8 Basic", PtUTF8Basic},
    {"PT UTF-8 Basic2", PtUTF8Basic2},
    {"PT UTF-8 Basic3", PtUTF8Basic3},
    {"PT UTF-8 Basic4", PtUTF8Basic4},
    {"PT UTF-8 Basic5", PtUTF8Basic5},
    {"PT UTF-8 Basic6", PtUTF8Basic6},
    {"PT UTF-8 Basic7", PtUTF8Basic7},
    {"PT UTF-8 Basic8", PtUTF8Basic8},
    {"PT UTF-8 Malformed character handling (part 1)", PtUTF8Malformed},
    {"PT UTF-8 Malformed character handling (part 2)", PtUTF8Malformed2},
    {"PT UTF-8 Malformed character handling (part 3)", PtUTF8Malformed3},
    {"PT UTF-8 Malformed character handling (part 4)", PtUTF8Malformed4},
    {"Window size handling check", PtWindowSizeCheck},
    {"Write on master with no sub (part 1)", PtWriteNoSub1},
    {"Write on master with no sub (part 2)", PtWriteNoSub2},
    {"Write to sub, read from master (part 1)", PtWriteToSubReadFromMaster1},
    //{ "I/O stress test", PtStressIo }
    {"Line discipline", PtLineDiscipline}};

int DevPtTestEntry(int Argc, char* Argv[])

/*++

Routine Description:

    This routine main entry point for the test for dup, dup2 system call.

Arguments:

    Argc - Supplies the number of command line arguments.

    Argv - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    LXT_ARGS Args;
    int Result;

    LxtCheckErrno(LxtInitialize(Argc, Argv, &Args, LXT_NAME));

    LXT_SYNCHRONIZATION_POINT_INIT();

    //
    // Query the pseudo terminal buffer size before running any test cases.
    //

    LxtCheckErrno(LxtRunVariations(&Args, g_LxtVariations, LXT_COUNT_OF(g_LxtVariations)));

    // TestFun();

ErrorExit:
    LXT_SYNCHRONIZATION_POINT_DESTROY();
    LxtUninitialize();
    return !LXT_SUCCESS(Result);
}

void* PerformIoStressThread(void* Config)

/*++

Routine Description:

    This routine performs IO Stress test on the given PT as
    per the configuration specified by the argument.


Arguments:

    Config - Supplies the stress related configuration argument.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{
    StressThreadArg* IoDetails;
    int LoopItr;
    int Result;

    IoDetails = (StressThreadArg*)Config;

    //
    // Lock/unlock the mutex before proceeding. This mutex signifies
    // the start of the race.
    //

    pthread_mutex_lock(&DevPtStressMutex);
    pthread_mutex_unlock(&DevPtStressMutex);

    for (LoopItr = 0; LoopItr < IoDetails->LoopCount; LoopItr++)
    {
        LxtCheckErrno(SimpleReadWriteCheck(IoDetails->PtmFd, IoDetails->PtsFd));
    }

ErrorExit:
    pthread_exit(0);
    return NULL;
}

int PtBasic(PLXT_ARGS Args)

/*++

Routine Description:

    This routine performs a very basic check for pseudo terminal. The steps are:
    - Open the master.
    - Open the subordinate.
    - Turns off canonical mode to avoid line discipline.
    - Perform simple read/write check on the master-subordinate.


Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int PtmFd;
    int PtsFd;
    fd_set ReadFds;
    int Result;
    int SerialNumber;
    struct timeval Timeout;
    fd_set WriteFds;

    //
    // Initialize locals
    //

    PtmFd = -1;
    PtsFd = -1;

    //
    // Open Master-Subordinate
    //

    LxtCheckErrno(OpenMasterSubordinate(&PtmFd, &PtsFd, NULL, &SerialNumber));
    LxtLogInfo("Master opened at FD:%d", PtmFd);
    LxtLogInfo("Subordinate Serial Number: %d", SerialNumber);
    LxtLogInfo("Subordinate opened at FD:%d", PtsFd);
    LxtCheckErrno(RawInit(PtsFd));

    //
    // Verify the starting notification state of both endpoints.
    //

    memset(&Timeout, 0, sizeof(Timeout));
    Timeout.tv_sec = 0;
    FD_ZERO(&ReadFds);
    FD_SET(PtmFd, &ReadFds);
    FD_SET(PtsFd, &ReadFds);
    LxtCheckErrno(select((max(PtmFd, PtsFd) + 1), &ReadFds, NULL, NULL, &Timeout));
    LxtCheckEqual(Result, 0, "%d");
    FD_ZERO(&WriteFds);
    FD_SET(PtmFd, &WriteFds);
    FD_SET(PtsFd, &WriteFds);
    LxtCheckErrno(select((max(PtmFd, PtsFd) + 1), NULL, &WriteFds, NULL, &Timeout));
    LxtCheckEqual(Result, 2, "%d");

    //
    // Perform IO.
    //

    LxtCheckErrno(SimpleReadWriteCheck(PtmFd, PtsFd));

ErrorExit:
    if (PtmFd != -1)
    {
        close(PtmFd);
    }

    if (PtsFd != -1)
    {
        close(PtsFd);
    }

    return Result;
}

int PtBasic2(PLXT_ARGS Args)

/*++

Routine Description:

    This routine performs a very basic check for pseudo terminal. The steps are:
    - Open the master.
    - Open the subordinate.
    - Perform simple read/write check on the master-subordinate.


Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int PtmFd;
    int PtsFd;
    fd_set ReadFds;
    int Result;
    int SerialNumber;
    struct timeval Timeout;
    fd_set WriteFds;

    //
    // Initialize locals
    //

    PtmFd = -1;
    PtsFd = -1;

    //
    // Open Master-Subordinate
    //

    LxtCheckErrno(OpenMasterSubordinate(&PtmFd, &PtsFd, NULL, &SerialNumber));
    LxtLogInfo("Master opened at FD:%d", PtmFd);
    LxtLogInfo("Subordinate Serial Number: %d", SerialNumber);
    LxtLogInfo("Subordinate opened at FD:%d", PtsFd);

    //
    // Verify the starting notification state of both endpoints.
    //

    memset(&Timeout, 0, sizeof(Timeout));
    Timeout.tv_sec = 0;
    FD_ZERO(&ReadFds);
    FD_SET(PtmFd, &ReadFds);
    FD_SET(PtsFd, &ReadFds);
    LxtCheckErrno(select((max(PtmFd, PtsFd) + 1), &ReadFds, NULL, NULL, &Timeout));
    LxtCheckEqual(Result, 0, "%d");
    FD_ZERO(&WriteFds);
    FD_SET(PtmFd, &WriteFds);
    FD_SET(PtsFd, &WriteFds);
    LxtCheckErrno(select((max(PtmFd, PtsFd) + 1), NULL, &WriteFds, NULL, &Timeout));
    LxtCheckEqual(Result, 2, "%d");

    //
    // Perform IO.
    //

    LxtCheckErrno(SimpleReadWriteCheck(PtmFd, PtsFd));

ErrorExit:
    if (PtmFd != -1)
    {
        close(PtmFd);
    }

    if (PtsFd != -1)
    {
        close(PtsFd);
    }

    return Result;
}

int PtBasic3(PLXT_ARGS Args)

/*++

Routine Description:

    This routine performs a very basic check for pseudo terminal. The steps are:
    - Open the master.
    - Open the subordinate.
    - Turns off ICRNL to verify termios applies only to subordinate.
    - Perform simple read/write check on the master-subordinate.


Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int BytesReadWrite;
    int ExpectedResult;
    const char* GreetingsCR = "Hi there!!\r";
    const char* GreetingsNL = "Hi there!!\n";
    int PtmFd;
    int PtsFlags;
    int PtsFd;
    char ReadBuffer[1024];
    const char* ReplyCR = "Hi, how are you?\r";
    const char* ReplyNL = "Hi, how are you?\n";
    int Result;
    int SerialNumber;
    tcflag_t TermiosFlags;

    //
    // Initialize locals
    //

    PtmFd = -1;
    PtsFd = -1;

    //
    // Open Master-Subordinate
    //

    LxtCheckErrno(OpenMasterSubordinate(&PtmFd, &PtsFd, NULL, &SerialNumber));
    LxtLogInfo("Master opened at FD:%d", PtmFd);
    LxtLogInfo("Subordinate Serial Number: %d", SerialNumber);
    LxtLogInfo("Subordinate opened at FD:%d", PtsFd);

    LxtCheckErrno(PtsFlags = fcntl(PtsFd, F_GETFL, 0));

    //
    // Turn on OCRNL and turn off ICRNL to verify termios is effecting output
    // on only the subordinate.
    //

    LxtCheckErrno(TerminalSettingsGetInputFlags(PtsFd, &TermiosFlags));
    LxtCheckErrno(TerminalSettingsSetInputFlags(PtsFd, (TermiosFlags & ~ICRNL)));
    LxtCheckErrno(TerminalSettingsGetOutputFlags(PtsFd, &TermiosFlags));
    LxtCheckErrno(TerminalSettingsSetOutputFlags(PtsFd, (TermiosFlags | OCRNL)));

    //
    // Write the greetings message to the master.
    //

    LxtLogInfo("Writing to master");
    ExpectedResult = strlen(GreetingsCR);
    LxtCheckErrno(BytesReadWrite = write(PtmFd, GreetingsCR, ExpectedResult));
    LxtCheckFnResults("write", BytesReadWrite, ExpectedResult);
    LxtLogInfo("Master(FD:%d) --> subordinate(FD:%d):%*s", PtmFd, PtsFd, ExpectedResult, GreetingsCR);

    //
    // Read from subordinate. This should block because the master does not
    // respect the termios settings and a carriage-return does not signal the
    // end of a line.
    //

    memset(ReadBuffer, 0, sizeof(ReadBuffer));
    LxtLogInfo("Reading from subordinate");
    LxtCheckErrno(fcntl(PtsFd, F_SETFL, (PtsFlags | O_NONBLOCK)));
    LxtCheckErrnoFailure(BytesReadWrite = read(PtsFd, ReadBuffer, sizeof(ReadBuffer)), EAGAIN);
    LxtLogInfo("Message not ready for subordinate(FD:%d)", PtsFd);
    LxtCheckErrno(fcntl(PtsFd, F_SETFL, PtsFlags));

    //
    // In canonical mode, even though a full line was not presented, the
    // characters should have been echoed back with the carriage-return
    // control character "^M"
    //

    LxtLogInfo("Reading echo to master");
    memset(ReadBuffer, 0, sizeof(ReadBuffer));
    LxtCheckErrno(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)));
    ReadBuffer[BytesReadWrite] = '\0';
    LxtLogInfo("Echo received by master(FD:%d):%s", PtmFd, ReadBuffer);
    LxtLogInfo("Last character = %d [\\n = %d, \\r = %d]", ReadBuffer[BytesReadWrite - 1], '\n', '\r');
    LxtCheckFnResults("read", BytesReadWrite, (ExpectedResult + 1));
    if (ReadBuffer[BytesReadWrite - 1] != 'M' || ReadBuffer[BytesReadWrite - 2] != '^')
    {
        LxtLogError("Expected ^M carriage-return to be echoed.");
        Result = -1;
        goto ErrorExit;
    }

    LxtCheckMemoryEqual(ReadBuffer, (char*)GreetingsCR, (ExpectedResult - 1));

    //
    // Now write from the subordinate and read from the master, which should
    // use termios settings.
    //

    LxtLogInfo("Subordinate(FD:%d) --> master(FD:%d):%*s", PtsFd, PtmFd, ExpectedResult, ReplyCR);

    ExpectedResult = strlen(ReplyCR);
    LxtCheckErrno(BytesReadWrite = write(PtsFd, ReplyCR, ExpectedResult));
    LxtCheckFnResults("write", BytesReadWrite, ExpectedResult);

    //
    // Read from master. This should succeed and the carriage-return should be
    // transformed to a newline by the termios settings.
    //

    LxtLogInfo("Reading from master");
    memset(ReadBuffer, 0, sizeof(ReadBuffer));
    LxtCheckErrno(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)));
    ReadBuffer[BytesReadWrite] = '\0';
    LxtLogInfo("Reply received by master(FD:%d):%s", PtmFd, ReadBuffer);
    LxtLogInfo("Last character = %d [\\n = %d, \\r = %d]", ReadBuffer[BytesReadWrite - 1], '\n', '\r');
    LxtCheckFnResults("read", BytesReadWrite, ExpectedResult);
    LxtCheckMemoryEqual(ReadBuffer, (char*)ReplyNL, BytesReadWrite);

ErrorExit:
    if (PtmFd != -1)
    {
        close(PtmFd);
    }

    if (PtsFd != -1)
    {
        close(PtsFd);
    }

    return Result;
}

int PtBasic4(PLXT_ARGS Args)

/*++

Routine Description:

    This routine performs a very basic check for pseudo terminal. The steps are:
    - Open the master.
    - Open the subordinate.
    - Modify termios on subordinate, check on master.
    - Close subordinate, check termios on master.


Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int PtmFd;
    int PtsFd;
    int Result;
    int SerialNumber;
    tcflag_t TermiosFlags;

    //
    // Initialize locals
    //

    PtmFd = -1;
    PtsFd = -1;

    //
    // Open Master-Subordinate
    //

    LxtCheckErrno(OpenMasterSubordinate(&PtmFd, &PtsFd, NULL, &SerialNumber));
    LxtLogInfo("Master opened at FD:%d", PtmFd);
    LxtLogInfo("Subordinate Serial Number: %d", SerialNumber);
    LxtLogInfo("Subordinate opened at FD:%d", PtsFd);

    //
    // Set an input flag on the subordinate and read it from the master.
    //

    LxtCheckErrno(TerminalSettingsSetInputFlags(PtsFd, INLCR));
    LxtCheckErrno(TerminalSettingsGetInputFlags(PtmFd, &TermiosFlags));
    LxtCheckEqual(TermiosFlags, INLCR, "%lu");

    //
    // Close the subordinate and check again.
    //

    close(PtsFd);
    PtsFd = -1;
    LxtCheckErrno(TerminalSettingsGetInputFlags(PtmFd, &TermiosFlags));
    LxtCheckEqual(TermiosFlags, INLCR, "%lu");

ErrorExit:
    if (PtmFd != -1)
    {
        close(PtmFd);
    }

    if (PtsFd != -1)
    {
        close(PtsFd);
    }

    return Result;
}

int PtBasic5(PLXT_ARGS Args)

/*++

Routine Description:

    This routine performs a very basic check for pseudo terminal. The steps are:
    - Open the master.
    - Open the subordinate.
    - Call ttyname on both file descriptors.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    char NameBuffer[50];
    int NameSerialNumber;
    int PtmFd;
    int PtsFd;
    int Result;
    int SerialNumber;
    tcflag_t TermiosFlags;

    //
    // Initialize locals
    //

    PtmFd = -1;
    PtsFd = -1;

    //
    // Open Master-Subordinate
    //

    LxtCheckErrno(OpenMasterSubordinate(&PtmFd, &PtsFd, NULL, &SerialNumber));
    LxtLogInfo("Master opened at FD:%d", PtmFd);
    LxtLogInfo("Subordinate Serial Number: %d", SerialNumber);
    LxtLogInfo("Subordinate opened at FD:%d", PtsFd);

    //
    // Fetch the names and compare with expected values.
    //

    LxtCheckErrno(ttyname_r(PtmFd, NameBuffer, sizeof(NameBuffer)));
    LxtCheckStringEqual(NameBuffer, "/dev/ptmx");
    LxtCheckErrno(ttyname_r(PtsFd, NameBuffer, sizeof(NameBuffer)));
    LxtCheckNotEqual(sscanf(NameBuffer, "/dev/pts/%d", &NameSerialNumber), EOF, "%d");
    LxtCheckEqual(SerialNumber, NameSerialNumber, "%d");

ErrorExit:
    if (PtmFd != -1)
    {
        close(PtmFd);
    }

    if (PtsFd != -1)
    {
        close(PtsFd);
    }

    return Result;
}

int PtSessionNoTerminal(PLXT_ARGS Args)

/*++

Routine Description:

    This routine checks PTY access from a session with no controlling terminal
    to a terminal that is also not associated with a session.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    pid_t ChildPid;
    int ChildStatus;
    pid_t ForegroundId;
    int PtmFd;
    int PtsFd;
    int Result;
    int SerialNumber;
    pid_t SessionId;
    pid_t TerminalForegroundId;
    pid_t TerminalSessionId;

    //
    // Initialize locals
    //

    ChildPid = -1;
    PtmFd = -1;
    PtsFd = -1;

    //
    // Open Master-Subordinate
    //

    LxtCheckErrno(OpenMasterSubordinate(&PtmFd, &PtsFd, NULL, &SerialNumber));
    LxtLogInfo("Master opened at FD:%d", PtmFd);
    LxtLogInfo("Subordinate Serial Number: %d", SerialNumber);
    LxtLogInfo("Subordinate opened at FD:%d", PtsFd);

    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        LxtCheckErrno(SessionId = setsid());
        ForegroundId = getpid();
        LxtCheckResult(LxtSignalInitialize());
        LxtCheckResult(LxtSignalSetupHandler(SIGTTOU, SA_SIGINFO));
        LxtCheckResult(LxtSignalSetupHandler(SIGTTIN, SA_SIGINFO));
        LxtLogInfo("Verifying access to a non-controlling terminal from a new session");
        LxtCheckErrno(RawInit(PtsFd));
        LxtCheckErrno(SimpleReadWriteCheck(PtmFd, PtsFd));
        LxtCheckResult(LxtSignalCheckNoSignal());
        LxtCheckErrnoFailure(tcgetsid(PtsFd), ENOTTY);
        LxtCheckErrnoFailure(tcgetsid(PtmFd), ENOTTY);
        LxtCheckErrnoFailure(tcgetsid(PtmFd), ENOTTY);
        LxtCheckErrnoFailure(tcgetpgrp(PtsFd), ENOTTY);

        //
        // Querying the foreground process on the master endpoint doesn't fail,
        // instead returning 0 if there is no foreground process (either
        // because the terminal is not a controlling terminal of a session or
        // the session has no foreground process).
        //

        LxtCheckErrno(tcgetpgrp(PtmFd));
        LxtCheckEqual(Result, 0, "%d");
    }
    else
    {
        LxtCheckErrno(TEMP_FAILURE_RETRY(Result = waitpid(ChildPid, &ChildStatus, 0)));
        LxtCheckResult(WIFEXITED(ChildStatus) ? 0 : -1);
        LxtCheckResult((int)(char)WEXITSTATUS(ChildStatus));
    }

ErrorExit:
    if (ChildPid == 0)
    {
        exit(Result);
    }

    return Result;
}

int PtSessionBasic(PLXT_ARGS Args)

/*++

Routine Description:

    This routine performs basic checks on endpoints made the controlling
    terminal of a new session.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    pid_t ChildPid;
    int ChildStatus;
    pid_t ForegroundId;
    pid_t SelfPid;
    int PtmFd;
    int PtsFd;
    int Result;
    pid_t SessionId;
    pid_t TerminalForegroundId;
    pid_t TerminalSessionId;

    //
    // Initialize locals
    //

    ChildPid = -1;
    PtmFd = -1;
    PtsFd = -1;

    LxtCheckErrno(ChildPid = ForkPty(&PtmFd, &PtsFd));
    if (ChildPid == 0)
    {
        LxtCheckResult(SelfPid = getpid());
        LxtCheckResult(SessionId = getsid(0));
        LxtCheckEqual(SelfPid, SessionId, "%d");
        LxtCheckResult(LxtSignalInitialize());
        LxtCheckResult(LxtSignalSetupHandler(SIGTTOU, SA_SIGINFO));
        LxtCheckResult(LxtSignalSetupHandler(SIGTTIN, SA_SIGINFO));
        LxtCheckErrno(TerminalSessionId = tcgetsid(PtsFd));
        LxtCheckEqual(SessionId, TerminalSessionId, "%d");
        LxtCheckErrno(TerminalSessionId = tcgetsid(PtmFd));
        LxtCheckEqual(SessionId, TerminalSessionId, "%d");
        LxtCheckErrno(TerminalForegroundId = tcgetpgrp(PtsFd));
        LxtCheckEqual(SelfPid, TerminalForegroundId, "%d");
        LxtCheckErrno(TerminalForegroundId = tcgetpgrp(PtmFd));
        LxtCheckEqual(SelfPid, TerminalForegroundId, "%d");
        LxtCheckErrno(RawInit(PtsFd));
        LxtCheckErrno(SimpleReadWriteCheck(PtmFd, PtsFd));
        LxtCheckResult(LxtSignalCheckNoSignal());
    }
    else
    {
        LxtCheckErrno(TEMP_FAILURE_RETRY(Result = waitpid(ChildPid, &ChildStatus, 0)));
        LxtCheckResult(WIFEXITED(ChildStatus) ? 0 : -1);
        LxtCheckResult((int)(char)WEXITSTATUS(ChildStatus));
    }

ErrorExit:
    if (PtmFd != -1)
    {
        close(PtmFd);
    }

    if (PtsFd != -1)
    {
        close(PtsFd);
    }

    if (ChildPid == 0)
    {
        exit(Result);
    }

    return Result;
}

int PtDisassociateTty(PLXT_ARGS Args)

/*++

Routine Description:

    This routine removes the controlling terminal from its process and checks
    IO.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    pid_t ChildPid;
    BOOLEAN EndChildPidSynchronization;
    pid_t ForegroundId;
    int PtmFd;
    int PtsFd;
    int Result;
    pid_t SessionId;
    int Status;
    pid_t TerminalForegroundId;
    pid_t TerminalSessionId;

    //
    // Initialize locals
    //

    ChildPid = -1;
    EndChildPidSynchronization = TRUE;
    PtmFd = -1;
    PtsFd = -1;

    LXT_SYNCHRONIZATION_POINT_START();
    LxtCheckErrno(ChildPid = ForkPty(&PtmFd, &PtsFd));
    if (ChildPid == 0)
    {
        LxtCheckResult(LxtSignalInitialize());
        LxtSignalSetAllowMultiple(TRUE);
        LxtCheckResult(LxtSignalSetupHandler(SIGTTOU, SA_SIGINFO));
        LxtCheckResult(LxtSignalSetupHandler(SIGTTIN, SA_SIGINFO));
        LxtCheckResult(LxtSignalSetupHandler(SIGHUP, SA_SIGINFO));
        LxtCheckResult(LxtSignalSetupHandler(SIGCONT, SA_SIGINFO));
        LxtCheckResult(ForegroundId = getpid());
        LxtCheckResult(SessionId = getsid(0));

        //
        // Allow the other thread to try to disassociate the terminal, and wait
        // for that to complete.
        //

        LXT_SYNCHRONIZATION_POINT();
        LXT_SYNCHRONIZATION_POINT();
        LxtCheckResult(LxtSignalCheckNoSignal());

        //
        // Check session and foreground process group for both endpoints of
        // the psuedo-terminal.
        //

        LxtCheckErrno(TerminalSessionId = tcgetsid(PtsFd));
        LxtCheckEqual(TerminalSessionId, SessionId, "%d");
        LxtCheckErrno(TerminalSessionId = tcgetsid(PtmFd));
        LxtCheckEqual(TerminalSessionId, SessionId, "%d");
        LxtCheckErrno(TerminalForegroundId = tcgetpgrp(PtsFd));
        LxtCheckEqual(TerminalForegroundId, ForegroundId, "%d");
        LxtCheckErrno(TerminalForegroundId = tcgetpgrp(PtmFd));
        LxtCheckEqual(TerminalForegroundId, ForegroundId, "%d");

        //
        // Disconnect the controlling terminal.
        //

        LxtCheckErrno(ioctl(PtsFd, TIOCNOTTY, (char*)NULL));

        //
        // TODO_LX: Support SIGCONT.
        //
        // LxtCheckResult(LxtSignalCheckReceived(SIGCONT));
        //

        LxtCheckResult(LxtSignalCheckReceived(SIGHUP));
        LxtSignalResetReceived();

        //
        // Trying to disconnect again should fail.
        //

        LxtCheckErrnoFailure(ioctl(PtsFd, TIOCNOTTY, (char*)NULL), ENOTTY);

        //
        // The terminal is no longer associated, so it is expected to fail the
        // commands to retrieve session and foreground process group.
        //

        LxtCheckErrnoFailure(tcgetsid(PtsFd), ENOTTY);
        LxtCheckErrnoFailure(tcgetpgrp(PtsFd), ENOTTY);

        //
        // On Linux, The master endpoint returns foreground/session state, but
        // instead of failing the foreground group query will just return 0.
        //

        LxtCheckErrnoFailure(tcgetsid(PtmFd), ENOTTY);
        LxtCheckErrno(TerminalForegroundId = tcgetpgrp(PtmFd));
        LxtCheckEqual(TerminalForegroundId, 0, "%d");

        //
        // Do a simple IO check.
        //

        LxtCheckErrno(RawInit(PtsFd));
        LxtCheckErrno(SimpleReadWriteCheck(PtmFd, PtsFd));
        LxtCheckResult(LxtSignalCheckNoSignal());

        //
        // Test TIOCSTI.
        //

        LxtCheckErrno(ioctl(PtsFd, TIOCSTI, "x"));
        LxtCheckErrnoFailure(ioctl(PtsFd, TIOCSTI, (char*)NULL), EFAULT);
        LxtCheckErrno(setuid(1001));
        LxtCheckErrnoFailure(ioctl(PtsFd, TIOCSTI, "x"), EPERM);
        LxtCheckErrnoFailure(ioctl(PtsFd, TIOCSTI, (char*)NULL), EPERM);
    }
    else
    {

        //
        // Try to disassociate terminal from another session.
        //

        LXT_SYNCHRONIZATION_POINT();
        LxtCheckErrnoFailure(ioctl(PtsFd, TIOCNOTTY, (char*)NULL), ENOTTY);
        LXT_SYNCHRONIZATION_POINT();

        //
        // Wait for the child here in order to run more tests after the session
        // has been destroyed.
        //

        LxtCheckErrno(TEMP_FAILURE_RETRY(Result = waitpid(ChildPid, &Status, 0)));
        EndChildPidSynchronization = FALSE;
        LxtCheckResult(WIFEXITED(Status) ? 0 : -1);
        LxtCheckResult((int)(char)WEXITSTATUS(Status));

        //
        // Check status of master endpoint after session is gone.
        //

        LxtCheckErrnoFailure(tcgetsid(PtmFd), ENOTTY);
        LxtCheckErrno(tcgetpgrp(PtmFd));
    }

ErrorExit:
    if ((ChildPid != 0) && (PtmFd != -1))
    {
        close(PtmFd);
    }

    if ((ChildPid != 0) && (PtsFd != -1))
    {
        close(PtsFd);
    }

    if (EndChildPidSynchronization != FALSE)
    {
        LXT_SYNCHRONIZATION_POINT_END();
    }

    return Result;
}

int PtGlibcForkPtyBasic(PLXT_ARGS Args)

/*++

Routine Description:

    This routine does a basic sanity test of glibc's forkpty.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int BytesReadWrite;
    pid_t ChildPid;
    int ChildStatus;
    pid_t ForegroundId;
    const char* Message1 = "Message1\n";
    const char* Message2 = "2egasseM\n";
    char MessageBuffer[10];
    size_t MessageLength;
    int PtmFd;
    char PtsBuffer[PTS_DEV_NAME_BUFFER_SIZE];
    int Result;
    pid_t SelfPid;
    pid_t SessionId;
    pid_t TerminalForegroundId;
    pid_t TerminalSessionId;

    //
    // Initialize locals
    //

    ChildPid = -1;
    PtmFd = -1;

    LxtCheckErrno(ChildPid = forkpty(&PtmFd, PtsBuffer, NULL, NULL));
    if (ChildPid == 0)
    {

        //
        // N.B. forkpty resets STDOUT/IN/ERR to the pty fd so no messages will
        //      appear to the console, but they will still be logged.
        //
        //      No information logging is allowed since it will go to STDOUT
        //      which is being tested.
        //

        LxtCheckResult(SelfPid = getpid());
        LxtCheckResult(SessionId = getsid(0));
        LxtCheckEqual(SelfPid, SessionId, "%d");
        LxtCheckResult(LxtSignalInitialize());
        LxtCheckResult(LxtSignalSetupHandler(SIGTTOU, SA_SIGINFO));
        LxtCheckResult(LxtSignalSetupHandler(SIGTTIN, SA_SIGINFO));
        LxtCheckErrno(TerminalSessionId = tcgetsid(STDOUT));
        LxtCheckEqual(SessionId, TerminalSessionId, "%d");
        LxtCheckErrno(TerminalForegroundId = tcgetpgrp(STDOUT));
        LxtCheckEqual(SelfPid, TerminalForegroundId, "%d");

        MessageLength = strlen(Message1);
        memset(MessageBuffer, 0, sizeof(MessageBuffer));
        LxtCheckErrno(BytesReadWrite = read(STDIN, MessageBuffer, MessageLength));
        LxtCheckFnResults("read", BytesReadWrite, MessageLength);
        LxtCheckStringEqual(Message1, MessageBuffer);
        LxtCheckResult(LxtSignalCheckNoSignal());

        MessageLength = strlen(Message2);
        LxtCheckErrno(BytesReadWrite = write(STDOUT, Message2, MessageLength));
        LxtCheckFnResults("write", BytesReadWrite, MessageLength);
        LxtCheckResult(LxtSignalCheckNoSignal());
    }
    else
    {
        LxtCheckResult(LxtSignalInitialize());
        LxtCheckResult(LxtSignalSetupHandler(SIGTTOU, SA_SIGINFO));
        LxtCheckResult(LxtSignalSetupHandler(SIGTTIN, SA_SIGINFO));
        LxtCheckErrno(TerminalSettingsSetInputFlags(PtmFd, 0));
        LxtCheckErrno(TerminalSettingsSetOutputFlags(PtmFd, 0));
        LxtCheckErrno(TerminalSettingsSetLocalFlags(PtmFd, (ICANON | TOSTOP)));
        MessageLength = strlen(Message1);
        LxtLogInfo("Writing '%s' to master (fd:%d)", Message1, PtmFd);
        LxtCheckErrno(BytesReadWrite = write(PtmFd, Message1, MessageLength));
        LxtCheckFnResults("write", BytesReadWrite, MessageLength);
        LxtCheckResult(LxtSignalCheckNoSignal());

        MessageLength = strlen(Message2);
        memset(MessageBuffer, 0, sizeof(MessageBuffer));
        LxtCheckErrno(BytesReadWrite = read(PtmFd, MessageBuffer, MessageLength));
        LxtCheckFnResults("read", BytesReadWrite, MessageLength);
        LxtLogInfo("Read '%s' from master", MessageBuffer);
        LxtCheckStringEqual(Message2, MessageBuffer);
        LxtCheckResult(LxtSignalCheckNoSignal());

        LxtCheckErrno(TEMP_FAILURE_RETRY(Result = waitpid(ChildPid, &ChildStatus, 0)));
        ChildPid = -1;
        LxtCheckResult(WIFEXITED(ChildStatus) ? 0 : -1);
        LxtCheckResult((int)(char)WEXITSTATUS(ChildStatus));
    }

ErrorExit:
    if (PtmFd != -1)
    {
        close(PtmFd);
    }

    if (ChildPid == 0)
    {
        exit(Result);
    }
    else if (ChildPid > 0)
    {
        kill(ChildPid, SIGKILL);
    }

    return Result;
}

int PtCheck1(PLXT_ARGS Args)

/*++

Routine Description:

    This routine validates following checks:
    1. Open a subordinate device that does not exist.
    Expected Result: The operation should fail with error: ENOENT.
    2. Open a subordinate that has not been unlocked.
    Expected Result: The operation should fail with result EIO.
    3. Open a master, get the subordinate device name, close the master
       and then open the subordinate.
    Expected Result: The operation should fail with error ENOENT.
    4. Open the master, open the subordinate, close the master and try
       opening the subordinate again.
    Expected Result: The last open operation should fail with error ENOENT.
    5. Open the master, open a subordinate, close it and then open the
       subordinate again.
       Expected Result: As long as the master is alive, one should be
       able to get a handle to the subordinate.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int PtmFd;
    char PtsDevName[PTS_DEV_NAME_BUFFER_SIZE];
    int PtsFd;
    int Result;
    int SerialNumber;

    //
    // Initialize locals
    //

    PtmFd = -1;
    PtsFd = -1;

    //
    // Check 1:
    // Choose a subordinate device that is highly unlikely to exist,
    // and open it.
    //

    strcpy(PtsDevName, "/dev/pts/100");
    LxtCheckErrnoFailure(open(PtsDevName, O_RDWR), ENOENT);

    //
    // Open the master.
    //

    LxtCheckErrno((PtmFd = open("/dev/ptmx", O_RDWR)));
    LxtLogInfo("Master opened at FD:%d", PtmFd);
    LxtCheckErrno(grantpt(PtmFd));

    //
    // Check 2:
    // Do not unlock the subordinate. Try opening the subordinate.
    // It should fail.
    //

    LxtCheckErrno(ptsname_r(PtmFd, PtsDevName, PTS_DEV_NAME_BUFFER_SIZE));
    LxtLogInfo("Subordinate Device is:%s", PtsDevName);
    PtsFd = open(PtsDevName, O_RDWR);
    LxtCheckErrnoFailure(open(PtsDevName, O_RDWR), EIO);

    //
    // Unlock the subordinate and try opening the subordinate again.
    // It should succeed this time.
    //

    LxtCheckErrno(unlockpt(PtmFd));
    LxtCheckErrno((PtsFd = open(PtsDevName, O_RDWR)));
    LxtLogInfo("Subordinate opened at FD:%d", PtsFd);

    //
    // Check 3.
    // Close the subordinate and the master and then try opening the
    // subordinate again. It should fail.
    //

    LxtClose(PtmFd);
    LxtClose(PtsFd);
    LxtCheckErrnoFailure(open(PtsDevName, O_RDWR), ENOENT);

    //
    // Check 4.
    // Open Master-Subordinate
    //

    LxtCheckErrno(OpenMasterSubordinate(&PtmFd, &PtsFd, NULL, &SerialNumber));
    LxtLogInfo("Master opened at FD:%d", PtmFd);
    LxtLogInfo("Subordinate Serial Number: %d", SerialNumber);
    LxtLogInfo("Subordinate opened at FD:%d", PtsFd);

    //
    // Subordinate is opened. Close the master.
    //

    LxtClose(PtmFd);

    //
    // Try opening the same subordinate again. It should fail.
    //

    LxtCheckErrnoFailure(open(PtsDevName, O_RDWR), ENOENT);
    LxtClose(PtmFd);
    LxtClose(PtsFd);

    //
    // Check 5.
    // Open Master-Subordinate
    //

    LxtCheckErrno(OpenMasterSubordinate(&PtmFd, &PtsFd, PtsDevName, &SerialNumber));

    LxtLogInfo("Master opened at FD:%d", PtmFd);
    LxtLogInfo("Subordinate Device is:%s", PtsDevName);
    LxtLogInfo("Subordinate Serial Number: %d", SerialNumber);
    LxtLogInfo("Subordinate opened at FD:%d", PtsFd);

    //
    // Close the subordinate and open it again.
    //

    LxtClose(PtsFd);
    LxtCheckErrno(PtsFd = open(PtsDevName, O_RDWR));
    LxtLogInfo("Subordinate opened at FD:%d", PtsFd);

ErrorExit:
    if (PtmFd != -1)
    {
        close(PtmFd);
    }

    if (PtsFd != -1)
    {
        close(PtsFd);
    }

    return Result;
}

int PtCheck2(PLXT_ARGS Args)

/*++

Routine Description:

    This routine validates that the serial number for the pseudo terminal
    does not get reused if there are still open handle(s) to the master
    or the subordinate.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int PtmFd;
    char PtsDevName[PTS_DEV_NAME_BUFFER_SIZE];
    int PtsFd;
    int Result;
    int SerialNumber1;
    int SerialNumber2;
    int SerialNumber3;

    //
    // Initialize locals
    //

    PtmFd = -1;
    PtsFd = -1;
    SerialNumber1 = -1;
    SerialNumber2 = -1;
    SerialNumber3 = -1;

    //
    // Open Master-Subordinate
    //

    LxtCheckErrno(OpenMasterSubordinate(&PtmFd, &PtsFd, PtsDevName, &SerialNumber1));

    LxtLogInfo("Master opened at FD:%d", PtmFd);
    LxtLogInfo("Subordinate Device is:%s", PtsDevName);
    LxtLogInfo("Subordinate Serial Number: %d", SerialNumber1);
    LxtLogInfo("Subordinate opened at FD:%d", PtsFd);

    //
    // Close the master, but keep the subordinate open.
    //

    LxtClose(PtmFd);

    //
    // Open a new pseudo terminal.
    //

    LxtCheckErrno((PtmFd = open("/dev/ptmx", O_RDWR)));
    LxtLogInfo("Master opened at FD:%d", PtmFd);
    LxtCheckErrno(ptsname_r(PtmFd, PtsDevName, PTS_DEV_NAME_BUFFER_SIZE));
    LxtLogInfo("Subordinate Device is:%s", PtsDevName);
    LxtCheckErrno((SerialNumber2 = GetPtSerialNumFromDeviceString(PtsDevName)));

    //
    // SerialNumber2 should not be the same as SerialNumber1 because
    // the subordinate pseudo terminal handle is still open.
    //

    if (SerialNumber1 == SerialNumber2)
    {
        LxtLogError(
            "Serial number was re-used while handle(s) to "
            "subordinate were still open. SerialNumber1 = %d, "
            "SerialNumber2 = %d",
            SerialNumber1,
            SerialNumber2);
        Result = -1;
        goto ErrorExit;
    }

    //
    // Close all handles to master and subordinate.
    //

    LxtClose(PtmFd);
    LxtClose(PtsFd);

    //
    // Open Master-Subordinate again.
    //

    LxtCheckErrno(OpenMasterSubordinate(&PtmFd, &PtsFd, PtsDevName, &SerialNumber3));

    LxtLogInfo("Master opened at FD:%d", PtmFd);
    LxtLogInfo("Subordinate Device is:%s", PtsDevName);
    LxtLogInfo("Subordinate Serial Number: %d", SerialNumber3);
    LxtLogInfo("Subordinate opened at FD:%d", PtsFd);

    //
    // SerialNumber1 should get repurposed for this pseudo terminal.
    //

    if (SerialNumber3 != SerialNumber1)
    {
        LxtLogError(
            "Serial number was not re-purposed. "
            "(SerialNumber1 = %d) != (SerialNumber3 = %d)",
            SerialNumber1,
            SerialNumber3);
        Result = -1;
        goto ErrorExit;
    }

ErrorExit:
    if (PtmFd != -1)
    {
        close(PtmFd);
    }

    if (PtsFd != -1)
    {
        close(PtsFd);
    }

    return Result;
}

int PtCheck3(PLXT_ARGS Args)

/*++

Routine Description:

    This routine validates that the pseudo terminal driver is able to
    handle multiple opens on the same subordinate device.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int PtmFd;
    char PtsDevName[PTS_DEV_NAME_BUFFER_SIZE];
    int PtsFd;
    int PtsFd1;
    int PtsFd2;
    int PtsFd3;
    int Result;
    int SerialNumber;

    //
    // Initialize locals
    //

    PtmFd = -1;
    PtsFd = -1;
    PtsFd1 = -1;
    PtsFd2 = -1;
    PtsFd3 = -1;

    //
    // Open Master-Subordinate
    //

    LxtCheckErrno(OpenMasterSubordinate(&PtmFd, &PtsFd, PtsDevName, &SerialNumber));

    LxtCheckErrno(RawInit(PtsFd));
    LxtLogInfo("Master opened at FD:%d", PtmFd);
    LxtLogInfo("Subordinate Device is:%s", PtsDevName);
    LxtLogInfo("Subordinate Serial Number: %d", SerialNumber);
    LxtLogInfo("Subordinate opened at FD:%d", PtsFd);
    LxtCheckErrno(PtsFd1 = open(PtsDevName, O_RDWR));
    LxtCheckErrno(RawInit(PtsFd1));
    LxtLogInfo("Subordinate opened again at FD:%d", PtsFd1);
    LxtCheckErrno(PtsFd2 = open(PtsDevName, O_RDWR));
    LxtCheckErrno(RawInit(PtsFd2));
    LxtLogInfo("Subordinate opened again at FD:%d", PtsFd2);
    LxtCheckErrno(PtsFd3 = open(PtsDevName, O_RDWR));
    LxtCheckErrno(RawInit(PtsFd3));
    LxtLogInfo("Subordinate opened again at FD:%d", PtsFd3);

    //
    // Do simple read\write check on each of the subordinates.
    // Master should be connected to all of them.
    //

    LxtCheckErrno(SimpleReadWriteCheck(PtmFd, PtsFd));
    LxtCheckErrno(SimpleReadWriteCheck(PtmFd, PtsFd1));
    LxtCheckErrno(SimpleReadWriteCheck(PtmFd, PtsFd2));
    LxtCheckErrno(SimpleReadWriteCheck(PtmFd, PtsFd3));

ErrorExit:
    if (PtmFd != -1)
    {
        close(PtmFd);
    }

    if (PtsFd != -1)
    {
        close(PtsFd);
    }

    if (PtsFd1 != -1)
    {
        close(PtsFd1);
    }

    if (PtsFd2 != -1)
    {
        close(PtsFd2);
    }

    if (PtsFd3 != -1)
    {
        close(PtsFd3);
    }

    return Result;
}

int PtCheck4(PLXT_ARGS Args)

/*++

Routine Description:

    This routine validates that the subordinate should be able to
    read any pending data that is written by the master even
    after closing and opening the handle to the subordinate.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int BytesReadWrite;
    int ExpectedResult;
    char Message1[] = "ls -al\n";
    char Message2[] = "date\n";
    int PtmFd;
    char PtsDevName[PTS_DEV_NAME_BUFFER_SIZE];
    int PtsFd;
    char ReadBuffer[50];
    int Result;
    int SerialNumber;

    //
    // Initialize locals
    //

    PtmFd = -1;
    PtsFd = -1;

    //
    // Open Master-Subordinate
    //

    LxtCheckErrno(OpenMasterSubordinate(&PtmFd, &PtsFd, PtsDevName, &SerialNumber));

    //
    // This is a message boundary test, do not set the subordinate for raw init.
    //

    LxtLogInfo("Master opened at FD:%d", PtmFd);
    LxtLogInfo("Subordinate Device is:%s", PtsDevName);
    LxtLogInfo("Subordinate Serial Number: %d", SerialNumber);
    LxtLogInfo("Subordinate opened at FD:%d", PtsFd);

    //
    // Send message 1 and 2 to the subordinate.
    //

    ExpectedResult = strlen(Message1);
    BytesReadWrite = write(PtmFd, Message1, ExpectedResult);
    LxtLogInfo("Message sent(%d bytes) to subordinate: \n%s", BytesReadWrite, Message1);

    LxtCheckFnResults("write", BytesReadWrite, ExpectedResult);
    ExpectedResult = strlen(Message2);
    BytesReadWrite = write(PtmFd, Message2, ExpectedResult);
    LxtLogInfo("Message sent(%d bytes) to subordinate: \n%s", BytesReadWrite, Message2);

    LxtCheckFnResults("write", BytesReadWrite, ExpectedResult);

    //
    // Read Message 1 from the subordinate.
    //

    ExpectedResult = strlen(Message1);
    LxtCheckErrno(BytesReadWrite = read(PtsFd, ReadBuffer, sizeof(ReadBuffer)));

    ReadBuffer[BytesReadWrite] = '\0';
    LxtLogInfo("Message read(%d bytes) from subordinate: \n%s", BytesReadWrite, ReadBuffer);

    LxtCheckFnResults("read", BytesReadWrite, ExpectedResult);

    //
    // Compare the messages.
    //

    ExpectedResult = strlen(Message1);
    if (memcmp(ReadBuffer, Message1, min(BytesReadWrite, ExpectedResult)) != 0)
    {

        LxtLogError(
            "Data read from subordinate does not match what was "
            "written by master.");
        Result = -1;
        goto ErrorExit;
    }

    //
    // Close and re-open the subordinate.
    //

    LxtClose(PtsFd);
    LxtLogInfo("Closing and opening subordinate.");
    LxtCheckErrno(PtsFd = open(PtsDevName, O_RDWR));
    LxtLogInfo("Subordinate opened at FD:%d", PtsFd);

    //
    // Read Message 2 from the subordinate.
    //

    ExpectedResult = strlen(Message2);
    LxtCheckErrno(BytesReadWrite = read(PtsFd, ReadBuffer, sizeof(ReadBuffer)));

    ReadBuffer[BytesReadWrite] = '\0';
    LxtLogInfo("Message read(%d bytes) from subordinate: \n%s", BytesReadWrite, ReadBuffer);

    LxtCheckFnResults("read", BytesReadWrite, ExpectedResult);

    //
    // Compare the messages.
    //

    ExpectedResult = strlen(Message2);
    if (memcmp(ReadBuffer, Message2, min(BytesReadWrite, ExpectedResult)) != 0)
    {

        LxtLogError(
            "Data read from subordinate does not match what was "
            "written by master.");
        Result = -1;
        goto ErrorExit;
    }

ErrorExit:
    if (PtmFd != -1)
    {
        close(PtmFd);
    }

    if (PtsFd != -1)
    {
        close(PtsFd);
    }

    return Result;
}

int PtControlCharCheck(PLXT_ARGS Args)

/*++

Routine Description:

    This routine checks that SIGINT is delivered with a ^C.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    char ReadBuffer[10];
    ssize_t BytesReadWrite;
    pid_t ChildPid;
    int ChildStatus;
    cc_t ControlArray[NCCS];
    int PtmFd;
    int PtsFd;
    int PtsFlags;
    int Result;

    //
    // Initialize locals
    //

    ChildPid = -1;
    PtmFd = -1;
    PtsFd = -1;

    LxtCheckErrno(ChildPid = ForkPty(&PtmFd, &PtsFd));
    if (ChildPid == 0)
    {
        LxtCheckResult(LxtSignalInitialize());
        LxtCheckResult(TerminalSettingsGetControlArray(PtmFd, ControlArray));
        LxtCheckResult(LxtSignalSetupHandler(SIGINT, SA_SIGINFO));
        LxtCheckErrno(BytesReadWrite = write(PtmFd, &ControlArray[VINTR], 1));
        LxtCheckFnResults("write", BytesReadWrite, 1);

        //
        // A SIGINT signal should be generated shortly after the control
        // character is received.
        //

        LxtSignalWait();
        LxtCheckResult(LxtSignalCheckReceived(SIGINT));
        LxtSignalResetReceived();

        //
        // The control character sequence should have been echoed back.
        //

        LxtCheckErrno(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)));
        LxtCheckFnResults("read", BytesReadWrite, 2);
        LxtCheckTrue(IS_CONTROL_CHAR_ECHO_STRING(ReadBuffer, ControlArray[VINTR]));

        //
        // There should be no character waiting at the subordinate.
        //

        LxtCheckErrno(PtsFlags = fcntl(PtsFd, F_GETFL, 0));
        LxtCheckErrno(fcntl(PtsFd, F_SETFL, (PtsFlags | O_NONBLOCK)));
        LxtCheckErrnoFailure(BytesReadWrite = read(PtsFd, ReadBuffer, sizeof(ReadBuffer)), EAGAIN);
        LxtCheckErrno(fcntl(PtsFd, F_SETFL, PtsFlags));
        Result = 0;
    }
    else
    {
        LxtCheckErrno(TEMP_FAILURE_RETRY(Result = waitpid(ChildPid, &ChildStatus, 0)));
        LxtCheckResult(WIFEXITED(ChildStatus) ? 0 : -1);
        LxtCheckResult((int)(char)WEXITSTATUS(ChildStatus));
    }

ErrorExit:
    if (PtmFd != -1)
    {
        close(PtmFd);
    }

    if (PtsFd != -1)
    {
        close(PtsFd);
    }

    if (ChildPid == 0)
    {
        exit(Result);
    }

    return Result;
}

int PtControlCharCheck2(PLXT_ARGS Args)

/*++

Routine Description:

    This routine checks that changing VINTR to TAB still delivers SIGINT.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    char ReadBuffer[10];
    ssize_t BytesReadWrite;
    pid_t ChildPid;
    int ChildStatus;
    cc_t ControlArray[NCCS];
    int PtmFd;
    int PtsFd;
    int PtsFlags;
    int Result;

    //
    // Initialize locals
    //

    ChildPid = -1;
    PtmFd = -1;
    PtsFd = -1;

    LxtCheckErrno(ChildPid = ForkPty(&PtmFd, &PtsFd));
    if (ChildPid == 0)
    {
        LxtCheckResult(LxtSignalInitialize());
        LxtCheckResult(TerminalSettingsGetControlArray(PtmFd, ControlArray));
        ControlArray[VINTR] = '\t';
        LxtCheckResult(TerminalSettingsSetControlArray(PtmFd, ControlArray));
        LxtCheckResult(LxtSignalSetupHandler(SIGINT, SA_SIGINFO));
        LxtCheckErrno(BytesReadWrite = write(PtmFd, &ControlArray[VINTR], 1));
        LxtCheckFnResults("write", BytesReadWrite, 1);

        //
        // A SIGINT signal should be generated shortly after the control
        // character is received.
        //

        LxtSignalWait();
        LxtCheckResult(LxtSignalCheckReceived(SIGINT));
        LxtSignalResetReceived();

        //
        // TAB does not get echoed as a control character.
        //

        LxtCheckErrno(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)));
        LxtCheckFnResults("read", BytesReadWrite, 1);
        LxtCheckEqual(ReadBuffer[0], '\t', "%hhd");

        //
        // There should be no character waiting at the subordinate.
        //

        LxtCheckErrno(PtsFlags = fcntl(PtsFd, F_GETFL, 0));
        LxtCheckErrno(fcntl(PtsFd, F_SETFL, (PtsFlags | O_NONBLOCK)));
        LxtCheckErrnoFailure(BytesReadWrite = read(PtsFd, ReadBuffer, sizeof(ReadBuffer)), EAGAIN);
        LxtCheckErrno(fcntl(PtsFd, F_SETFL, PtsFlags));
        Result = 0;
    }
    else
    {
        LxtCheckErrno(TEMP_FAILURE_RETRY(Result = waitpid(ChildPid, &ChildStatus, 0)));
        LxtCheckResult(WIFEXITED(ChildStatus) ? 0 : -1);
        LxtCheckResult((int)(char)WEXITSTATUS(ChildStatus));
    }

ErrorExit:
    if (PtmFd != -1)
    {
        close(PtmFd);
    }

    if (PtsFd != -1)
    {
        close(PtsFd);
    }

    if (ChildPid == 0)
    {
        exit(Result);
    }

    return Result;
}

int PtControlCharCheck3(PLXT_ARGS Args)

/*++

Routine Description:

    This routine checks that changing VINTR to the letter 'A' still delivers
    SIGINT.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    char ReadBuffer[10];
    ssize_t BytesReadWrite;
    pid_t ChildPid;
    int ChildStatus;
    cc_t ControlArray[NCCS];
    int PtmFd;
    int PtsFd;
    int PtsFlags;
    int Result;

    //
    // Initialize locals
    //

    ChildPid = -1;
    PtmFd = -1;
    PtsFd = -1;

    LxtCheckErrno(ChildPid = ForkPty(&PtmFd, &PtsFd));
    if (ChildPid == 0)
    {
        LxtCheckResult(LxtSignalInitialize());
        LxtCheckResult(TerminalSettingsGetControlArray(PtmFd, ControlArray));
        ControlArray[VINTR] = 'A';
        LxtCheckResult(TerminalSettingsSetControlArray(PtmFd, ControlArray));
        LxtCheckResult(LxtSignalSetupHandler(SIGINT, SA_SIGINFO));
        LxtCheckErrno(BytesReadWrite = write(PtmFd, &ControlArray[VINTR], 1));
        LxtCheckFnResults("write", BytesReadWrite, 1);

        //
        // A SIGINT signal should be generated shortly after the control
        // character is received.
        //

        LxtSignalWait();
        LxtCheckResult(LxtSignalCheckReceived(SIGINT));
        LxtSignalResetReceived();

        //
        // 'A' does not get echoed as a control character.
        //

        LxtCheckErrno(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)));
        LxtCheckFnResults("read", BytesReadWrite, 1);
        LxtCheckEqual(ReadBuffer[0], 'A', "%hhd");

        //
        // There should be no character waiting at the subordinate.
        //

        LxtCheckErrno(PtsFlags = fcntl(PtsFd, F_GETFL, 0));
        LxtCheckErrno(fcntl(PtsFd, F_SETFL, (PtsFlags | O_NONBLOCK)));
        LxtCheckErrnoFailure(BytesReadWrite = read(PtsFd, ReadBuffer, sizeof(ReadBuffer)), EAGAIN);
        LxtCheckErrno(fcntl(PtsFd, F_SETFL, PtsFlags));
        Result = 0;
    }
    else
    {
        LxtCheckErrno(TEMP_FAILURE_RETRY(Result = waitpid(ChildPid, &ChildStatus, 0)));
        LxtCheckResult(WIFEXITED(ChildStatus) ? 0 : -1);
        LxtCheckResult((int)(char)WEXITSTATUS(ChildStatus));
    }

ErrorExit:
    if (PtmFd != -1)
    {
        close(PtmFd);
    }

    if (PtsFd != -1)
    {
        close(PtsFd);
    }

    if (ChildPid == 0)
    {
        exit(Result);
    }

    return Result;
}

int PtControlCharCheck4(PLXT_ARGS Args)

/*++

Routine Description:

    This routine checks that control character are echoed back properly. This
    test skips control characters with special behaviors (suspend, et al.).

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    ssize_t BytesReadWrite;
    ssize_t CumulativeBytesRead;
    int Index;
    int PtmFd;
    int PtsFd;
    char ReadBuffer[50];
    const char ReadResult[] = {0, 1, 2, 5, 6, 7, 8, 9, 10, 11, 12, 10, 14, 15, 16, 20, 24, 25, 27, 29, 30, 31, 32, '\n'};
    int Result;
    const char WriteBuffer[] = {0, 1, 2, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 20, 24, 25, 27, 29, 30, 31, 32, '\n'};
    const char* WriteBufferEcho = "^@^A^B^E^F^G^H\t\r\n^K^L\r\n^N^O^P^T^X^Y^[^]^^^_ \r\n";

    //
    // Initialize locals
    //

    PtmFd = -1;
    PtsFd = -1;

    LxtCheckErrno(OpenMasterSubordinate(&PtmFd, &PtsFd, NULL, NULL));
    LxtCheckErrno(BytesReadWrite = write(PtmFd, WriteBuffer, sizeof(WriteBuffer)));
    LxtCheckFnResults("write", BytesReadWrite, sizeof(WriteBuffer));

    //
    // Check the echo result
    //

    LxtCheckErrno(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)));
    LxtCheckFnResults("read", BytesReadWrite, strlen(WriteBufferEcho));
    ReadBuffer[BytesReadWrite] = '\0';
    LxtCheckStringEqual(ReadBuffer, WriteBufferEcho);

    //
    // Check the subordinate data.
    //

    LxtCheckErrno(BytesReadWrite = read(PtsFd, ReadBuffer, sizeof(ReadBuffer)));
    LxtCheckFnResults("read", BytesReadWrite, 3);
    CumulativeBytesRead = BytesReadWrite;

    //
    // Read past EOF (0x4)
    //

    LxtCheckErrno(BytesReadWrite = read(PtsFd, &ReadBuffer[CumulativeBytesRead], (sizeof(ReadBuffer) - CumulativeBytesRead)));

    LxtCheckFnResults("read", BytesReadWrite, 6);
    CumulativeBytesRead += BytesReadWrite;

    //
    // Read past newline (0xa)
    //

    LxtCheckErrno(BytesReadWrite = read(PtsFd, &ReadBuffer[CumulativeBytesRead], (sizeof(ReadBuffer) - CumulativeBytesRead)));

    LxtCheckFnResults("read", BytesReadWrite, 3);
    CumulativeBytesRead += BytesReadWrite;

    //
    // Read past carriage-return (0xd)
    //

    LxtCheckErrno(BytesReadWrite = read(PtsFd, &ReadBuffer[CumulativeBytesRead], (sizeof(ReadBuffer) - CumulativeBytesRead)));

    CumulativeBytesRead += BytesReadWrite;
    LxtCheckFnResults("read", CumulativeBytesRead, sizeof(ReadResult));
    LxtCheckMemoryEqual(ReadBuffer, (void*)ReadResult, sizeof(ReadResult));
    Result = 0;

ErrorExit:
    if (PtmFd != -1)
    {
        close(PtmFd);
    }

    if (PtsFd != -1)
    {
        close(PtsFd);
    }

    return Result;
}

int PtControlCharCheck5(PLXT_ARGS Args)

/*++

Routine Description:

    This routine checks that VINTR flushes the buffer.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    char ReadBuffer[10];
    ssize_t BytesReadWrite;
    cc_t ControlArray[NCCS];
    ssize_t ExpectedResult;
    int PtmFd;
    int PtsFd;
    int PtsFlags;
    int Result;
    const char* WriteString = "hello\n\x3";
    const char* WriteStringEcho = "^C";

    //
    // Initialize locals
    //

    PtmFd = -1;
    PtsFd = -1;

    LxtCheckErrno(OpenMasterSubordinate(&PtmFd, &PtsFd, NULL, NULL));
    ExpectedResult = strlen(WriteString);
    LxtCheckErrno(BytesReadWrite = write(PtmFd, WriteString, ExpectedResult));
    LxtCheckFnResults("write", BytesReadWrite, ExpectedResult);

    //
    // Check the echo result
    //

    ExpectedResult = strlen(WriteStringEcho);
    LxtCheckErrno(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)));
    LxtCheckFnResults("read", BytesReadWrite, ExpectedResult);
    ReadBuffer[BytesReadWrite] = '\0';
    LxtCheckStringEqual(ReadBuffer, WriteStringEcho);

    //
    // There should be no characters waiting at the subordinate.
    //

    LxtCheckErrno(PtsFlags = fcntl(PtsFd, F_GETFL, 0));
    LxtCheckErrno(fcntl(PtsFd, F_SETFL, (PtsFlags | O_NONBLOCK)));
    LxtCheckErrnoFailure(BytesReadWrite = read(PtsFd, ReadBuffer, sizeof(ReadBuffer)), EAGAIN);
    LxtCheckErrno(fcntl(PtsFd, F_SETFL, PtsFlags));
    Result = 0;

ErrorExit:
    if (PtmFd != -1)
    {
        close(PtmFd);
    }

    if (PtsFd != -1)
    {
        close(PtsFd);
    }

    return Result;
}

int PtControlCharCheck6(PLXT_ARGS Args)

/*++

Routine Description:

    This routine checks that VINTR does not flush the buffer with NOFLSH set.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    char ReadBuffer[10];
    ssize_t BytesReadWrite;
    cc_t ControlArray[NCCS];
    ssize_t ExpectedResult;
    int PtmFd;
    int PtsFd;
    int Result;
    tcflag_t LocalFlags;
    const char* WriteString = "hello\n\x3";
    const char* WriteStringEcho = "hello\r\n^C";

    //
    // Initialize locals
    //

    PtmFd = -1;
    PtsFd = -1;

    LxtCheckErrno(OpenMasterSubordinate(&PtmFd, &PtsFd, NULL, NULL));
    LxtCheckResult(TerminalSettingsGetLocalFlags(PtsFd, &LocalFlags));
    LxtCheckResult(TerminalSettingsSetLocalFlags(PtsFd, LocalFlags | NOFLSH));
    ExpectedResult = strlen(WriteString);
    LxtCheckErrno(BytesReadWrite = write(PtmFd, WriteString, ExpectedResult));
    LxtCheckFnResults("write", BytesReadWrite, ExpectedResult);

    //
    // Check the echo result
    //

    ExpectedResult = strlen(WriteStringEcho);
    LxtCheckErrno(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)));
    LxtCheckFnResults("read", BytesReadWrite, ExpectedResult);
    ReadBuffer[BytesReadWrite] = '\0';
    LxtCheckStringEqual(ReadBuffer, WriteStringEcho);

    //
    // Check data at subordinate.
    //

    ExpectedResult = strlen(WriteString) - 1;
    LxtCheckErrno(BytesReadWrite = read(PtsFd, ReadBuffer, sizeof(ReadBuffer)));
    LxtCheckFnResults("read", BytesReadWrite, ExpectedResult);
    ReadBuffer[BytesReadWrite] = '\0';
    LxtCheckMemoryEqual(ReadBuffer, (void*)WriteString, ExpectedResult);
    Result = 0;

ErrorExit:
    if (PtmFd != -1)
    {
        close(PtmFd);
    }

    if (PtsFd != -1)
    {
        close(PtsFd);
    }

    return Result;
}

int PtEmbeddedNullReadWrite(PLXT_ARGS Args)

/*++

Routine Description:

    This routine validates embedded NULL behavior.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int BytesReadWrite;
    int ExpectedResult;
    char* EmbeddedNullMessage = "ABC\0DEF\n";
    int OldFlags;
    void* PointerResult;
    int PtmFd;
    FILE* PtmFile;
    int PtsFd;
    char ReadMessage[50] = {0};
    int Result;
    int SerialNumber;

    //
    // Initialize locals
    //

    PtmFd = -1;
    PtsFd = -1;
    PtmFile = NULL;

    //
    // Open Master-Subordinate
    //

    LxtCheckErrno(OpenMasterSubordinate(&PtmFd, &PtsFd, NULL, &SerialNumber));
    LxtCheckNullErrno(PtmFile = fdopen(PtmFd, "w"));

    //
    // This is a message boundary test, do not set the subordinate for raw init.
    //

    LxtLogInfo("Master opened at FD:%d", PtmFd);
    LxtLogInfo("Subordinate Serial Number: %d", SerialNumber);
    LxtLogInfo("Subordinate opened at FD:%d", PtsFd);

    //
    // Write string with an embedded NULL.
    //

    ExpectedResult = sizeof(EmbeddedNullMessage);
    BytesReadWrite = write(PtmFd, EmbeddedNullMessage, ExpectedResult);
    LxtLogInfo("Message sent(%d bytes) to subordinate: \n%s...", BytesReadWrite, EmbeddedNullMessage);

    LxtCheckFnResults("write", BytesReadWrite, ExpectedResult);

    //
    // Read next message.
    //

    LxtCheckErrno(BytesReadWrite = read(PtsFd, ReadMessage, sizeof(ReadMessage)));

    ReadMessage[BytesReadWrite] = '\0';
    LxtLogInfo("Message read(%d bytes) from subordinate: \n%s", BytesReadWrite, ReadMessage);

    LxtCheckFnResults("read", BytesReadWrite, ExpectedResult);

    if (memcmp(ReadMessage, EmbeddedNullMessage, min(BytesReadWrite, ExpectedResult)) != 0)
    {

        LxtLogError(
            "Data read from subordinate does not match what was "
            "written by master.");

        Result = -1;
        goto ErrorExit;
    }

ErrorExit:
    if (PtmFd != -1)
    {
        close(PtmFd);
    }

    if (PtsFd != -1)
    {
        close(PtsFd);
    }

    if (PtmFile != NULL)
    {
        fclose(PtmFile);
    }

    return Result;
}

int PtEraseCheck(PLXT_ARGS Args)

/*++

Routine Description:

    This routine writes a string, sends the delete character and checks that
    both the echo bytes and the final string match expected values.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    ssize_t BytesReadWrite;
    cc_t ControlArray[NCCS];
    char EndString[3] = {0, '\n', '\0'};
    const char* EndStringEcho = "\x8 \x8\r\n";
    ssize_t ExpectedResult;
    tcflag_t InputFlags;
    int PtmFd;
    int PtsFd;
    char ReadBuffer[10];
    int Result;
    const char* SendString = "hello\nhi";
    const char* SendStringEcho = "hello\r\nhi";
    const char* SendStringFinal = "hello\nh\n";
    int SerialNumber;

    //
    // Initialize locals
    //

    PtmFd = -1;
    PtsFd = -1;

    //
    // Open Master-Subordinate
    //

    LxtCheckErrno(OpenMasterSubordinate(&PtmFd, &PtsFd, NULL, &SerialNumber));
    LxtLogInfo("Master opened at FD:%d", PtmFd);
    LxtLogInfo("Subordinate Serial Number: %d", SerialNumber);
    LxtLogInfo("Subordinate opened at FD:%d", PtsFd);

    //
    // Fetch special characters.
    //

    LxtCheckResult(TerminalSettingsGetControlArray(PtsFd, ControlArray));
    EndString[0] = ControlArray[VERASE];

    //
    // Write non-terminated string to the master.
    //

    LxtLogInfo("Writing to master");
    ExpectedResult = strlen(SendString);
    LxtCheckErrno(BytesReadWrite = write(PtmFd, SendString, ExpectedResult));
    LxtCheckFnResults("write", BytesReadWrite, ExpectedResult);
    LxtLogInfo("Master(FD:%d) --> subordinate(FD:%d):%*s", PtmFd, PtsFd, BytesReadWrite, SendString);

    //
    // Canonical mode should echo the input back to the master.
    //

    ExpectedResult = strlen(SendStringEcho);
    LxtCheckErrno(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)));
    ReadBuffer[BytesReadWrite] = '\0';
    LxtLogInfo("Echo received by master(FD:%d):%s", PtmFd, ReadBuffer);
    LxtCheckFnResults("read", BytesReadWrite, ExpectedResult);
    if (memcmp(ReadBuffer, SendStringEcho, ExpectedResult) != 0)
    {
        LxtLogError(
            "Echo to master(FD:%d) does not match what was "
            "written.",
            PtmFd);

        Result = -1;
        goto ErrorExit;
    }

    //
    // Now send delete character followed by the newline.
    //

    LxtLogInfo("Writing to master");
    ExpectedResult = strlen(EndString);
    LxtCheckErrno(BytesReadWrite = write(PtmFd, EndString, ExpectedResult));
    LxtCheckFnResults("write", BytesReadWrite, ExpectedResult);
    LxtLogInfo("Master(FD:%d) --> subordinate(FD:%d):%*s", PtmFd, PtsFd, BytesReadWrite, EndString);

    //
    // Canonical mode should echo the input back to the master.
    //

    ExpectedResult = strlen(EndStringEcho);
    LxtCheckErrno(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)));
    ReadBuffer[BytesReadWrite] = '\0';
    LxtLogInfo("Echo received by master(FD:%d):%s", PtmFd, ReadBuffer);
    LxtCheckFnResults("read", BytesReadWrite, ExpectedResult);
    if (memcmp(ReadBuffer, EndStringEcho, ExpectedResult) != 0)
    {
        LxtLogError("Echo to master(FD:%d) does not match expected value.", PtmFd);

        Result = -1;
        goto ErrorExit;
    }

    //
    // Read the message from the subordinate.
    //

    LxtLogInfo("Reading from subordinate");
    ExpectedResult = strlen(SendStringFinal);
    LxtCheckErrno(BytesReadWrite = read(PtsFd, ReadBuffer, sizeof(ReadBuffer)));
    ExpectedResult -= BytesReadWrite;
    LxtCheckErrno(BytesReadWrite = read(PtsFd, &ReadBuffer[BytesReadWrite], (sizeof(ReadBuffer) - BytesReadWrite)));

    LxtLogInfo("Message received by subordinate(FD:%d):%s", PtsFd, ReadBuffer);
    LxtCheckFnResults("read", BytesReadWrite, ExpectedResult);

    //
    // Compare the messages.
    //

    if (memcmp(ReadBuffer, SendStringFinal, strlen(SendStringFinal)) != 0)
    {
        LxtLogError(
            "Data read from subordinate(FD:%d) does not match what was "
            "written by master(FD:%d).",
            PtsFd,
            PtmFd);

        Result = -1;
        goto ErrorExit;
    }

    Result = 0;

ErrorExit:
    if (PtmFd != -1)
    {
        close(PtmFd);
    }

    if (PtsFd != -1)
    {
        close(PtsFd);
    }

    return Result;
}

int PtEraseCheck2(PLXT_ARGS Args)

/*++

Routine Description:

    This routine switches to raw input mode and then sends an erase character
    on an empty buffer. In raw mode the erase character should not be treated
    special.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    ssize_t BytesReadWrite;
    cc_t ControlArray[NCCS];
    ssize_t ExpectedResult;
    tcflag_t InputFlags;
    int PtmFd;
    int PtsFd;
    char ReadBuffer[10];
    int Result;
    int SerialNumber;

    //
    // Initialize locals
    //

    PtmFd = -1;
    PtsFd = -1;

    //
    // Open Master-Subordinate
    //

    LxtCheckErrno(OpenMasterSubordinate(&PtmFd, &PtsFd, NULL, &SerialNumber));
    LxtLogInfo("Master opened at FD:%d", PtmFd);
    LxtLogInfo("Subordinate Serial Number: %d", SerialNumber);
    LxtLogInfo("Subordinate opened at FD:%d", PtsFd);
    LxtCheckErrno(RawInit(PtsFd));

    //
    // Fetch special characters.
    //

    LxtCheckResult(TerminalSettingsGetControlArray(PtsFd, ControlArray));

    //
    // Write the erase character to the master.
    //

    LxtLogInfo("Writing to master");
    LxtCheckErrno(BytesReadWrite = write(PtmFd, &ControlArray[VERASE], 1));
    LxtCheckFnResults("write", BytesReadWrite, 1);

    //
    // Read the message from the subordinate.
    //

    LxtLogInfo("Reading from subordinate");
    LxtCheckErrno(BytesReadWrite = read(PtsFd, ReadBuffer, sizeof(ReadBuffer)));
    LxtCheckFnResults("read", BytesReadWrite, 1);
    LxtCheckEqual(ControlArray[VERASE], ReadBuffer[0], "%%d");
    Result = 0;

ErrorExit:
    if (PtmFd != -1)
    {
        close(PtmFd);
    }

    if (PtsFd != -1)
    {
        close(PtsFd);
    }

    return Result;
}

int PtEraseCheck3(PLXT_ARGS Args)

/*++

Routine Description:

    This routine sends the erase character on an empty buffer. In canonical
    mode this should do nothing.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    ssize_t BytesReadWrite;
    cc_t ControlArray[NCCS];
    tcflag_t InputFlags;
    int PtmFd;
    int PtmFlags;
    int PtsFd;
    char ReadBuffer[10];
    int Result;
    int SerialNumber;

    //
    // Initialize locals
    //

    PtmFd = -1;
    PtsFd = -1;

    //
    // Open Master-Subordinate
    //

    LxtCheckErrno(OpenMasterSubordinate(&PtmFd, &PtsFd, NULL, &SerialNumber));
    LxtLogInfo("Master opened at FD:%d", PtmFd);
    LxtLogInfo("Subordinate Serial Number: %d", SerialNumber);
    LxtLogInfo("Subordinate opened at FD:%d", PtsFd);

    LxtCheckErrno(PtmFlags = fcntl(PtmFd, F_GETFL, 0));

    //
    // Fetch special characters.
    //

    LxtCheckResult(TerminalSettingsGetControlArray(PtsFd, ControlArray));

    //
    // Send erase character on an empty buffer.
    //

    LxtLogInfo("Writing to master");
    LxtCheckErrno(BytesReadWrite = write(PtmFd, &ControlArray[VERASE], 1));
    LxtCheckFnResults("write", BytesReadWrite, 1);

    //
    // Canonical mode should not echo anything back.
    //

    LxtCheckErrno(fcntl(PtmFd, F_SETFL, (PtmFlags | O_NONBLOCK)));
    LxtCheckErrnoFailure(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)), EAGAIN);
    LxtLogInfo("No bytes echoed(FD:%d)", PtmFd);
    LxtCheckErrno(fcntl(PtmFd, F_SETFL, PtmFlags));
    Result = 0;

ErrorExit:
    if (PtmFd != -1)
    {
        close(PtmFd);
    }

    if (PtsFd != -1)
    {
        close(PtsFd);
    }

    return Result;
}

int PtEraseCheck4(PLXT_ARGS Args)

/*++

Routine Description:

    This routine writes a string with control characters, sends delete
    characters and checks that both the echo bytes and the final string match
    expected values.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    ssize_t BytesReadWrite;
    cc_t ControlArray[NCCS];
    char EndString[] = {0, 0, '\n', '\0'};
    const char* EndStringEcho = "\x8 \x8\x8 \x8\x8 \x8\r\n";
    ssize_t ExpectedResult;
    tcflag_t InputFlags;
    int PtmFd;
    int PtsFd;
    char ReadBuffer[15];
    int Result;
    const char* SendString = "hi\x2 ";
    const char* SendStringEcho = "hi^B ";
    const char* SendStringFinal = "hi\n";
    int SerialNumber;

    //
    // Initialize locals
    //

    PtmFd = -1;
    PtsFd = -1;

    //
    // Open Master-Subordinate
    //

    LxtCheckErrno(OpenMasterSubordinate(&PtmFd, &PtsFd, NULL, &SerialNumber));
    LxtLogInfo("Master opened at FD:%d", PtmFd);
    LxtLogInfo("Subordinate Serial Number: %d", SerialNumber);
    LxtLogInfo("Subordinate opened at FD:%d", PtsFd);

    //
    // Fetch special characters.
    //

    LxtCheckResult(TerminalSettingsGetControlArray(PtsFd, ControlArray));
    EndString[0] = ControlArray[VERASE];
    EndString[1] = ControlArray[VERASE];

    //
    // Write non-terminated string to the master.
    //

    LxtLogInfo("Writing to master");
    ExpectedResult = strlen(SendString);
    LxtCheckErrno(BytesReadWrite = write(PtmFd, SendString, ExpectedResult));
    LxtCheckFnResults("write", BytesReadWrite, ExpectedResult);
    LxtLogInfo("Master(FD:%d) --> subordinate(FD:%d):%*s", PtmFd, PtsFd, BytesReadWrite, SendString);

    //
    // Canonical mode should echo the input back to the master.
    //

    ExpectedResult = strlen(SendStringEcho);
    LxtCheckErrno(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)));
    ReadBuffer[BytesReadWrite] = '\0';
    LxtLogInfo("Echo received by master(FD:%d):%s", PtmFd, ReadBuffer);
    LxtCheckFnResults("read", BytesReadWrite, ExpectedResult);
    if (memcmp(ReadBuffer, SendStringEcho, ExpectedResult) != 0)
    {
        LxtLogError(
            "Echo to master(FD:%d) does not match what was "
            "written.",
            PtmFd);

        Result = -1;
        goto ErrorExit;
    }

    //
    // Now send two delete characters followed by the newline.
    //

    LxtLogInfo("Writing to master");
    ExpectedResult = strlen(EndString);
    LxtCheckErrno(BytesReadWrite = write(PtmFd, EndString, ExpectedResult));
    LxtCheckFnResults("write", BytesReadWrite, ExpectedResult);
    LxtLogInfo("Master(FD:%d) --> subordinate(FD:%d):%*s", PtmFd, PtsFd, BytesReadWrite, EndString);

    //
    // Canonical mode should echo the input back to the master.
    //

    ExpectedResult = strlen(EndStringEcho);
    LxtCheckErrno(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)));
    ReadBuffer[BytesReadWrite] = '\0';
    LxtLogInfo("Echo received by master(FD:%d):%s", PtmFd, ReadBuffer);
    LxtCheckFnResults("read", BytesReadWrite, ExpectedResult);
    if (memcmp(ReadBuffer, EndStringEcho, ExpectedResult) != 0)
    {
        LxtLogError("Echo to master(FD:%d) does not match expected value.", PtmFd);

        Result = -1;
        goto ErrorExit;
    }

    //
    // Read the message from the subordinate.
    //

    LxtLogInfo("Reading from subordinate");
    ExpectedResult = strlen(SendStringFinal);
    LxtCheckErrno(BytesReadWrite = read(PtsFd, ReadBuffer, sizeof(ReadBuffer)));
    LxtLogInfo("Message received by subordinate(FD:%d):%s", PtsFd, ReadBuffer);
    LxtCheckFnResults("read", BytesReadWrite, ExpectedResult);

    //
    // Compare the messages.
    //

    if (memcmp(ReadBuffer, SendStringFinal, strlen(SendStringFinal)) != 0)
    {
        LxtLogError(
            "Data read from subordinate(FD:%d) does not match what was "
            "written by master(FD:%d).",
            PtsFd,
            PtmFd);

        Result = -1;
        goto ErrorExit;
    }

    Result = 0;

ErrorExit:
    if (PtmFd != -1)
    {
        close(PtmFd);
    }

    if (PtsFd != -1)
    {
        close(PtsFd);
    }

    return Result;
}

int PtLateOpen1(PLXT_ARGS Args)

/*++

Routine Description:

    This routine validates part (1) below;
    when:
    1. A subordinate is opened after the master has been closed and
    2. An open handle exists for the subordinate, master is closed
       and the subordinate is opened again.

    Expected Result: in both (1) and (2), once the master is closed,
    the open on subordinate should return with error:2 (ENOENT)

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int Result;
    int PtmFd;
    char PtsDevName[PTS_DEV_NAME_BUFFER_SIZE];
    int PtsFd;

    //
    // Initialize locals
    //

    PtmFd = -1;
    PtsFd = -1;

    //
    // Open Master.
    //

    LxtCheckErrno((PtmFd = open("/dev/ptmx", O_RDWR)));
    LxtLogInfo("Master opened at FD:%d", PtmFd);
    LxtCheckErrno(grantpt(PtmFd));
    LxtCheckErrno(unlockpt(PtmFd));
    LxtCheckErrno(ptsname_r(PtmFd, PtsDevName, PTS_DEV_NAME_BUFFER_SIZE));
    LxtLogInfo("Subordinate Device is:%s", PtsDevName);

    //
    // Close the master.
    //

    LxtClose(PtmFd);

    //
    // Open the subordinate after closing the master.
    //

    LxtCheckErrnoFailure(open(PtsDevName, O_RDWR), ENOENT);

ErrorExit:
    if (PtmFd != -1)
    {
        close(PtmFd);
    }

    if (PtsFd != -1)
    {
        close(PtsFd);
    }

    return Result;
}

int PtLateOpen2(PLXT_ARGS Args)

/*++

Routine Description:

    This routine validates part (2) of the behavior as described in PtLateOpen1.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int Result;
    int PtmFd;
    char PtsDevName[PTS_DEV_NAME_BUFFER_SIZE];
    int PtsFd;
    int SerialNumber;

    //
    // Initialize locals
    //

    PtmFd = -1;
    PtsFd = -1;

    //
    // Open Master-Subordinate
    //

    LxtCheckErrno(OpenMasterSubordinate(&PtmFd, &PtsFd, PtsDevName, &SerialNumber));

    LxtLogInfo("Master opened at FD:%d", PtmFd);
    LxtLogInfo("Subordinate Device is:%s", PtsDevName);
    LxtLogInfo("Subordinate Serial Number: %d", SerialNumber);
    LxtLogInfo("Subordinate opened at FD:%d", PtsFd);

    //
    // Close the master.
    //

    LxtClose(PtmFd);

    //
    // Master is closed, try to open subordinate again.
    //

    LxtCheckErrnoFailure(open(PtsDevName, O_RDWR), ENOENT);

ErrorExit:
    if (PtmFd != -1)
    {
        close(PtmFd);
    }

    if (PtsFd != -1)
    {
        close(PtsFd);
    }

    return Result;
}

int PtLineDiscipline(PLXT_ARGS Args)

/*++

Description:

    This routine tests replacing LF with CRLF sequences.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    char Buffer[64];
    ssize_t BytesRead;
    ssize_t BytesWritten;
    const char* Message = "This\nis\na\ntest";
    const char* ExpectedMessage = "This\r\nis\r\na\r\ntest";
    size_t ExpectedSize;
    int PtmFd;
    int PtsFd;
    int Result;
    int SerialNumber;

    //
    // Initialize locals
    //

    PtmFd = -1;
    PtsFd = -1;

    //
    // Open Master-Subordinate
    //

    LxtCheckErrno(OpenMasterSubordinate(&PtmFd, &PtsFd, NULL, &SerialNumber));
    LxtLogInfo("Master opened at FD:%d", PtmFd);
    LxtLogInfo("Subordinate Serial Number: %d", SerialNumber);
    LxtLogInfo("Subordinate opened at FD:%d", PtsFd);

    //
    // Write a message with new lines to the subordinate.
    //

    ExpectedSize = strlen(Message);
    LxtCheckErrno(BytesWritten = write(PtsFd, Message, ExpectedSize));
    LxtCheckEqual((size_t)BytesWritten, ExpectedSize, "%ld");

    //
    // Read the message from the master.
    //

    ExpectedSize = strlen(ExpectedMessage);
    LxtCheckErrno(BytesRead = read(PtmFd, Buffer, sizeof(Buffer) - 1));
    LxtCheckEqual((size_t)BytesRead, ExpectedSize, "%ld");
    Buffer[BytesRead] = '\0';
    LxtCheckStringEqual(ExpectedMessage, Buffer);

ErrorExit:
    if (PtmFd != -1)
    {
        close(PtmFd);
    }

    if (PtsFd != -1)
    {
        close(PtsFd);
    }

    return Result;
}

int PtLineBreakCheck(PLXT_ARGS Args)

/*++

Routine Description:

    This routine writes VEOF to an empty buffer and checks the results.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int BytesReadWrite;
    cc_t ControlArray[NCCS];
    int PtmFd;
    int PtsFd;
    char ReadBuffer[10];
    int Result;

    //
    // Initialize locals
    //

    PtmFd = -1;
    PtsFd = -1;

    //
    // Open Master-Subordinate
    //

    LxtCheckErrno(OpenMasterSubordinate(&PtmFd, &PtsFd, NULL, NULL));
    LxtLogInfo("Master opened at FD:%d", PtmFd);
    LxtLogInfo("Subordinate opened at FD:%d", PtsFd);

    //
    // Fetch special characters.
    //

    LxtCheckResult(TerminalSettingsGetControlArray(PtsFd, ControlArray));

    //
    // Write VEOF to the master.
    //

    LxtLogInfo("Writing to master");
    LxtCheckErrno(BytesReadWrite = write(PtmFd, &ControlArray[VEOF], 1));
    LxtCheckFnResults("write", BytesReadWrite, 1);

    //
    // Nothing is expected to be echoed.
    //

    LxtCheckErrno(fcntl(PtmFd, F_SETFL, O_NONBLOCK));
    LxtCheckErrnoFailure(read(PtmFd, ReadBuffer, sizeof(ReadBuffer)), EAGAIN);

    //
    // Check subordinate data.
    //

    LxtCheckErrno(BytesReadWrite = read(PtsFd, ReadBuffer, sizeof(ReadBuffer)));
    LxtCheckFnResults("read", BytesReadWrite, 0);

    //
    // No subordinate data should be left.
    //

    LxtCheckErrno(fcntl(PtsFd, F_SETFL, O_NONBLOCK));
    LxtCheckErrnoFailure(read(PtsFd, ReadBuffer, sizeof(ReadBuffer)), EAGAIN);

ErrorExit:
    if (PtmFd != -1)
    {
        close(PtmFd);
    }

    if (PtsFd != -1)
    {
        close(PtsFd);
    }

    return Result;
}

int PtLineBreakCheck2(PLXT_ARGS Args)

/*++

Routine Description:

    This routine sets an EOL character and echoes it to an empty buffer,
    verifying the expected results..

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int BytesReadWrite;
    cc_t ControlArray[NCCS];
    const char* EchoResult = "^E";
    int ExpectedResult;
    int PtmFd;
    int PtsFd;
    char ReadBuffer[10];
    int Result;

    //
    // Initialize locals
    //

    PtmFd = -1;
    PtsFd = -1;

    //
    // Open Master-Subordinate
    //

    LxtCheckErrno(OpenMasterSubordinate(&PtmFd, &PtsFd, NULL, NULL));
    LxtLogInfo("Master opened at FD:%d", PtmFd);
    LxtLogInfo("Subordinate opened at FD:%d", PtsFd);

    //
    // Set VEOL
    //

    LxtCheckResult(TerminalSettingsGetControlArray(PtsFd, ControlArray));
    ControlArray[VEOL] = 5;
    LxtCheckResult(TerminalSettingsSetControlArray(PtsFd, ControlArray));

    //
    // Write VEOL to the master.
    //

    LxtLogInfo("Writing to master");
    LxtCheckErrno(BytesReadWrite = write(PtmFd, &ControlArray[VEOL], 1));
    LxtCheckFnResults("write", BytesReadWrite, 1);

    //
    // Check echo result.
    //

    ExpectedResult = strlen(EchoResult);
    LxtCheckErrno(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)));
    LxtCheckFnResults("read", BytesReadWrite, ExpectedResult);
    ReadBuffer[BytesReadWrite] = '\0';
    LxtCheckStringEqual(ReadBuffer, EchoResult);

    //
    // Check subordinate data.
    //

    LxtCheckErrno(BytesReadWrite = read(PtsFd, ReadBuffer, sizeof(ReadBuffer)));
    LxtCheckFnResults("read", BytesReadWrite, 1);
    LxtCheckEqual(ReadBuffer[0], ControlArray[VEOL], "%hhd");

ErrorExit:
    if (PtmFd != -1)
    {
        close(PtmFd);
    }

    if (PtsFd != -1)
    {
        close(PtsFd);
    }

    return Result;
}

int PtLineBreakCheck3(PLXT_ARGS Args)

/*++

Routine Description:

    This routine sets an EOL2 character and echoes it to an empty buffer,
    verifying the expected results..

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int BytesReadWrite;
    cc_t ControlArray[NCCS];
    const char* EchoResult = "^E";
    int ExpectedResult;
    int PtmFd;
    int PtsFd;
    char ReadBuffer[10];
    int Result;

    //
    // Initialize locals
    //

    PtmFd = -1;
    PtsFd = -1;

    //
    // Open Master-Subordinate
    //

    LxtCheckErrno(OpenMasterSubordinate(&PtmFd, &PtsFd, NULL, NULL));
    LxtLogInfo("Master opened at FD:%d", PtmFd);
    LxtLogInfo("Subordinate opened at FD:%d", PtsFd);

    //
    // Set VEOL2
    //

    LxtCheckResult(TerminalSettingsGetControlArray(PtsFd, ControlArray));
    ControlArray[VEOL2] = 5;
    LxtCheckResult(TerminalSettingsSetControlArray(PtsFd, ControlArray));

    //
    // Write VEOL to the master.
    //

    LxtLogInfo("Writing to master");
    LxtCheckErrno(BytesReadWrite = write(PtmFd, &ControlArray[VEOL2], 1));
    LxtCheckFnResults("write", BytesReadWrite, 1);

    //
    // Check echo result.
    //

    ExpectedResult = strlen(EchoResult);
    LxtCheckErrno(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)));
    LxtCheckFnResults("read", BytesReadWrite, ExpectedResult);
    ReadBuffer[BytesReadWrite] = '\0';
    LxtCheckStringEqual(ReadBuffer, EchoResult);

    //
    // Check subordinate data.
    //

    LxtCheckErrno(BytesReadWrite = read(PtsFd, ReadBuffer, sizeof(ReadBuffer)));
    LxtCheckFnResults("read", BytesReadWrite, 1);
    LxtCheckEqual(ReadBuffer[0], ControlArray[VEOL2], "%hhd");

ErrorExit:
    if (PtmFd != -1)
    {
        close(PtmFd);
    }

    if (PtsFd != -1)
    {
        close(PtsFd);
    }

    return Result;
}

int PtLineBreakCheck4(PLXT_ARGS Args)

/*++

Routine Description:

    This routine set the VEOL character and sends a string with an embedded
    VEOL, checking the results.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int BytesReadWrite;
    cc_t ControlArray[NCCS];
    int ExpectedResult;
    int PtmFd;
    int PtsFd;
    char ReadBuffer[10];
    int Result;
    const char* WriteValue =
        "hi\x5"
        "bye\n";
    const char* WriteValueEcho = "hi^Ebye\r\n";
    const char* WriteValueRead1 = "hi\x5";
    const char* WriteValueRead2 = "bye\n";

    //
    // Initialize locals
    //

    PtmFd = -1;
    PtsFd = -1;

    //
    // Open Master-Subordinate
    //

    LxtCheckErrno(OpenMasterSubordinate(&PtmFd, &PtsFd, NULL, NULL));
    LxtLogInfo("Master opened at FD:%d", PtmFd);
    LxtLogInfo("Subordinate opened at FD:%d", PtsFd);

    //
    // Set VEOL
    //

    LxtCheckResult(TerminalSettingsGetControlArray(PtsFd, ControlArray));
    ControlArray[VEOL] = 5;
    LxtCheckResult(TerminalSettingsSetControlArray(PtsFd, ControlArray));

    //
    // Write string with embedded VEOL to the master.
    //

    ExpectedResult = strlen(WriteValue);
    LxtLogInfo("Writing to master");
    LxtCheckErrno(BytesReadWrite = write(PtmFd, WriteValue, ExpectedResult));
    LxtCheckFnResults("write", BytesReadWrite, ExpectedResult);

    //
    // Check echo result.
    //

    ExpectedResult = strlen(WriteValueEcho);
    LxtCheckErrno(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)));
    LxtCheckFnResults("read", BytesReadWrite, ExpectedResult);
    ReadBuffer[BytesReadWrite] = '\0';
    LxtCheckStringEqual(ReadBuffer, WriteValueEcho);

    //
    // Check subordinate data. It should be returned as two strings.
    //

    ExpectedResult = strlen(WriteValueRead1);
    LxtCheckErrno(BytesReadWrite = read(PtsFd, ReadBuffer, sizeof(ReadBuffer)));
    LxtCheckFnResults("read", BytesReadWrite, ExpectedResult);
    ReadBuffer[ExpectedResult] = '\0';
    LxtCheckStringEqual(ReadBuffer, WriteValueRead1);

    ExpectedResult = strlen(WriteValueRead2);
    LxtCheckErrno(BytesReadWrite = read(PtsFd, ReadBuffer, sizeof(ReadBuffer)));
    LxtCheckFnResults("read", BytesReadWrite, ExpectedResult);
    ReadBuffer[ExpectedResult] = '\0';
    LxtCheckStringEqual(ReadBuffer, WriteValueRead2);

ErrorExit:
    if (PtmFd != -1)
    {
        close(PtmFd);
    }

    if (PtsFd != -1)
    {
        close(PtsFd);
    }

    return Result;
}

int PtLineBreakCheck5(PLXT_ARGS Args)

/*++

Routine Description:

    This routine sends a string with an embedded VEOF, checking the results.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int BytesReadWrite;
    cc_t ControlArray[NCCS];
    int ExpectedResult;
    int PtmFd;
    int PtsFd;
    char ReadBuffer[10];
    fd_set ReadFds;
    int Result;
    struct timeval Timeout;
    char WriteBuffer[] = {'h', 'i', 0, 'b', 'y', 'e', '\n'};
    const char* WriteValueEcho = "hibye\r\n";
    const char* WriteValueRead1 = "hi";
    const char* WriteValueRead2 = "bye\n";

    //
    // Initialize locals
    //

    PtmFd = -1;
    PtsFd = -1;

    //
    // Open Master-Subordinate
    //

    LxtCheckErrno(OpenMasterSubordinate(&PtmFd, &PtsFd, NULL, NULL));
    LxtLogInfo("Master opened at FD:%d", PtmFd);
    LxtLogInfo("Subordinate opened at FD:%d", PtsFd);

    //
    // Add VEOF character
    //

    LxtCheckResult(TerminalSettingsGetControlArray(PtsFd, ControlArray));
    WriteBuffer[2] = ControlArray[VEOF];

    //
    // Write string with embedded VEOL to the master.
    //

    ExpectedResult = sizeof(WriteBuffer);
    LxtLogInfo("Writing to master");
    LxtCheckErrno(BytesReadWrite = write(PtmFd, WriteBuffer, ExpectedResult));
    LxtCheckFnResults("write", BytesReadWrite, ExpectedResult);

    //
    // Check echo result.
    //

    ExpectedResult = strlen(WriteValueEcho);
    memset(&Timeout, 0, sizeof(Timeout));
    Timeout.tv_sec = 1;
    FD_ZERO(&ReadFds);
    FD_SET(PtmFd, &ReadFds);
    LxtCheckErrno(select((PtmFd + 1), &ReadFds, NULL, NULL, &Timeout));
    LxtCheckEqual(Result, 1, "%d");
    LxtCheckErrno(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)));
    LxtCheckFnResults("read", BytesReadWrite, ExpectedResult);
    ReadBuffer[BytesReadWrite] = '\0';
    LxtCheckStringEqual(ReadBuffer, WriteValueEcho);

    //
    // Check subordinate data. It should be returned as two strings.
    //

    ExpectedResult = strlen(WriteValueRead1);
    LxtCheckErrno(BytesReadWrite = read(PtsFd, ReadBuffer, sizeof(ReadBuffer)));
    LxtCheckFnResults("read", BytesReadWrite, ExpectedResult);
    ReadBuffer[ExpectedResult] = '\0';
    LxtCheckStringEqual(ReadBuffer, WriteValueRead1);

    ExpectedResult = strlen(WriteValueRead2);
    LxtCheckErrno(BytesReadWrite = read(PtsFd, ReadBuffer, sizeof(ReadBuffer)));
    LxtCheckFnResults("read", BytesReadWrite, ExpectedResult);
    ReadBuffer[ExpectedResult] = '\0';
    LxtCheckStringEqual(ReadBuffer, WriteValueRead2);

ErrorExit:
    if (PtmFd != -1)
    {
        close(PtmFd);
    }

    if (PtsFd != -1)
    {
        close(PtsFd);
    }

    return Result;
}

int PtLineBreakCheck6(PLXT_ARGS Args)

/*++

Routine Description:

    This routine writes VEOF to an empty buffer, switches to non-canonical mode
    and checks the results.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int BytesReadWrite;
    cc_t ControlArray[NCCS];
    int PtmFd;
    int PtsFd;
    char ReadBuffer[10];
    int Result;

    //
    // Initialize locals
    //

    PtmFd = -1;
    PtsFd = -1;

    //
    // Open Master-Subordinate
    //

    LxtCheckErrno(OpenMasterSubordinate(&PtmFd, &PtsFd, NULL, NULL));
    LxtLogInfo("Master opened at FD:%d", PtmFd);
    LxtLogInfo("Subordinate opened at FD:%d", PtsFd);

    //
    // Fetch special characters.
    //

    LxtCheckResult(TerminalSettingsGetControlArray(PtsFd, ControlArray));

    //
    // Write VEOF to the master.
    //

    LxtLogInfo("Writing to master: %hhd", ControlArray[VEOF]);
    LxtCheckErrno(BytesReadWrite = write(PtmFd, &ControlArray[VEOF], 1));
    LxtCheckFnResults("write", BytesReadWrite, 1);

    //
    // On Ubuntu16 pty processing is asynchronous so sleep for a second to make
    // sure the VEOF character is processed before switching to raw mode.
    //

    sleep(1);

    //
    // Turn off canonical mode.
    //

    LxtCheckErrno(RawInit(PtsFd));

    //
    // Nothing is expected to be echoed.
    //

    LxtCheckErrno(fcntl(PtmFd, F_SETFL, O_NONBLOCK));
    LxtCheckErrnoFailure(read(PtmFd, ReadBuffer, sizeof(ReadBuffer)), EAGAIN);

    //
    // Check subordinate data.
    //

    LxtCheckErrno(BytesReadWrite = read(PtsFd, ReadBuffer, sizeof(ReadBuffer)));
    LxtCheckFnResults("read", BytesReadWrite, 1);
    LxtCheckEqual(ReadBuffer[0], 0, "%hhd");

ErrorExit:
    if (PtmFd != -1)
    {
        close(PtmFd);
    }

    if (PtsFd != -1)
    {
        close(PtsFd);
    }

    return Result;
}

int PtLineBreakCheck7(PLXT_ARGS Args)

/*++

Routine Description:

    This routine writes string with VEOF characters in non-canonical mode and
    then reads it back in canonical mode.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int BytesReadWrite;
    cc_t ControlArray[NCCS];
    tcflag_t ControlFlags;
    int ExpectedResult;
    tcflag_t InputFlags;
    tcflag_t LocalFlags;
    tcflag_t OutputFlags;
    int PtmFd;
    int PtsFd;
    char ReadBuffer[10];
    int Result;
    char WriteBuffer[] = {'h', 'i', 0, 'b', 'y', 'e'};

    //
    // Initialize locals
    //

    PtmFd = -1;
    PtsFd = -1;

    //
    // Open Master-Subordinate
    //

    LxtCheckErrno(OpenMasterSubordinate(&PtmFd, &PtsFd, NULL, NULL));
    LxtLogInfo("Master opened at FD:%d", PtmFd);
    LxtLogInfo("Subordinate opened at FD:%d", PtsFd);

    //
    // Capture termios settings.
    //

    LxtCheckResult(TerminalSettingsGet(PtsFd, ControlArray, &ControlFlags, &InputFlags, &LocalFlags, &OutputFlags));

    WriteBuffer[2] = ControlArray[VEOF];

    //
    // Switch to non-canonical mode.
    //

    LxtCheckErrno(RawInit(PtsFd));

    //
    // Write string with embedded VEOF to the master.
    //

    ExpectedResult = sizeof(WriteBuffer);
    LxtLogInfo("Writing to master");
    LxtCheckErrno(BytesReadWrite = write(PtmFd, WriteBuffer, ExpectedResult));
    LxtCheckFnResults("write", BytesReadWrite, ExpectedResult);

    //
    // On Ubuntu16 pty processing is done asynchronously so wait a second to
    // give the character time to be processed before turning off canonical
    // mode.
    //

    sleep(1);

    //
    // No echo expected in non-canonical mode.
    //

    LxtCheckErrno(fcntl(PtmFd, F_SETFL, O_NONBLOCK));
    LxtCheckErrnoFailure(read(PtmFd, ReadBuffer, sizeof(ReadBuffer)), EAGAIN);

    //
    // Restore termios settings.
    //

    LxtCheckResult(TerminalSettingsSet(PtsFd, ControlArray, ControlFlags, InputFlags, LocalFlags, OutputFlags));

    //
    // Check subordinate data.
    //

    ExpectedResult = sizeof(WriteBuffer);
    LxtCheckErrno(BytesReadWrite = read(PtsFd, ReadBuffer, sizeof(ReadBuffer)));
    LxtCheckFnResults("read", BytesReadWrite, ExpectedResult);
    LxtCheckMemoryEqual(ReadBuffer, WriteBuffer, ExpectedResult);

ErrorExit:
    if (PtmFd != -1)
    {
        close(PtmFd);
    }

    if (PtsFd != -1)
    {
        close(PtsFd);
    }

    return Result;
}

int PtLineBreakCheck8(PLXT_ARGS Args)

/*++

Routine Description:

    This routine writes a string ending with a VEOF character.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int BytesReadWrite;
    cc_t ControlArray[NCCS];
    int ExpectedResult;
    int PtmFd;
    int PtsFd;
    char ReadBuffer[10];
    int Result;
    char WriteBuffer[] = {'a', 0};
    const char* WriteEcho = "a";

    //
    // Initialize locals
    //

    PtmFd = -1;
    PtsFd = -1;

    //
    // Open Master-Subordinate
    //

    LxtCheckErrno(OpenMasterSubordinate(&PtmFd, &PtsFd, NULL, NULL));
    LxtLogInfo("Master opened at FD:%d", PtmFd);
    LxtLogInfo("Subordinate opened at FD:%d", PtsFd);

    //
    // Get control characters.
    //

    LxtCheckResult(TerminalSettingsGetControlArray(PtsFd, ControlArray));
    WriteBuffer[1] = ControlArray[VEOF];

    //
    // Write string to the master.
    //

    ExpectedResult = sizeof(WriteBuffer);
    LxtLogInfo("Writing to master");
    LxtCheckErrno(BytesReadWrite = write(PtmFd, WriteBuffer, ExpectedResult));
    LxtCheckFnResults("write", BytesReadWrite, ExpectedResult);

    //
    // Check echo.
    //

    ExpectedResult = strlen(WriteEcho);
    LxtCheckErrno(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)));
    LxtCheckFnResults("read", BytesReadWrite, ExpectedResult);
    ReadBuffer[BytesReadWrite] = '\0';
    LxtCheckStringEqual(ReadBuffer, WriteEcho);

    //
    // Check subordinate data.
    //

    LxtCheckErrno(BytesReadWrite = read(PtsFd, ReadBuffer, 1));
    LxtCheckFnResults("read", BytesReadWrite, 1);
    LxtCheckEqual(ReadBuffer[0], WriteBuffer[0], "%hhd");

    //
    // Wrote EOF byte, but it should have been consumed with the last character
    // of the line.
    //

    LxtCheckErrno(fcntl(PtsFd, F_SETFL, O_NONBLOCK));
    LxtCheckErrnoFailure(read(PtsFd, ReadBuffer, sizeof(ReadBuffer)), EAGAIN);

ErrorExit:
    if (PtmFd != -1)
    {
        close(PtmFd);
    }

    if (PtsFd != -1)
    {
        close(PtsFd);
    }

    return Result;
}

int PtLineBreakCheck9(PLXT_ARGS Args)

/*++

Routine Description:

    This routine writes a non-terminated string in canonical mode, then
    switches to raw and back without any writes to check the availability of
    the data.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int BytesReadWrite;
    cc_t ControlArray[NCCS];
    tcflag_t ControlFlags;
    int ExpectedResult;
    tcflag_t InputFlags;
    tcflag_t LocalFlags;
    tcflag_t OutputFlags;
    int PtmFd;
    int PtsFd;
    char ReadBuffer[10];
    int Result;
    const char* WriteValue = "hello";

    //
    // Initialize locals
    //

    PtmFd = -1;
    PtsFd = -1;

    //
    // Open Master-Subordinate
    //

    LxtCheckErrno(OpenMasterSubordinate(&PtmFd, &PtsFd, NULL, NULL));
    LxtLogInfo("Master opened at FD:%d", PtmFd);
    LxtLogInfo("Subordinate opened at FD:%d", PtsFd);

    //
    // Write non-terminated string to the master.
    //

    ExpectedResult = strlen(WriteValue);
    LxtLogInfo("Writing to master");
    LxtCheckErrno(BytesReadWrite = write(PtmFd, WriteValue, ExpectedResult));
    LxtCheckFnResults("write", BytesReadWrite, ExpectedResult);

    //
    // Capture termios settings.
    //

    LxtCheckResult(TerminalSettingsGet(PtsFd, ControlArray, &ControlFlags, &InputFlags, &LocalFlags, &OutputFlags));

    //
    // On Ubuntu16 pty processing is done asynchronously so pause for a second
    // to give the write time to be processed before turning off canonical mode.
    //

    sleep(1);

    //
    // Switch to non-canonical mode.
    //

    LxtCheckErrno(RawInit(PtsFd));

    //
    // Restore termios settings.
    //

    LxtCheckResult(TerminalSettingsSet(PtsFd, ControlArray, ControlFlags, InputFlags, LocalFlags, OutputFlags));

    //
    // Check subordinate data.
    //

    LxtLogInfo("Reading from subordinate...");
    ExpectedResult = strlen(WriteValue);
    LxtCheckErrno(BytesReadWrite = read(PtsFd, ReadBuffer, sizeof(ReadBuffer)));
    LxtCheckFnResults("read", BytesReadWrite, ExpectedResult);
    ReadBuffer[ExpectedResult] = '\0';
    LxtCheckStringEqual(ReadBuffer, WriteValue);

ErrorExit:
    if (PtmFd != -1)
    {
        close(PtmFd);
    }

    if (PtsFd != -1)
    {
        close(PtsFd);
    }

    return Result;
}

int PtLineBreakCheck10(PLXT_ARGS Args)

/*++

Routine Description:

    This routine writes a non-terminated string in canonical mode, then
    switches to and from raw mode, eventually reading the results in
    raw mode.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int BytesReadWrite;
    cc_t ControlArray[NCCS];
    tcflag_t ControlFlags;
    int ExpectedResult;
    tcflag_t InputFlags;
    tcflag_t LocalFlags;
    tcflag_t OutputFlags;
    int PtmFd;
    int PtsFd;
    char ReadBuffer[10];
    int Result;
    const char* WriteValue = "hello";

    //
    // Initialize locals
    //

    PtmFd = -1;
    PtsFd = -1;

    //
    // Open Master-Subordinate
    //

    LxtCheckErrno(OpenMasterSubordinate(&PtmFd, &PtsFd, NULL, NULL));
    LxtLogInfo("Master opened at FD:%d", PtmFd);
    LxtLogInfo("Subordinate opened at FD:%d", PtsFd);

    //
    // Write non-terminated string to the master.
    //

    ExpectedResult = strlen(WriteValue);
    LxtLogInfo("Writing to master");
    LxtCheckErrno(BytesReadWrite = write(PtmFd, WriteValue, ExpectedResult));
    LxtCheckFnResults("write", BytesReadWrite, ExpectedResult);

    //
    // Capture termios settings.
    //

    LxtCheckResult(TerminalSettingsGet(PtsFd, ControlArray, &ControlFlags, &InputFlags, &LocalFlags, &OutputFlags));

    //
    // Switch to non-canonical mode.
    //

    LxtCheckErrno(RawInit(PtsFd));

    //
    // Restore termios settings.
    //

    LxtCheckResult(TerminalSettingsSet(PtsFd, ControlArray, ControlFlags, InputFlags, LocalFlags, OutputFlags));

    //
    // Switch back to non-canonical mode.
    //

    LxtCheckErrno(RawInit(PtsFd));

    //
    // Check subordinate data.
    //

    ExpectedResult = strlen(WriteValue);
    LxtCheckErrno(BytesReadWrite = read(PtsFd, ReadBuffer, sizeof(ReadBuffer)));
    LxtCheckFnResults("read", BytesReadWrite, ExpectedResult);
    ReadBuffer[ExpectedResult] = '\0';
    LxtCheckStringEqual(ReadBuffer, WriteValue);

ErrorExit:
    if (PtmFd != -1)
    {
        close(PtmFd);
    }

    if (PtsFd != -1)
    {
        close(PtsFd);
    }

    return Result;
}

int PtMasterFillBuffer(PLXT_ARGS Args)

/*++

Routine Description:

    This routine will fill the master buffer by writing to the subordinate, and
    then test various scenarios:
        1. Write to the master with echo on.
        2. Turn suspend on/off which normally would echo.
        3. Perform a blocking write and unblock via different mechanisms
            a. Read bytes to free up space
            b. flush
            c. close the master causing a hangup

Arguments:

    None.

Return Value:

    Returns 0 on success; -1 on error.

--*/

{

    int BytesReadWrite;
    pid_t ChildPid;
    int Iterations;
    int OldFlags;
    int PtmFd;
    int PtsFd;
    char ReadBuffer[1024];
    int Result;
    int SerialNumber;
    int Status;
    char TestBuffer[] = "ZYXWVUTSRQPO\n";
    char TestBufferEcho[] = "ZYXWVUTSRQPO\r\n";
    size_t TestBufferLen;
    size_t TotalBytes;
    struct timeval Timeout;
    char WriteBuffer[] = "0123456789ABC";
    size_t WriteBufferLen;
    fd_set WriteFds;

    //
    // Initialize locals
    //

    ChildPid = -1;
    PtmFd = -1;
    PtsFd = -1;
    Result = 0;
    TestBufferLen = sizeof(TestBuffer) - 1;
    WriteBufferLen = sizeof(WriteBuffer) - 1;

    LXT_SYNCHRONIZATION_POINT_START();

    //
    // Open Master-Subordinate
    //

    LxtCheckErrno(OpenMasterSubordinate(&PtmFd, &PtsFd, NULL, &SerialNumber));
    LxtLogInfo("Master opened at FD:%d", PtmFd);
    LxtLogInfo("Subordinate Serial Number: %d", SerialNumber);
    LxtLogInfo("Subordinate opened at FD:%d", PtsFd);

    //
    // Fork
    //

    LxtCheckErrno((ChildPid = fork()));
    if (ChildPid == 0)
    {

        //
        // Child.
        //
        // Mark the subordinate Non-blocking and write to it in a loop.
        // When it is out of room, it will return with EAGAIN.
        //

        fcntl(PtsFd, F_SETFL, O_NONBLOCK);
        LxtLogInfo(
            "Filling up the subordinate's buffer. "
            "This might take some time...");

        TotalBytes = 0;
        for (;;)
        {
            BytesReadWrite = write(PtsFd, WriteBuffer, WriteBufferLen);
            if (BytesReadWrite < 0)
            {
                if (errno != EAGAIN)
                {
                    LxtLogError(
                        "Expecting the write to return with "
                        "result:%d(%s), but it returned with result:%d(%s)",
                        EAGAIN,
                        strerror(EAGAIN),
                        errno,
                        strerror(errno));

                    Result = -1;
                    goto ErrorExit;
                }
                else
                {

                    //
                    // On Ubuntu, the buffer auto-expands at least once under
                    // memory pressure so wait a bit to see if the buffer is
                    // really full.
                    //

                    memset(&Timeout, 0, sizeof(Timeout));
                    Timeout.tv_sec = 1;
                    FD_ZERO(&WriteFds);
                    FD_SET(PtsFd, &WriteFds);
                    LxtCheckErrno(select((PtsFd + 1), NULL, &WriteFds, NULL, &Timeout));
                    if (Result == 0)
                    {
                        break;
                    }
                }
            }
            else if (BytesReadWrite < WriteBufferLen)
            {
                LxtLogInfo("Last write added %d bytes of %d bytes", BytesReadWrite, WriteBufferLen);
            }

            TotalBytes += BytesReadWrite;
        }

        LxtLogInfo("Buffer filled up with %lld bytes", TotalBytes);

        //
        // Try to write to the master with echo on and a full master endpoint
        // buffer. The write should succeed and the echo characters should be
        // discarded.
        //

        fcntl(PtmFd, F_SETFL, O_NONBLOCK);
        LxtCheckErrno(BytesReadWrite = write(PtmFd, TestBuffer, TestBufferLen));

        //
        // Check that the test message with failed echo was received.
        //

        LxtLogInfo("Reading back message written to master...");
        LxtCheckErrno(BytesReadWrite = read(PtsFd, ReadBuffer, sizeof(ReadBuffer)));
        LxtCheckEqual(BytesReadWrite, TestBufferLen, "%llu");
        LxtCheckMemoryEqual(ReadBuffer, TestBuffer, TestBufferLen);

        //
        // Try to turn suspend on/off with the buffer full. This normally would
        // echo the start/stop characters back to the master endpoint.
        //

        LxtLogInfo("Toggling suspend...");
        LxtCheckErrno(tcflow(PtmFd, TCIOFF));
        LxtCheckErrno(tcflow(PtmFd, TCION));
        LxtClose(PtmFd);

        //
        // Drain on Linux is effected by the buffer being full, but because
        // PtsFd is marked with O_NONBLOCK this should complete.
        //

        LxtLogInfo("Draining queue...");
        LxtCheckErrno(tcdrain(PtsFd));

        //
        // Sanity check to verify that the buffer is still full.
        //

        LxtCheckErrnoFailure(BytesReadWrite = write(PtsFd, WriteBuffer, WriteBufferLen), EAGAIN);

        //
        // Try to write a byte, which will block. Wait for the other thread
        // to unblock this request. Do this multiple times to test different
        // methods of unblocking.
        //

        for (Iterations = 0; Iterations < 2; Iterations += 1)
        {
            LXT_SYNCHRONIZATION_POINT();
            LxtLogInfo("Writing to the subordinate.");
            LxtCheckErrno((OldFlags = fcntl(PtsFd, F_GETFL)));
            fcntl(PtsFd, F_SETFL, OldFlags & ~O_NONBLOCK);
            LxtCheckErrno(BytesReadWrite = write(PtsFd, WriteBuffer, 1));
            LxtCheckEqual(BytesReadWrite, 1, "%llu");

            //
            // Fill the buffer back up.
            //

            LxtLogInfo("Refilling the buffer...");
            LxtCheckErrno((OldFlags = fcntl(PtsFd, F_GETFL)));
            fcntl(PtsFd, F_SETFL, OldFlags | O_NONBLOCK);
            for (;;)
            {
                BytesReadWrite = write(PtsFd, WriteBuffer, WriteBufferLen);
                if (BytesReadWrite < 0)
                {
                    LxtCheckErrnoFailure(-1, EAGAIN);
                    break;
                }
            }
        }

        //
        // When the master hangs up eventually, the blocked write should
        // return.
        //

        LXT_SYNCHRONIZATION_POINT();
        LxtLogInfo("Writing to the subordinate.");
        LxtCheckErrno((OldFlags = fcntl(PtsFd, F_GETFL)));
        fcntl(PtsFd, F_SETFL, OldFlags & ~O_NONBLOCK);
        LxtCheckErrnoFailure((BytesReadWrite = write(PtsFd, WriteBuffer, 1)), EIO);
    }
    else
    {

        //
        // Parent.
        //

        //
        // Close the subordinate device handle and wait for the master buffer
        // to fill.
        //

        LxtLogInfo("Waiting for the subordinate to fill its buffer...");
        LXT_SYNCHRONIZATION_POINT();

        //
        // Wait a bit to make sure the write from the child is blocked.
        //

        LxtLogInfo("Waiting a bit for the subordinate write to block...");
        sleep(1);

        //
        // On Ubuntu, there seems to be some odd behavior when you fill the
        // buffer up. After filling the buffer, you need to read some multiple
        // of the byte chunks written before a new write will succeed. For
        // example, if you write 13 bytes 1522 times to fill up the buffer, you
        // may need to read back 36 of those writes (13*36 = 468 bytes) before
        // the next write will succeed. Worse, if you queue a write while the
        // buffer is full, you need to completely empty the buffer before that
        // write will complete.
        //

        LxtLogInfo("Unblocking write by reading from master...");
        TotalBytes = 0;
        for (Iterations = 0;; Iterations += 1)
        {
            LxtCheckErrno(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)));
            TotalBytes += BytesReadWrite;
            LxtLogInfo("Checking write ready...");
            memset(&Timeout, 0, sizeof(Timeout));
            Timeout.tv_sec = 1;
            FD_ZERO(&WriteFds);
            FD_SET(PtsFd, &WriteFds);
            LxtCheckErrno(select((PtsFd + 1), NULL, &WriteFds, NULL, &Timeout));
            if (Result == 1)
            {
                break;
            }
        }

        LxtLogInfo("Removed %lld bytes from buffer", TotalBytes);

        //
        // Unblock write by flushing master input buffer.
        //

        LXT_SYNCHRONIZATION_POINT();
        LxtLogInfo("Waiting a bit for the subordinate write to block...");
        sleep(1);
        LxtLogInfo("Unblocking write by flushing master input...");
        LxtCheckErrno(tcflush(PtmFd, TCIFLUSH));

        //
        // On Ubuntu, flushing the subordinate output buffer appears to free up
        // space as expected. It does not however seem to complete the queued
        // read. Skip this test for now.
        //

        /*
        LXT_SYNCHRONIZATION_POINT();
        LxtLogInfo("Waiting a bit for the subordinate write to block...");
        sleep(1);
        LxtLogInfo("Unblocking write by flushing subordinate output...");
        LxtCheckErrno(tcflush(PtsFd, TCOFLUSH));
        LxtCheckErrno(BytesReadWrite = read(PtmFd, ReadBuffer, 1));
        */

        LxtClose(PtsFd);

        //
        // Hangup the master endpoint.
        //

        LXT_SYNCHRONIZATION_POINT();
        LxtLogInfo("Waiting a bit for the subordinate write to block...");
        sleep(1);
        LxtLogInfo("Hanging up master endpoint to unblock writer thread.") LxtClose(PtmFd);
    }

    Result = 0;

ErrorExit:
    if ((ChildPid != 0) && (PtmFd != -1))
    {
        close(PtmFd);
    }

    if ((ChildPid != 0) && (PtsFd != -1))
    {
        close(PtsFd);
    }

    LXT_SYNCHRONIZATION_POINT_END();
    return Result;
}

int PtMasterHangup1(PLXT_ARGS Args)

/*++

Routine Description:

    This routine will try to determine the behavior when the subordinate
    tries to write after the master has hangup.
    Expected Result: The write on subordinate should return with error 5:EIO.

Arguments:

    None.

Return Value:

    Returns 0 on success; -1 on error.

--*/

{

    int BytesReadWrite;
    int LoopCount;
    int NonBlockingValue;
    int PtmFd;
    int PtsFd;
    char ReadBuffer[1024];
    int Result;
    int SerialNumber;
    tcflag_t TermiosFlags;
    char WriteBuffer[10] = {"123456789"};

    //
    // Initialize locals
    //

    PtmFd = -1;
    PtsFd = -1;
    Result = 0;

    //
    // Open Master-Subordinate
    //

    LxtCheckErrno(OpenMasterSubordinate(&PtmFd, &PtsFd, NULL, &SerialNumber));
    LxtLogInfo("Master opened at FD:%d", PtmFd);
    LxtLogInfo("Subordinate Serial Number: %d", SerialNumber);
    LxtLogInfo("Subordinate opened at FD:%d", PtsFd);

    //
    // Hangup Master
    //

    LxtClose(PtmFd);

    //
    // Set a file-descriptor flag.
    //

    NonBlockingValue = 0;
    LxtCheckErrno(ioctl(PtsFd, FIONBIO, &NonBlockingValue));

    //
    // Attempt to get the current termios settings from the subordinate.
    //

    LxtCheckErrnoFailure(TerminalSettingsGetOutputFlags(PtsFd, &TermiosFlags), EIO);

    //
    // Write on subordinate.
    //

    LxtCheckErrnoFailure((BytesReadWrite = write(PtsFd, WriteBuffer, strlen(WriteBuffer) + 1)), EIO);

    //
    // Mark the subordinate as non-blocking and attempt write again.
    // Expected behavior is the same as of previous write.
    //

    fcntl(PtsFd, F_SETFL, O_NONBLOCK);
    LxtCheckErrnoFailure((BytesReadWrite = write(PtsFd, WriteBuffer, strlen(WriteBuffer) + 1)), EIO);

ErrorExit:
    if (PtmFd != -1)
    {
        close(PtmFd);
    }

    if (PtsFd != -1)
    {
        close(PtsFd);
    }

    return Result;
}

int PtMasterHangup2(PLXT_ARGS Args)

/*++

Routine Description:

    This routine will try to determine the behavior when the master opens
    and closes immediately. Subordinate then tries to read. Also checks the
    behavior of the master disconnecting while the subordinate is blocked in a
    read.
    Expected Result: The read on subordinate should return 0 bytes read.

Arguments:

    None.

Return Value:

    Returns 0 on success; -1 on error.

--*/

{

    int BytesReadWrite;
    int LoopCount;
    pid_t Pid;
    int PtmFd;
    int PtsFd;
    char ReadBuffer[1024];
    int Result;
    int SerialNumber;
    char WriteBuffer[10] = {"123456789"};

    //
    // Initialize locals
    //

    Pid = -1;
    PtmFd = -1;
    PtsFd = -1;
    Result = LXT_RESULT_FAILURE;

    //
    // First check the behavior of read after hang-up.
    //

    //
    // Open Master-Subordinate
    //

    LxtCheckErrno(OpenMasterSubordinate(&PtmFd, &PtsFd, NULL, &SerialNumber));
    LxtLogInfo("Master opened at FD:%d", PtmFd);
    LxtLogInfo("Subordinate Serial Number: %d", SerialNumber);
    LxtLogInfo("Subordinate opened at FD:%d", PtsFd);

    //
    // Hangup Master
    //

    LxtClose(PtmFd);

    //
    // read on subordinate.
    //

    LxtCheckErrno((BytesReadWrite = read(PtsFd, ReadBuffer, sizeof(ReadBuffer))));
    LxtCheckFnResults("read", BytesReadWrite, 0);
    LxtClose(PtsFd);

    //
    // Now, check the behavior of hang-up during a blocking read.
    //

    //
    // Open Master-Subordinate
    //

    LxtCheckErrno(OpenMasterSubordinate(&PtmFd, &PtsFd, NULL, &SerialNumber));
    LxtLogInfo("Master opened at FD:%d", PtmFd);
    LxtLogInfo("Subordinate Serial Number: %d", SerialNumber);
    LxtLogInfo("Subordinate opened at FD:%d", PtsFd);

    LxtCheckErrno((Pid = fork()));
    if (Pid == 0)
    {

        //
        // Child - hangup during a blocked read returns EIO but a read after
        // hangup returns EOF.
        //

        LxtClose(PtmFd);
        LxtCheckErrnoFailure(BytesReadWrite = read(PtsFd, ReadBuffer, sizeof(ReadBuffer)), EIO);
        LxtCheckErrno(BytesReadWrite = read(PtsFd, ReadBuffer, sizeof(ReadBuffer)));
        LxtCheckFnResults("read", BytesReadWrite, 0);
        Result = LXT_RESULT_SUCCESS;
        goto ErrorExit;
    }

    //
    // Close the subordinate device handle.
    //

    LxtClose(PtsFd);
    LxtLogInfo("Waiting for the subordinate to block in read...");
    usleep(2 * 500 * 1000);

    //
    // Hangup master. This should unblock the subordinate's blocked read.
    //

    LxtClose(PtmFd);
    LxtCheckResult(LxtWaitPidPoll(Pid, 0));
    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    if (PtmFd != -1)
    {
        close(PtmFd);
    }

    if (PtsFd != -1)
    {
        close(PtsFd);
    }

    //
    // Exit if child process.
    //

    if (Pid == 0)
    {
        _exit(Result);
    }

    return Result;
}

int PtMasterHangup3(PLXT_ARGS Args)

/*++

Routine Description:

    This routine will try to determine the behavior when the master opens,
    writes some complete messages and closes. Subordinate then tries to read.
    Expected Result: The read on subordinate should return 0 bytes read.

Arguments:

    None.

Return Value:

    Returns 0 on success; -1 on error.

--*/

{

    int BytesReadWrite;
    int ExpectedResult;
    int LoopCount;
    int PtmFd;
    int PtsFd;
    char ReadBuffer[50];
    int Result;
    int SerialNumber;
    char WriteBuffer[] = {"123456789"};

    //
    // Initialize locals
    //

    PtmFd = -1;
    PtsFd = -1;
    Result = 0;

    //
    // Open Master-Subordinate
    //

    LxtCheckErrno(OpenMasterSubordinate(&PtmFd, &PtsFd, NULL, &SerialNumber));
    LxtLogInfo("Master opened at FD:%d", PtmFd);
    LxtLogInfo("Subordinate Serial Number: %d", SerialNumber);
    LxtLogInfo("Subordinate opened at FD:%d", PtsFd);

    //
    // Write two complete messages to the Master.
    //

    ExpectedResult = strlen(WriteBuffer) + 1;
    LxtCheckErrno((BytesReadWrite = write(PtmFd, WriteBuffer, ExpectedResult)));

    LxtCheckFnResults("write", BytesReadWrite, ExpectedResult);
    LxtCheckErrno((BytesReadWrite = write(PtmFd, WriteBuffer, ExpectedResult)));

    LxtCheckFnResults("write", BytesReadWrite, ExpectedResult);

    //
    // Hangup Master
    //

    LxtClose(PtmFd);

    //
    // read on subordinate.
    //

    LxtCheckErrno((BytesReadWrite = read(PtsFd, ReadBuffer, sizeof(ReadBuffer))));

    LxtCheckFnResults("read", BytesReadWrite, 0);

ErrorExit:
    if (PtmFd != -1)
    {
        close(PtmFd);
    }

    if (PtsFd != -1)
    {
        close(PtsFd);
    }

    return Result;
}

int PtMasterHangup4(PLXT_ARGS Args)

/*++

Routine Description:

    This routine will try to determine the behavior when the master opens,
    writes some incomplete messages and closes. Subordinate then tries to read.
    Expected Result: The read on subordinate should return 0 bytes read.

Arguments:

    None.

Return Value:

    Returns 0 on success; -1 on error.

--*/

{

    int BytesReadWrite;
    int ExpectedResult;
    int LoopCount;
    int PtmFd;
    int PtsFd;
    char ReadBuffer[1024];
    int Result;
    int SerialNumber;
    char* WriteBuffer = "123456789";

    //
    // Initialize locals
    //

    PtmFd = -1;
    PtsFd = -1;
    Result = 0;

    //
    // Open Master-Subordinate
    //

    LxtCheckErrno(OpenMasterSubordinate(&PtmFd, &PtsFd, NULL, &SerialNumber));
    LxtCheckErrno(RawInit(PtsFd));
    LxtLogInfo("Master opened at FD:%d", PtmFd);
    LxtLogInfo("Subordinate Serial Number: %d", SerialNumber);
    LxtLogInfo("Subordinate opened at FD:%d", PtsFd);
    LxtLogInfo("Setting non blocking");
    int one = fcntl(PtsFd, F_SETFD, O_NONBLOCK);
    fcntl(PtmFd, F_SETFD, O_NONBLOCK);

    //
    // Write an incomplete(without the last CR) message to the Master.
    //

    ExpectedResult = strlen(WriteBuffer);
    LxtCheckErrno((BytesReadWrite = write(PtmFd, WriteBuffer, ExpectedResult)));
    LxtCheckFnResults("write", BytesReadWrite, ExpectedResult);

    //
    // Hangup Master
    //

    LxtClose(PtmFd);

    //
    // read on subordinate.
    //

    LxtCheckErrno((BytesReadWrite = read(PtsFd, ReadBuffer, 1)));
    LxtCheckFnResults("read", BytesReadWrite, 0);

ErrorExit:
    if (PtmFd != -1)
    {
        close(PtmFd);
    }

    if (PtsFd != -1)
    {
        close(PtsFd);
    }

    return Result;
}

int PtMoreThanOne(PLXT_ARGS Args)

/*++

Routine Description:

    This routine validates that more than one pseudo terminal can be opened
    at any given time. Ideally the test should validate for MAX pt, but there
    can be open pt's while the test is executing (for example, if the test is
    run over adb shell). So, the test will validate that at least half of max
    pt's can be opened. For every master-subordinate pair that it opens, it
    will also perform a simple read/write check.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int Itr;

    //
    // Number of times to test.
    //

    int Loop;
    int LoopCount;
    int NumPtToTest;
    char PtsDevName[PTS_DEV_NAME_BUFFER_SIZE];
    int Result;

    //
    // PtsFd[<n>][0] will hold the fd for master and
    // PtsFd[<n>][1]] will hold the fd for the subordinate.
    //

    int PtFds[PTY_MAX_OPEN_LIMIT / 2][2];
    int SerialNumber;

    //
    // Initialize locals
    //

    Loop = 2;
    NumPtToTest = PTY_MAX_OPEN_LIMIT / 2;
    for (Itr = 0; Itr < NumPtToTest; Itr++)
    {
        PtFds[Itr][0] = -1;
        PtFds[Itr][1] = -1;
    }

    for (LoopCount = 0; LoopCount < Loop; LoopCount++)
    {
        LxtLogInfo("Opening %d pt's, loop count:%d", NumPtToTest, LoopCount + 1);

        for (Itr = 0; Itr < NumPtToTest; Itr++)
        {

            //
            // Open Master-Subordinate for Itr
            //

            LxtCheckErrno(OpenMasterSubordinate(&PtFds[Itr][0], &PtFds[Itr][1], PtsDevName, &SerialNumber));

            //
            // Enable raw input on the subordinates.
            //

            LxtCheckErrno(RawInit(PtFds[Itr][1]));
            LxtLogInfo("Master opened at FD:%d", PtFds[Itr][0]);
            LxtLogInfo("Subordinate Device is:%s", PtsDevName);
            LxtLogInfo("Subordinate Serial Number: %d", SerialNumber);
            LxtLogInfo("Subordinate opened at FD:%d", PtFds[Itr][1]);

            //
            // Perform a simple read/write check on the master-subordinate.
            //

            LxtLogInfo(
                "Performing a simple read/write check on"
                "master-subordinate pair...");

            LxtCheckErrno(SimpleReadWriteCheck(PtFds[Itr][0], PtFds[Itr][1]));
        }

        //
        // Once all of the pt's are open, close them for the next
        // Loop.
        //

        LxtLogInfo("Closing the pt's");
        for (Itr = 0; Itr < NumPtToTest; Itr++)
        {
            LxtClose(PtFds[Itr][0]);
            LxtClose(PtFds[Itr][1]);
        }
    }

ErrorExit:
    for (Itr = 0; Itr < NumPtToTest; Itr++)
    {
        if (PtFds[Itr][0] != -1)
        {
            close(PtFds[Itr][0]);
        }

        if (PtFds[Itr][1] != -1)
        {
            close(PtFds[Itr][1]);
        }
    }

    return Result;
}

int PtMultiMessageReadWrite(PLXT_ARGS Args)

/*++

Routine Description:

    This routine validates multi-message behavior.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int BytesReadWrite;
    int ExpectedResult;
    char Lf = 10;
    struct iovec Iov[5];
    int MessageNum;
    char* Messages[] = {"ABC\n", "\n", "DE\r", "FG", "HI\n"};
    char* ExpectedReadMessages[] = {"ABC\n", "\n", "DE\n", "FGHI\n"};
    int OldFlags;
    void* PointerResult;
    int PtmFd;
    FILE* PtmFile;
    int PtsFd;
    char ReadMessage[50] = {0};
    int Result;
    int SerialNumber;

    //
    // Initialize locals
    //

    PtmFd = -1;
    PtsFd = -1;
    PtmFile = NULL;

    //
    // Open Master-Subordinate
    //

    LxtCheckErrno(OpenMasterSubordinate(&PtmFd, &PtsFd, NULL, &SerialNumber));
    LxtCheckNullErrno(PtmFile = fdopen(PtmFd, "w"));

    //
    // This is a message boundary test, do not set the subordinate for raw init.
    //

    LxtLogInfo("Master opened at FD:%d", PtmFd);
    LxtLogInfo("Subordinate Serial Number: %d", SerialNumber);
    LxtLogInfo("Subordinate opened at FD:%d", PtsFd);

    //
    // Send the message(1 to 4) to the subordinate.
    //

    BytesReadWrite = fprintf(PtmFile, "%s%s%s%s", Messages[0], Messages[1], Messages[2], Messages[3]);
    LxtCheckErrno(fflush(PtmFile));
    ExpectedResult = strlen(Messages[0]) + strlen(Messages[1]) + strlen(Messages[2]) + strlen(Messages[3]);
    LxtLogInfo("Message sent(%d bytes) to subordinate: \n%s%s%s%s", BytesReadWrite, Messages[0], Messages[1], Messages[2], Messages[3]);

    LxtCheckFnResults("fprintf", BytesReadWrite, ExpectedResult);

    //
    // Every read from the subordinate should return one message at a time.
    // If the message is not complete, the read will block.
    //

    for (MessageNum = 0; MessageNum < 3; ++MessageNum)
    {
        LxtCheckErrno(BytesReadWrite = read(PtsFd, ReadMessage, sizeof(ReadMessage)));

        ReadMessage[BytesReadWrite] = '\0';
        LxtLogInfo("Message read(%d bytes) from subordinate: \n%s", BytesReadWrite, ReadMessage);

        ExpectedResult = strlen(ExpectedReadMessages[MessageNum]);
        LxtCheckFnResults("read", BytesReadWrite, ExpectedResult);

        //
        // Compare the messages.
        //

        if (memcmp(ReadMessage, ExpectedReadMessages[MessageNum], min(BytesReadWrite, ExpectedResult)) != 0)
        {

            LxtLogError(
                "Data read from subordinate does not match what was "
                "written by master.");
            Result = -1;
            goto ErrorExit;
        }
    }

    //
    // Next read on the subordinate should block. Set it to non-blocking.
    //

    LxtCheckErrno(fcntl(PtsFd, F_SETFL, O_NONBLOCK));
    LxtCheckErrnoFailure(read(PtsFd, ReadMessage, sizeof(ReadMessage)), EAGAIN);

    //
    // Complete the message from the master side and try reading again
    // from the subordinate.
    //

    ExpectedResult = strlen(Messages[MessageNum + 1]);
    BytesReadWrite = write(PtmFd, Messages[MessageNum + 1], ExpectedResult);
    LxtLogInfo("Message sent(%d bytes) to subordinate: \n%s", BytesReadWrite, Messages[MessageNum + 1]);

    LxtCheckFnResults("write", BytesReadWrite, ExpectedResult);

    //
    // Mark the subordinate as blocking again.
    //

    LxtCheckErrno((OldFlags = fcntl(PtsFd, F_GETFL)));
    LxtCheckErrno(fcntl(PtsFd, F_SETFL, OldFlags & ~O_NONBLOCK));

    //
    // Read next message.
    //

    LxtCheckErrno(BytesReadWrite = read(PtsFd, ReadMessage, sizeof(ReadMessage)));

    ReadMessage[BytesReadWrite] = '\0';
    LxtLogInfo("Message read(%d bytes) from subordinate: \n%s", BytesReadWrite, ReadMessage);

    ExpectedResult = strlen(ExpectedReadMessages[MessageNum]);
    LxtCheckFnResults("read", BytesReadWrite, ExpectedResult);

    //
    // Compare the messages.
    //

    if (memcmp(ReadMessage, ExpectedReadMessages[MessageNum], min(BytesReadWrite, ExpectedResult)) != 0)
    {

        LxtLogError(
            "Data read from subordinate does not match what was "
            "written by master.");
        Result = -1;
        goto ErrorExit;
    }

    //
    // Use the writev system call to write the messages again.
    //

    Iov[0].iov_base = Messages[0];
    Iov[0].iov_len = strlen(Messages[0]);
    Iov[1].iov_base = Messages[1];
    Iov[1].iov_len = strlen(Messages[1]);
    Iov[2].iov_base = Messages[2];
    Iov[2].iov_len = strlen(Messages[2]);
    Iov[3].iov_base = Messages[3];
    Iov[3].iov_len = strlen(Messages[3]);
    Iov[4].iov_base = Messages[4];
    Iov[4].iov_len = strlen(Messages[4]);
    LxtCheckErrno(BytesReadWrite = writev(PtmFd, Iov, 5));
    LxtLogInfo("writev wrote %d bytes", BytesReadWrite);

    //
    // Every read from the subordinate should return one message at a time.
    // If the message is not complete, the read will block.
    //

    for (MessageNum = 0; MessageNum < 4; ++MessageNum)
    {
        LxtCheckErrno(BytesReadWrite = read(PtsFd, ReadMessage, sizeof(ReadMessage)));

        ReadMessage[BytesReadWrite] = '\0';
        LxtLogInfo("Message %d read(%d bytes) from subordinate: \n%s", MessageNum, BytesReadWrite, ReadMessage);

        ExpectedResult = strlen(ExpectedReadMessages[MessageNum]);
        LxtCheckFnResults("read", BytesReadWrite, ExpectedResult);

        //
        // Compare the messages.
        //

        if (memcmp(ReadMessage, ExpectedReadMessages[MessageNum], min(BytesReadWrite, ExpectedResult)) != 0)
        {

            LxtLogError(
                "Data read from subordinate does not match what was "
                "written by master.");
            Result = -1;
            goto ErrorExit;
        }
    }

    //
    // Ensure there are no other messages.
    //

    LxtCheckErrno(fcntl(PtsFd, F_SETFL, O_NONBLOCK));
    LxtCheckErrnoFailure(read(PtsFd, ReadMessage, sizeof(ReadMessage)), EAGAIN);

ErrorExit:
    if (PtmFd != -1)
    {
        close(PtmFd);
    }

    if (PtsFd != -1)
    {
        close(PtsFd);
    }

    if (PtmFile != NULL)
    {
        fclose(PtmFile);
    }

    return Result;
}

int PtReadNoSub1(PLXT_ARGS Args)

/*++

Routine Description:

    The PtReadNoSubxxx validate the behavior of read on the master, when
    there are no open handles to the subordinate; where 'xxx' is the
    sub-test case as described below:
    1. A handle to the subordinate was never opened
       Expected Result:
           For blocking call, the read will block.
           For non-blocking call, read should return error EAGAIN.
    2. A handle to subordinate was opened and closed and then the read on
       master was attempted.
       Expected Result: Read should return error:5(EIO).
    3. A handle to subordinate was opened, sub wrote few bytes and then
       closed. Then read is attempted on master for fewer bytes than that
       were written.
       Expected Result: Read should return successfully for the number of
           bytes written. After that any read should return error:5(EIO).

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    char Buffer;
    int PtmFd;
    int Result;

    //
    // Initialize locals
    //

    PtmFd = -1;

    //
    // Open Master.
    //

    LxtCheckErrno((PtmFd = open("/dev/ptmx", O_RDWR)));
    LxtLogInfo("Master opened at FD:%d", PtmFd);

    //
    // Set master to non-blocking and then attempt a read on master.
    //

    fcntl(PtmFd, F_SETFL, O_NONBLOCK);
    LxtCheckErrnoFailure(read(PtmFd, &Buffer, 1), EAGAIN);

ErrorExit:
    if (PtmFd != -1)
    {
        close(PtmFd);
    }

    return Result;
}

int PtReadNoSub2(PLXT_ARGS Args)

/*++

Routine Description:

    See PtReadNoSub1 for details.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    char Buffer;
    int PtmFd;
    int PtsFd;
    int Result;
    int SerialNumber;

    //
    // Initialize locals
    //

    PtmFd = -1;
    PtsFd = -1;

    //
    // Open Master-Subordinate
    //

    LxtCheckErrno(OpenMasterSubordinate(&PtmFd, &PtsFd, NULL, &SerialNumber));
    LxtLogInfo("Master opened at FD:%d", PtmFd);
    LxtLogInfo("Subordinate Serial Number: %d", SerialNumber);
    LxtLogInfo("Subordinate opened at FD:%d", PtsFd);

    //
    // Close the subordinate.
    //

    LxtClose(PtsFd);
    LxtLogInfo("Subordinate closed");

    //
    // Set master to non-blocking and then attempt a read on master.
    //

    LxtCheckErrnoFailure(read(PtmFd, &Buffer, 1), EIO);

ErrorExit:
    if (PtmFd != -1)
    {
        close(PtmFd);
    }

    if (PtsFd != -1)
    {
        close(PtsFd);
    }

    return Result;
}

int PtReadNoSub3(PLXT_ARGS Args)

/*++

Routine Description:

    See PtReadNoSub1 for details.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int Result;
    int ExpectedResult;
    int PtmFd;
    int PtsFd;
    char ReadBuffer[10];
    char WriteBuffer[] = "abcd";
    int SerialNumber;
    int BytesReadWrite;

    //
    // Initialize locals
    //

    PtmFd = -1;
    PtsFd = -1;

    //
    // Open Master-Subordinate
    //

    LxtCheckErrno(OpenMasterSubordinate(&PtmFd, &PtsFd, NULL, &SerialNumber));
    LxtLogInfo("Master opened at FD:%d", PtmFd);
    LxtLogInfo("Subordinate Serial Number: %d", SerialNumber);
    LxtLogInfo("Subordinate opened at FD:%d", PtsFd);

    //
    // Write few bytes to the subordinate.
    //

    ExpectedResult = strlen(WriteBuffer);
    LxtCheckErrno((BytesReadWrite = write(PtsFd, &WriteBuffer, ExpectedResult)));

    LxtCheckFnResults("write", BytesReadWrite, ExpectedResult);

    //
    // Close the subordinate.
    //

    LxtClose(PtsFd);
    LxtLogInfo("Subordinate closed");

    //
    // Set master to non-blocking and then attempt a read on master.
    //

    LxtCheckErrno((BytesReadWrite = read(PtmFd, ReadBuffer, 1)));
    LxtCheckFnResults("read", BytesReadWrite, 1);
    if (ReadBuffer[0] != WriteBuffer[0])
    {
        LxtLogError(
            "data read does not match expected. Expected data:%d, "
            "read:%d",
            WriteBuffer[0],
            ReadBuffer[0]);

        Result = -1;
        goto ErrorExit;
    }

    //
    // Drain all the data from master. We have already read 1 byte before.
    //

    LxtCheckErrno((BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer))));

    ExpectedResult = strlen(WriteBuffer) - 1;
    LxtCheckFnResults("read", BytesReadWrite, ExpectedResult);

    //
    // Once the data has been drained from the master buffer, read
    // should return error.
    //

    LxtCheckErrnoFailure(read(PtmFd, &ReadBuffer, 1), EIO);

ErrorExit:
    if (PtmFd != -1)
    {
        close(PtmFd);
    }

    if (PtsFd != -1)
    {
        close(PtsFd);
    }

    return Result;
}

int PtStressIo(PLXT_ARGS Args)

/*++

Routine Description:

    The routine performs IO Stress test. It will open STRESS_NUM_PT
    number of pseudo terminals (pt). For each pt, it will create
    STRESS_NUM_THREAD threads, where each thread will do a
    SimpleReadWrite check for STRESS_NUM_ITERATION cycles.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int Itr;
    char PtsDevName[50];

    //
    // PtsFd[<n>][0] will hold the fd for master and
    // PtsFd[<n>][1]] will hold the fd for the subordinate.
    //

    int PtFds[STRESS_NUM_PT][2];
    int Result;
    int SerialNumber;
    pthread_t Thread[STRESS_NUM_PT][STRESS_NUM_THREAD];
    StressThreadArg ThreadArg[STRESS_NUM_PT];
    int ThreadItr;

    //
    // Initialize locals
    //

    for (Itr = 0; Itr < STRESS_NUM_PT; Itr++)
    {
        PtFds[Itr][0] = -1;
        PtFds[Itr][1] = -1;
    }

    //
    // Open all the pseudo terminals required for the stress.
    //

    for (Itr = 0; Itr < STRESS_NUM_PT; Itr++)
    {

        //
        // Open Master-Subordinate for Itr
        //

        LxtCheckErrno(OpenMasterSubordinate(&PtFds[Itr][0], &PtFds[Itr][1], PtsDevName, &SerialNumber));

        //
        // This is a message boundary test, do not set the subordinate for raw
        // init.
        //

        LxtLogInfo("PT#%d: Master FD:%d", Itr, PtFds[Itr][0]);
        LxtLogInfo("PT#%dSubordinate FD:%d", Itr, PtFds[Itr][1]);
        LxtLogInfo("PT#%dSubordinate Device is:%s", Itr, PtsDevName);
        LxtLogInfo("PT#%dSubordinate Serial Number: %d", Itr, SerialNumber);
    }

    //
    // For each PT, create threads.
    // Lock the stress mutex. This will allow to gate every stress thread
    // at the start.
    //

    pthread_mutex_lock(&DevPtStressMutex);
    for (Itr = 0; Itr < STRESS_NUM_PT; Itr++)
    {

        //
        // Set up the argument for the stress I/O thread.
        //

        ThreadArg[Itr].PtmFd = PtFds[Itr][0];
        ThreadArg[Itr].PtsFd = PtFds[Itr][1];
        ThreadArg[Itr].LoopCount = STRESS_NUM_ITERATION;

        for (ThreadItr = 0; ThreadItr < STRESS_NUM_THREAD; ThreadItr++)
        {

            //
            // Create I/O Stress thread#ThreadItr for PT#Itr
            //

            LxtCheckErrno(pthread_create(&Thread[Itr][ThreadItr], NULL, PerformIoStressThread, (void*)&ThreadArg[Itr]));
        }
    }

    LxtLogInfo("\nStress Start Time:");
    system("date");

    //
    // Open the flood gates.
    //

    pthread_mutex_unlock(&DevPtStressMutex);

    //
    // Wait for all the threads to terminate.
    //

    for (Itr = 0; Itr < STRESS_NUM_PT; Itr++)
    {
        for (ThreadItr = 0; ThreadItr < STRESS_NUM_THREAD; ThreadItr++)
        {
            pthread_join(Thread[Itr][ThreadItr], NULL);
        }
    }

    LxtLogInfo("\nStress End Time:");
    system("date");
    Result = 0;

ErrorExit:
    for (Itr = 0; Itr < STRESS_NUM_PT; Itr++)
    {
        if (PtFds[Itr][0] != -1)
        {
            close(PtFds[Itr][0]);
            PtFds[Itr][0] = -1;
        }

        if (PtFds[Itr][1] != -1)
        {
            close(PtFds[Itr][1]);
            PtFds[Itr][1] = -1;
        }
    }

    return Result;
}

int PtUTF8Basic(PLXT_ARGS Args)

/*++

Routine Description:

    This routine turns off canonical mode, turns on UTF8 mode and send a UTF8
    character. UTF8 mode should have no effect in either raw or canonical mode
    for this operation.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    ssize_t BytesReadWrite;
    ssize_t ExpectedResult;
    tcflag_t InputFlags;
    int PtmFd;
    int PtsFd;
    char ReadBuffer[10];
    int Result;
    int SerialNumber;
    const char* Utf8String = "\xE2\x82\xAC";

    //
    // Initialize locals
    //

    PtmFd = -1;
    PtsFd = -1;

    //
    // Open Master-Subordinate
    //

    LxtCheckErrno(OpenMasterSubordinate(&PtmFd, &PtsFd, NULL, &SerialNumber));
    LxtLogInfo("Master opened at FD:%d", PtmFd);
    LxtLogInfo("Subordinate Serial Number: %d", SerialNumber);
    LxtLogInfo("Subordinate opened at FD:%d", PtsFd);
    LxtCheckErrno(RawInit(PtsFd));

    //
    // Set UTF-8 mode.
    //

    LxtCheckResult(TerminalSettingsGetInputFlags(PtsFd, &InputFlags));
    LxtCheckResult(TerminalSettingsSetInputFlags(PtsFd, InputFlags | IUTF8));

    //
    // Write UTF-8 character to the master.
    //

    LxtLogInfo("Writing to master");
    ExpectedResult = strlen(Utf8String);
    LxtCheckErrno(BytesReadWrite = write(PtmFd, Utf8String, ExpectedResult));
    LxtCheckFnResults("write", BytesReadWrite, ExpectedResult);
    LxtLogInfo("Master(FD:%d) --> subordinate(FD:%d):%*s", PtmFd, PtsFd, BytesReadWrite, Utf8String);

    //
    // Read from subordinate.
    //

    memset(ReadBuffer, 0, sizeof(ReadBuffer));
    LxtLogInfo("Reading from subordinate");
    LxtCheckErrno(BytesReadWrite = read(PtsFd, ReadBuffer, sizeof(ReadBuffer)));
    LxtLogInfo("Message received by subordinate(FD:%d):%s", PtsFd, ReadBuffer);
    LxtCheckFnResults("read", BytesReadWrite, ExpectedResult);

    //
    // Compare the messages.
    //

    if (memcmp(ReadBuffer, Utf8String, BytesReadWrite) != 0)
    {
        LxtLogError(
            "Data read from subordinate(FD:%d) does not match what was "
            "written by master(FD:%d).",
            PtsFd,
            PtmFd);

        Result = -1;
        goto ErrorExit;
    }

    Result = 0;

ErrorExit:
    if (PtmFd != -1)
    {
        close(PtmFd);
    }

    if (PtsFd != -1)
    {
        close(PtsFd);
    }

    return Result;
}

int PtUTF8Basic2(PLXT_ARGS Args)

/*++

Routine Description:

    This routine turns off canonical mode, turns on UTF8 mode and sends two
    UTF8 characters. UTF8 mode should have no effect in either raw or canonical
    mode for this operation.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    ssize_t BytesReadWrite;
    cc_t ControlArray[NCCS];
    ssize_t ExpectedResult;
    tcflag_t InputFlags;
    int PtmFd;
    int PtsFd;
    char ReadBuffer[10];
    int Result;
    int SerialNumber;
    const char* Utf8String = "\xE2\x82\xAC\xE2\x82\xAC";

    //
    // Initialize locals
    //

    PtmFd = -1;
    PtsFd = -1;

    //
    // Open Master-Subordinate
    //

    LxtCheckErrno(OpenMasterSubordinate(&PtmFd, &PtsFd, NULL, &SerialNumber));
    LxtLogInfo("Master opened at FD:%d", PtmFd);
    LxtLogInfo("Subordinate Serial Number: %d", SerialNumber);
    LxtLogInfo("Subordinate opened at FD:%d", PtsFd);
    LxtCheckErrno(RawInit(PtsFd));

    //
    // Set UTF-8 mode.
    //

    LxtCheckResult(TerminalSettingsGetInputFlags(PtsFd, &InputFlags));
    LxtCheckResult(TerminalSettingsSetInputFlags(PtsFd, InputFlags | IUTF8));

    //
    // Verify that the minimum character value is '1' by default.
    //

    LxtCheckResult(TerminalSettingsGetControlArray(PtsFd, ControlArray));
    LxtCheckEqual(ControlArray[VMIN], 1, "%hhd");

    //
    // Write UTF-8 characters to the master.
    //

    LxtLogInfo("Writing to master");
    ExpectedResult = strlen(Utf8String);
    LxtCheckErrno(BytesReadWrite = write(PtmFd, Utf8String, ExpectedResult));
    LxtCheckFnResults("write", BytesReadWrite, ExpectedResult);
    LxtLogInfo("Master(FD:%d) --> subordinate(FD:%d):%*s", PtmFd, PtsFd, BytesReadWrite, Utf8String);

    //
    // Read a single byte from the subordinate.
    //

    memset(ReadBuffer, 0, sizeof(ReadBuffer));
    LxtLogInfo("Reading from subordinate");
    LxtCheckErrno(BytesReadWrite = read(PtsFd, ReadBuffer, 1));
    LxtLogInfo("Message received by subordinate(FD:%d):%s", PtsFd, ReadBuffer);
    LxtCheckFnResults("read", BytesReadWrite, 1);
    LxtCheckEqual(Utf8String[0], ReadBuffer[0], "%hhd");

    //
    // Read the remainder from the subordinate.
    //

    LxtLogInfo("Reading from subordinate");
    LxtCheckErrno(BytesReadWrite = read(PtsFd, &ReadBuffer[1], (sizeof(ReadBuffer) - 1)));
    LxtLogInfo("Message received by subordinate(FD:%d):%s", PtsFd, ReadBuffer);
    LxtCheckFnResults("read", BytesReadWrite, (ExpectedResult - 1));

    //
    // Compare the messages.
    //

    if (memcmp(ReadBuffer, Utf8String, ExpectedResult) != 0)
    {
        LxtLogError(
            "Data read from subordinate(FD:%d) does not match what was "
            "written by master(FD:%d).",
            PtsFd,
            PtmFd);

        Result = -1;
        goto ErrorExit;
    }

    Result = 0;

ErrorExit:
    if (PtmFd != -1)
    {
        close(PtmFd);
    }

    if (PtsFd != -1)
    {
        close(PtsFd);
    }

    return Result;
}

int PtUTF8Basic3(PLXT_ARGS Args)

/*++

Routine Description:

    This routine turns on UTF8 mode and sends two UTF-8 characters. UTF8 mode
    should have no effect in either raw or canonical mode for this operation.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    ssize_t BytesReadWrite;
    ssize_t ExpectedResult;
    tcflag_t InputFlags;
    int PtmFd;
    int PtsFd;
    char ReadBuffer[10];
    int Result;
    int SerialNumber;
    const char Utf8FirstByte = '\xE2';
    const char* Utf8String = "\xE2\x82\xAC\xE2\x82\xAC\n";

    //
    // Initialize locals
    //

    PtmFd = -1;
    PtsFd = -1;

    //
    // Open Master-Subordinate
    //

    LxtCheckErrno(OpenMasterSubordinate(&PtmFd, &PtsFd, NULL, &SerialNumber));
    LxtLogInfo("Master opened at FD:%d", PtmFd);
    LxtLogInfo("Subordinate Serial Number: %d", SerialNumber);
    LxtLogInfo("Subordinate opened at FD:%d", PtsFd);

    //
    // Set UTF-8 mode.
    //

    LxtCheckResult(TerminalSettingsGetInputFlags(PtsFd, &InputFlags));
    LxtCheckResult(TerminalSettingsSetInputFlags(PtsFd, InputFlags | IUTF8));

    //
    // Write UTF-8 characters to the master.
    //

    LxtLogInfo("Writing to master");
    ExpectedResult = strlen(Utf8String);
    LxtCheckErrno(BytesReadWrite = write(PtmFd, Utf8String, ExpectedResult));
    LxtCheckFnResults("write", BytesReadWrite, ExpectedResult);
    LxtLogInfo("Master(FD:%d) --> subordinate(FD:%d):%*s", PtmFd, PtsFd, BytesReadWrite, Utf8String);

    //
    // Canonical mode should echo the input back to the master with a
    // carriage-return and newline.
    //

    LxtCheckErrno(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)));
    ReadBuffer[BytesReadWrite] = '\0';
    LxtLogInfo("Echo received by master(FD:%d):%s", PtmFd, ReadBuffer);
    LxtLogInfo("Last character = %d [\\n = %d, \\r = %d]", ReadBuffer[BytesReadWrite - 1], '\n', '\r');
    LxtCheckFnResults("read", BytesReadWrite, (ExpectedResult + 1));
    if ((ReadBuffer[BytesReadWrite - 1] != '\n') || (ReadBuffer[BytesReadWrite - 2] != '\r'))
    {
        LxtLogError("Echo to master(FD:%d) does not end with \r\n.", PtmFd);

        Result = -1;
        goto ErrorExit;
    }

    ReadBuffer[BytesReadWrite - 2] = '\n';
    if (memcmp(ReadBuffer, Utf8String, ExpectedResult) != 0)
    {
        LxtLogError(
            "Echo to master(FD:%d) does not match what was "
            "written.",
            PtmFd);

        Result = -1;
        goto ErrorExit;
    }

    //
    // Try to read a single-byte from subordinate.
    //

    memset(ReadBuffer, 0, sizeof(ReadBuffer));
    LxtLogInfo("Reading one byte from subordinate");
    LxtCheckErrno(BytesReadWrite = read(PtsFd, ReadBuffer, 1));
    LxtLogInfo("Message received by subordinate(FD:%d):%s", PtsFd, ReadBuffer);
    LxtCheckFnResults("read", BytesReadWrite, 1);
    LxtCheckEqual(Utf8FirstByte, ReadBuffer[0], "%c");

    //
    // Try to read the rest of the message.
    //

    LxtLogInfo("Reading more from subordinate");
    LxtCheckErrno(BytesReadWrite = read(PtsFd, &ReadBuffer[1], sizeof(ReadBuffer) - 1));
    LxtLogInfo("Message received by subordinate(FD:%d):%s", PtsFd, ReadBuffer);
    LxtCheckFnResults("read", BytesReadWrite, (ExpectedResult - 1));

    //
    // Compare the messages.
    //

    if (memcmp(ReadBuffer, Utf8String, BytesReadWrite) != 0)
    {
        LxtLogError(
            "Data read from subordinate(FD:%d) does not match what was "
            "written by master(FD:%d).",
            PtsFd,
            PtmFd);

        Result = -1;
        goto ErrorExit;
    }

    Result = 0;

ErrorExit:
    if (PtmFd != -1)
    {
        close(PtmFd);
    }

    if (PtsFd != -1)
    {
        close(PtsFd);
    }

    return Result;
}

int PtUTF8Basic4(PLXT_ARGS Args)

/*++

Routine Description:

    This routine turns on UTF8 mode and send a UTF8 character. UTF8 mode should
    have no effect for this operation.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    ssize_t BytesReadWrite;
    ssize_t ExpectedResult;
    tcflag_t InputFlags;
    int PtmFd;
    int PtsFd;
    char ReadBuffer[10];
    int Result;
    int SerialNumber;
    const char* Utf8String = "\xE2\x82\xAC\n";

    //
    // Initialize locals
    //

    PtmFd = -1;
    PtsFd = -1;

    //
    // Open Master-Subordinate
    //

    LxtCheckErrno(OpenMasterSubordinate(&PtmFd, &PtsFd, NULL, &SerialNumber));
    LxtLogInfo("Master opened at FD:%d", PtmFd);
    LxtLogInfo("Subordinate Serial Number: %d", SerialNumber);
    LxtLogInfo("Subordinate opened at FD:%d", PtsFd);

    //
    // Set UTF-8 mode.
    //

    LxtCheckResult(TerminalSettingsGetInputFlags(PtsFd, &InputFlags));
    LxtCheckResult(TerminalSettingsSetInputFlags(PtsFd, InputFlags | IUTF8));

    //
    // Write UTF-8 characters to the master.
    //

    LxtLogInfo("Writing to master");
    ExpectedResult = strlen(Utf8String);
    LxtCheckErrno(BytesReadWrite = write(PtmFd, Utf8String, ExpectedResult));
    LxtCheckFnResults("write", BytesReadWrite, ExpectedResult);
    LxtLogInfo("Master(FD:%d) --> subordinate(FD:%d):%*s", PtmFd, PtsFd, BytesReadWrite, Utf8String);

    //
    // Canonical mode should echo the input back to the master with a
    // carriage-return and newline.
    //

    LxtCheckErrno(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)));
    ReadBuffer[BytesReadWrite] = '\0';
    LxtLogInfo("Echo received by master(FD:%d):%s", PtmFd, ReadBuffer);
    LxtLogInfo("Last character = %d [\\n = %d, \\r = %d]", ReadBuffer[BytesReadWrite - 1], '\n', '\r');
    LxtCheckFnResults("read", BytesReadWrite, (ExpectedResult + 1));
    if ((ReadBuffer[BytesReadWrite - 1] != '\n') || (ReadBuffer[BytesReadWrite - 2] != '\r'))
    {
        LxtLogError("Echo to master(FD:%d) does not end with \r\n.", PtmFd);

        Result = -1;
        goto ErrorExit;
    }

    ReadBuffer[BytesReadWrite - 2] = '\n';
    if (memcmp(ReadBuffer, Utf8String, ExpectedResult) != 0)
    {
        LxtLogError(
            "Echo to master(FD:%d) does not match what was "
            "written.",
            PtmFd);

        Result = -1;
        goto ErrorExit;
    }

    //
    // Read from subordinate.
    //

    memset(ReadBuffer, 0, sizeof(ReadBuffer));
    LxtLogInfo("Reading from subordinate");
    LxtCheckErrno(BytesReadWrite = read(PtsFd, ReadBuffer, sizeof(ReadBuffer)));
    LxtLogInfo("Message received by subordinate(FD:%d):%s", PtsFd, ReadBuffer);
    LxtLogInfo("Last character = %d [\\n = %d, \\r = %d]", ReadBuffer[BytesReadWrite - 1], '\n', '\r');
    LxtCheckFnResults("read", BytesReadWrite, ExpectedResult);

    //
    // Compare the messages.
    //

    if (memcmp(ReadBuffer, Utf8String, BytesReadWrite) != 0)
    {
        LxtLogError(
            "Data read from subordinate(FD:%d) does not match what was "
            "written by master(FD:%d).",
            PtsFd,
            PtmFd);

        Result = -1;
        goto ErrorExit;
    }

    Result = 0;

ErrorExit:
    if (PtmFd != -1)
    {
        close(PtmFd);
    }

    if (PtsFd != -1)
    {
        close(PtsFd);
    }

    return Result;
}

int PtUTF8Basic5(PLXT_ARGS Args)

/*++

Routine Description:

    This routine sends part of a UTF8 character.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    ssize_t BytesReadWrite;
    ssize_t ExpectedResult;
    tcflag_t InputFlags;
    int PtmFd;
    int PtsFd;
    char ReadBuffer[10];
    int Result;
    int SerialNumber;
    const char* Utf8String = "\xE2\x82\xAC\n";

    //
    // Initialize locals
    //

    PtmFd = -1;
    PtsFd = -1;

    //
    // Open Master-Subordinate
    //

    LxtCheckErrno(OpenMasterSubordinate(&PtmFd, &PtsFd, NULL, &SerialNumber));
    LxtLogInfo("Master opened at FD:%d", PtmFd);
    LxtLogInfo("Subordinate Serial Number: %d", SerialNumber);
    LxtLogInfo("Subordinate opened at FD:%d", PtsFd);

    //
    // Write the first byte of the UTF-8 characters to the master.
    //

    ExpectedResult = strlen(Utf8String);
    LxtLogInfo("Writing to master");
    LxtCheckErrno(BytesReadWrite = write(PtmFd, Utf8String, 1));
    LxtCheckFnResults("write", BytesReadWrite, 1);

    //
    // Check that the byte has been echoed back.
    //

    LxtCheckErrno(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)));
    LxtCheckFnResults("read", BytesReadWrite, 1);
    LxtCheckEqual(Utf8String[0], ReadBuffer[0], "%hhd");

    //
    // Write the remaining bytes of the UTF-8 characters to the master.
    //

    LxtLogInfo("Writing to master");
    LxtCheckErrno(BytesReadWrite = write(PtmFd, &Utf8String[1], (ExpectedResult - 1)));
    LxtCheckFnResults("write", BytesReadWrite, (ExpectedResult - 1));

    //
    // Canonical mode should echo the input back to the master with a
    // carriage-return and newline.
    //

    LxtCheckErrno(BytesReadWrite = read(PtmFd, &ReadBuffer[1], (sizeof(ReadBuffer) - 1)));
    BytesReadWrite += 1;
    ReadBuffer[BytesReadWrite] = '\0';
    LxtLogInfo("Echo received by master(FD:%d):%s", PtmFd, ReadBuffer);
    LxtLogInfo("Last character = %d [\\n = %d, \\r = %d]", ReadBuffer[BytesReadWrite - 1], '\n', '\r');
    LxtCheckFnResults("read", BytesReadWrite, (ExpectedResult + 1));
    if ((ReadBuffer[BytesReadWrite - 1] != '\n') || (ReadBuffer[BytesReadWrite - 2] != '\r'))
    {
        LxtLogError("Echo to master(FD:%d) does not end with \r\n.", PtmFd);

        Result = -1;
        goto ErrorExit;
    }

    ReadBuffer[BytesReadWrite - 2] = '\n';
    if (memcmp(ReadBuffer, Utf8String, ExpectedResult) != 0)
    {
        LxtLogError(
            "Echo to master(FD:%d) does not match what was "
            "written.",
            PtmFd);

        Result = -1;
        goto ErrorExit;
    }

    //
    // Read from subordinate.
    //

    memset(ReadBuffer, 0, sizeof(ReadBuffer));
    LxtLogInfo("Reading from subordinate");
    LxtCheckErrno(BytesReadWrite = read(PtsFd, ReadBuffer, sizeof(ReadBuffer)));
    LxtLogInfo("Message received by subordinate(FD:%d):%s", PtsFd, ReadBuffer);
    LxtLogInfo("Last character = %d [\\n = %d, \\r = %d]", ReadBuffer[BytesReadWrite - 1], '\n', '\r');
    LxtCheckFnResults("read", BytesReadWrite, ExpectedResult);

    //
    // Compare the messages.
    //

    if (memcmp(ReadBuffer, Utf8String, BytesReadWrite) != 0)
    {
        LxtLogError(
            "Data read from subordinate(FD:%d) does not match what was "
            "written by master(FD:%d).",
            PtsFd,
            PtmFd);

        Result = -1;
        goto ErrorExit;
    }

    Result = 0;

ErrorExit:
    if (PtmFd != -1)
    {
        close(PtmFd);
    }

    if (PtsFd != -1)
    {
        close(PtsFd);
    }

    return Result;
}

int PtUTF8Basic6(PLXT_ARGS Args)

/*++

Routine Description:

    This routine turns on UTF8 mode and sends part of a UTF8 character.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    ssize_t BytesReadWrite;
    ssize_t ExpectedResult;
    tcflag_t InputFlags;
    int PtmFd;
    int PtsFd;
    char ReadBuffer[10];
    int Result;
    int SerialNumber;
    const char* Utf8String = "\xE2\x82\xAC\n";

    //
    // Initialize locals
    //

    PtmFd = -1;
    PtsFd = -1;

    //
    // Open Master-Subordinate
    //

    LxtCheckErrno(OpenMasterSubordinate(&PtmFd, &PtsFd, NULL, &SerialNumber));
    LxtLogInfo("Master opened at FD:%d", PtmFd);
    LxtLogInfo("Subordinate Serial Number: %d", SerialNumber);
    LxtLogInfo("Subordinate opened at FD:%d", PtsFd);

    //
    // Set UTF-8 mode.
    //

    LxtCheckResult(TerminalSettingsGetInputFlags(PtsFd, &InputFlags));
    LxtCheckResult(TerminalSettingsSetInputFlags(PtsFd, InputFlags | IUTF8));

    //
    // Write the first byte of the UTF-8 characters to the master.
    //

    ExpectedResult = strlen(Utf8String);
    LxtLogInfo("Writing to master");
    LxtCheckErrno(BytesReadWrite = write(PtmFd, Utf8String, 1));
    LxtCheckFnResults("write", BytesReadWrite, 1);

    //
    // Check that the byte has been echoed back.
    //

    LxtCheckErrno(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)));
    LxtCheckFnResults("read", BytesReadWrite, 1);
    LxtCheckEqual(Utf8String[0], ReadBuffer[0], "%hhd");

    //
    // Write the remaining bytes of the UTF-8 characters to the master.
    //

    LxtLogInfo("Writing to master");
    LxtCheckErrno(BytesReadWrite = write(PtmFd, &Utf8String[1], (ExpectedResult - 1)));
    LxtCheckFnResults("write", BytesReadWrite, (ExpectedResult - 1));

    //
    // Canonical mode should echo the input back to the master with a
    // carriage-return and newline.
    //

    LxtCheckErrno(BytesReadWrite = read(PtmFd, &ReadBuffer[1], (sizeof(ReadBuffer) - 1)));
    BytesReadWrite += 1;
    ReadBuffer[BytesReadWrite] = '\0';
    LxtLogInfo("Echo received by master(FD:%d):%s", PtmFd, ReadBuffer);
    LxtLogInfo("Last character = %d [\\n = %d, \\r = %d]", ReadBuffer[BytesReadWrite - 1], '\n', '\r');
    LxtCheckFnResults("read", BytesReadWrite, (ExpectedResult + 1));
    if ((ReadBuffer[BytesReadWrite - 1] != '\n') || (ReadBuffer[BytesReadWrite - 2] != '\r'))
    {
        LxtLogError("Echo to master(FD:%d) does not end with \r\n.", PtmFd);

        Result = -1;
        goto ErrorExit;
    }

    ReadBuffer[BytesReadWrite - 2] = '\n';
    if (memcmp(ReadBuffer, Utf8String, ExpectedResult) != 0)
    {
        LxtLogError(
            "Echo to master(FD:%d) does not match what was "
            "written.",
            PtmFd);

        Result = -1;
        goto ErrorExit;
    }

    //
    // Read from subordinate.
    //

    memset(ReadBuffer, 0, sizeof(ReadBuffer));
    LxtLogInfo("Reading from subordinate");
    LxtCheckErrno(BytesReadWrite = read(PtsFd, ReadBuffer, sizeof(ReadBuffer)));
    LxtLogInfo("Message received by subordinate(FD:%d):%s", PtsFd, ReadBuffer);
    LxtLogInfo("Last character = %d [\\n = %d, \\r = %d]", ReadBuffer[BytesReadWrite - 1], '\n', '\r');
    LxtCheckFnResults("read", BytesReadWrite, ExpectedResult);

    //
    // Compare the messages.
    //

    if (memcmp(ReadBuffer, Utf8String, BytesReadWrite) != 0)
    {
        LxtLogError(
            "Data read from subordinate(FD:%d) does not match what was "
            "written by master(FD:%d).",
            PtsFd,
            PtmFd);

        Result = -1;
        goto ErrorExit;
    }

    Result = 0;

ErrorExit:
    if (PtmFd != -1)
    {
        close(PtmFd);
    }

    if (PtsFd != -1)
    {
        close(PtsFd);
    }

    return Result;
}

int PtUTF8Basic7(PLXT_ARGS Args)

/*++

Routine Description:

    This routine sends a string ending with a UTF-8 character, followed by the
    delete char. This is expected to remove only a single byte from the string.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    ssize_t BytesReadWrite;
    cc_t ControlArray[NCCS];
    ssize_t ExpectedResult;
    int PtmFd;
    int PtsFd;
    char ReadBuffer[10];
    int Result;
    int SerialNumber;
    const char* Utf8String = "hello\xE2\x82\xAC";
    const char* Utf8StringFinal = "hello\xE2\x82\n";
    char EndString[3] = {0, '\n', '\0'};
    const char* EndStringEcho = "\x8 \x8\r\n";

    //
    // Initialize locals
    //

    PtmFd = -1;
    PtsFd = -1;

    //
    // Open Master-Subordinate
    //

    LxtCheckErrno(OpenMasterSubordinate(&PtmFd, &PtsFd, NULL, &SerialNumber));
    LxtLogInfo("Master opened at FD:%d", PtmFd);
    LxtLogInfo("Subordinate Serial Number: %d", SerialNumber);
    LxtLogInfo("Subordinate opened at FD:%d", PtsFd);

    //
    // Do NOT set UTF-8 mode for this test.
    //

    //
    // Fetch special characters.
    //

    LxtCheckResult(TerminalSettingsGetControlArray(PtsFd, ControlArray));
    EndString[0] = ControlArray[VERASE];

    //
    // Write non-terminated string to the master.
    //

    LxtLogInfo("Writing to master");
    ExpectedResult = strlen(Utf8String);
    LxtCheckErrno(BytesReadWrite = write(PtmFd, Utf8String, ExpectedResult));
    LxtCheckFnResults("write", BytesReadWrite, ExpectedResult);
    LxtLogInfo("Master(FD:%d) --> subordinate(FD:%d):%*s", PtmFd, PtsFd, BytesReadWrite, Utf8String);

    //
    // Canonical mode should echo the input back to the master.
    //

    LxtCheckErrno(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)));
    ReadBuffer[BytesReadWrite] = '\0';
    LxtLogInfo("Echo received by master(FD:%d):%s", PtmFd, ReadBuffer);
    LxtCheckFnResults("read", BytesReadWrite, ExpectedResult);
    if (memcmp(ReadBuffer, Utf8String, ExpectedResult) != 0)
    {
        LxtLogError(
            "Echo to master(FD:%d) does not match what was "
            "written.",
            PtmFd);

        Result = -1;
        goto ErrorExit;
    }

    //
    // Now send delete character followed by the newline.
    //

    LxtLogInfo("Writing to master");
    ExpectedResult = strlen(EndString);
    LxtCheckErrno(BytesReadWrite = write(PtmFd, EndString, ExpectedResult));
    LxtCheckFnResults("write", BytesReadWrite, ExpectedResult);
    LxtLogInfo("Master(FD:%d) --> subordinate(FD:%d):%*s", PtmFd, PtsFd, BytesReadWrite, EndString);

    //
    // Canonical mode should echo the input back to the master.
    //

    ExpectedResult = strlen(EndStringEcho);
    LxtCheckErrno(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)));
    ReadBuffer[BytesReadWrite] = '\0';
    LxtLogInfo("Echo received by master(FD:%d):%s", PtmFd, ReadBuffer);
    LxtCheckFnResults("read", BytesReadWrite, ExpectedResult);
    if (memcmp(ReadBuffer, EndStringEcho, ExpectedResult) != 0)
    {
        LxtLogError("Echo to master(FD:%d) does not match expected value.", PtmFd);

        Result = -1;
        goto ErrorExit;
    }

    //
    // Read the message from the subordinate.
    //

    LxtLogInfo("Reading from subordinate");
    ExpectedResult = strlen(Utf8StringFinal);
    LxtCheckErrno(BytesReadWrite = read(PtsFd, ReadBuffer, sizeof(ReadBuffer)));
    LxtLogInfo("Message received by subordinate(FD:%d):%s", PtsFd, ReadBuffer);
    LxtCheckFnResults("read", BytesReadWrite, ExpectedResult);

    //
    // Compare the messages.
    //

    if (memcmp(ReadBuffer, Utf8StringFinal, BytesReadWrite) != 0)
    {
        LxtLogError(
            "Data read from subordinate(FD:%d) does not match what was "
            "written by master(FD:%d).",
            PtsFd,
            PtmFd);

        Result = -1;
        goto ErrorExit;
    }

    Result = 0;

ErrorExit:
    if (PtmFd != -1)
    {
        close(PtmFd);
    }

    if (PtsFd != -1)
    {
        close(PtsFd);
    }

    return Result;
}

int PtUTF8Basic8(PLXT_ARGS Args)

/*++

Routine Description:

    This routine turns on UTF8 mode and then sends a string ending with a UTF-8
    character. Then it sends the delete char. This is expected to remove all of
    the bytes from the UTF-8 character.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    ssize_t BytesReadWrite;
    cc_t ControlArray[NCCS];
    ssize_t ExpectedResult;
    tcflag_t InputFlags;
    int PtmFd;
    int PtsFd;
    char ReadBuffer[10];
    int Result;
    int SerialNumber;
    const char* Utf8String = "hello\xE2\x82\xAC";
    const char* Utf8StringFinal = "hello\n";
    char EndString[3] = {0, '\n', '\0'};
    const char* EndStringEcho = "\x8 \x8\r\n";

    //
    // Initialize locals
    //

    PtmFd = -1;
    PtsFd = -1;

    //
    // Open Master-Subordinate
    //

    LxtCheckErrno(OpenMasterSubordinate(&PtmFd, &PtsFd, NULL, &SerialNumber));
    LxtLogInfo("Master opened at FD:%d", PtmFd);
    LxtLogInfo("Subordinate Serial Number: %d", SerialNumber);
    LxtLogInfo("Subordinate opened at FD:%d", PtsFd);

    //
    // Set UTF-8 mode.
    //

    LxtCheckResult(TerminalSettingsGetInputFlags(PtsFd, &InputFlags));
    LxtCheckResult(TerminalSettingsSetInputFlags(PtsFd, InputFlags | IUTF8));

    //
    // Fetch special characters.
    //

    LxtCheckResult(TerminalSettingsGetControlArray(PtsFd, ControlArray));
    EndString[0] = ControlArray[VERASE];

    //
    // Write non-terminated string to the master.
    //

    LxtLogInfo("Writing to master");
    ExpectedResult = strlen(Utf8String);
    LxtCheckErrno(BytesReadWrite = write(PtmFd, Utf8String, ExpectedResult));
    LxtCheckFnResults("write", BytesReadWrite, ExpectedResult);
    LxtLogInfo("Master(FD:%d) --> subordinate(FD:%d):%*s", PtmFd, PtsFd, BytesReadWrite, Utf8String);

    //
    // Canonical mode should echo the input back to the master.
    //

    LxtCheckErrno(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)));
    ReadBuffer[BytesReadWrite] = '\0';
    LxtLogInfo("Echo received by master(FD:%d):%s", PtmFd, ReadBuffer);
    LxtCheckFnResults("read", BytesReadWrite, ExpectedResult);
    if (memcmp(ReadBuffer, Utf8String, ExpectedResult) != 0)
    {
        LxtLogError(
            "Echo to master(FD:%d) does not match what was "
            "written.",
            PtmFd);

        Result = -1;
        goto ErrorExit;
    }

    //
    // Now send delete character followed by the newline.
    //

    LxtLogInfo("Writing to master");
    ExpectedResult = strlen(EndString);
    LxtCheckErrno(BytesReadWrite = write(PtmFd, EndString, ExpectedResult));
    LxtCheckFnResults("write", BytesReadWrite, ExpectedResult);
    LxtLogInfo("Master(FD:%d) --> subordinate(FD:%d):%*s", PtmFd, PtsFd, BytesReadWrite, EndString);

    //
    // Canonical mode should echo the input back to the master.
    //

    ExpectedResult = strlen(EndStringEcho);
    LxtCheckErrno(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)));
    ReadBuffer[BytesReadWrite] = '\0';
    LxtLogInfo("Echo received by master(FD:%d):%s", PtmFd, ReadBuffer);
    LxtCheckFnResults("read", BytesReadWrite, ExpectedResult);
    if (memcmp(ReadBuffer, EndStringEcho, ExpectedResult) != 0)
    {
        LxtLogError("Echo to master(FD:%d) does not match expected value.", PtmFd);

        Result = -1;
        goto ErrorExit;
    }

    //
    // Read the message from the subordinate.
    //

    LxtLogInfo("Reading from subordinate");
    ExpectedResult = strlen(Utf8StringFinal);
    LxtCheckErrno(BytesReadWrite = read(PtsFd, ReadBuffer, sizeof(ReadBuffer)));
    LxtLogInfo("Message received by subordinate(FD:%d):%s", PtsFd, ReadBuffer);
    LxtCheckFnResults("read", BytesReadWrite, ExpectedResult);

    //
    // Compare the messages.
    //

    if (memcmp(ReadBuffer, Utf8StringFinal, BytesReadWrite) != 0)
    {
        LxtLogError(
            "Data read from subordinate(FD:%d) does not match what was "
            "written by master(FD:%d).",
            PtsFd,
            PtmFd);

        Result = -1;
        goto ErrorExit;
    }

    Result = 0;

ErrorExit:
    if (PtmFd != -1)
    {
        close(PtmFd);
    }

    if (PtsFd != -1)
    {
        close(PtsFd);
    }

    return Result;
}

int PtUTF8Malformed(PLXT_ARGS Args)

/*++

Routine Description:

    This routine turns on UTF8 mode and then sends a string ending with a
    malformed UTF-8 character. Then it sends the delete char.

    The observed behavior is to do a simple removal of all bytes beginning with
    0b10 plus one more which for a real UTF-8 character would be the beginning
    byte.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    ssize_t BytesReadWrite;
    cc_t ControlArray[NCCS];
    ssize_t ExpectedResult;
    tcflag_t InputFlags;
    int PtmFd;
    int PtsFd;
    char ReadBuffer[15];
    int Result;
    int SerialNumber;
    const char* Utf8String = "howdy\x80\x80\x80\x80\x80\x80";
    const char* Utf8StringFinal = "howd\n";
    char EndString[3] = {0, '\n', '\0'};
    const char* EndStringEcho = "\x8 \x8\r\n";

    //
    // Initialize locals
    //

    PtmFd = -1;
    PtsFd = -1;

    //
    // Open Master-Subordinate
    //

    LxtCheckErrno(OpenMasterSubordinate(&PtmFd, &PtsFd, NULL, &SerialNumber));
    LxtLogInfo("Master opened at FD:%d", PtmFd);
    LxtLogInfo("Subordinate Serial Number: %d", SerialNumber);
    LxtLogInfo("Subordinate opened at FD:%d", PtsFd);

    //
    // Set UTF-8 mode.
    //

    LxtCheckResult(TerminalSettingsGetInputFlags(PtsFd, &InputFlags));
    LxtCheckResult(TerminalSettingsSetInputFlags(PtsFd, InputFlags | IUTF8));

    //
    // Fetch special characters.
    //

    LxtCheckResult(TerminalSettingsGetControlArray(PtsFd, ControlArray));
    EndString[0] = ControlArray[VERASE];

    //
    // Write non-terminated string to the master.
    //

    LxtLogInfo("Writing to master");
    ExpectedResult = strlen(Utf8String);
    LxtCheckErrno(BytesReadWrite = write(PtmFd, Utf8String, ExpectedResult));
    LxtCheckFnResults("write", BytesReadWrite, ExpectedResult);
    LxtLogInfo("Master(FD:%d) --> subordinate(FD:%d):%*s", PtmFd, PtsFd, BytesReadWrite, Utf8String);

    //
    // Canonical mode should echo the input back to the master.
    //

    LxtCheckErrno(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)));
    ReadBuffer[BytesReadWrite] = '\0';
    LxtLogInfo("Echo received by master(FD:%d):%s", PtmFd, ReadBuffer);
    LxtCheckFnResults("read", BytesReadWrite, ExpectedResult);
    if (memcmp(ReadBuffer, Utf8String, ExpectedResult) != 0)
    {
        LxtLogError(
            "Echo to master(FD:%d) does not match what was "
            "written.",
            PtmFd);

        Result = -1;
        goto ErrorExit;
    }

    //
    // Now send delete character followed by the newline.
    //

    LxtLogInfo("Writing to master");
    ExpectedResult = strlen(EndString);
    LxtCheckErrno(BytesReadWrite = write(PtmFd, EndString, ExpectedResult));
    LxtCheckFnResults("write", BytesReadWrite, ExpectedResult);
    LxtLogInfo("Master(FD:%d) --> subordinate(FD:%d):%*s", PtmFd, PtsFd, BytesReadWrite, EndString);

    //
    // Canonical mode should echo the input back to the master.
    //

    ExpectedResult = strlen(EndStringEcho);
    LxtCheckErrno(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)));
    ReadBuffer[BytesReadWrite] = '\0';
    LxtLogInfo("Echo received by master(FD:%d):%s", PtmFd, ReadBuffer);
    LxtCheckFnResults("read", BytesReadWrite, ExpectedResult);
    if (memcmp(ReadBuffer, EndStringEcho, ExpectedResult) != 0)
    {
        LxtLogError("Echo to master(FD:%d) does not match expected value.", PtmFd);

        Result = -1;
        goto ErrorExit;
    }

    //
    // Read the message from the subordinate.
    //

    LxtLogInfo("Reading from subordinate");
    ExpectedResult = strlen(Utf8StringFinal);
    LxtCheckErrno(BytesReadWrite = read(PtsFd, ReadBuffer, sizeof(ReadBuffer)));
    LxtLogInfo("Message received by subordinate(FD:%d):%s", PtsFd, ReadBuffer);
    LxtCheckFnResults("read", BytesReadWrite, ExpectedResult);

    //
    // Compare the messages.
    //

    if (memcmp(ReadBuffer, Utf8StringFinal, BytesReadWrite) != 0)
    {
        LxtLogError(
            "Data read from subordinate(FD:%d) does not match what was "
            "written by master(FD:%d).",
            PtsFd,
            PtmFd);

        Result = -1;
        goto ErrorExit;
    }

    Result = 0;

ErrorExit:
    if (PtmFd != -1)
    {
        close(PtmFd);
    }

    if (PtsFd != -1)
    {
        close(PtsFd);
    }

    return Result;
}

int PtUTF8Malformed2(PLXT_ARGS Args)

/*++

Routine Description:

    This routine turns on UTF8 mode and then sends a string ending with a
    malformed UTF-8 character. Then it sends the delete char.

    The observed behavior is to do a simple removal of all bytes beginning with
    0b10 plus one more which for a real UTF-8 character would be the beginning
    byte.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    ssize_t BytesReadWrite;
    cc_t ControlArray[NCCS];
    ssize_t ExpectedResult;
    tcflag_t InputFlags;
    int PtmFd;
    int PtsFd;
    char ReadBuffer[15];
    int Result;
    int SerialNumber;
    const char* Utf8String = "howdy\x80\x80\x80\x80\x80\xf0";
    const char* Utf8StringFinal = "howdy\x80\x80\x80\x80\x80\n";
    char EndString[3] = {0, '\n', '\0'};
    const char* EndStringEcho = "\x8 \x8\r\n";

    //
    // Initialize locals
    //

    PtmFd = -1;
    PtsFd = -1;

    //
    // Open Master-Subordinate
    //

    LxtCheckErrno(OpenMasterSubordinate(&PtmFd, &PtsFd, NULL, &SerialNumber));
    LxtLogInfo("Master opened at FD:%d", PtmFd);
    LxtLogInfo("Subordinate Serial Number: %d", SerialNumber);
    LxtLogInfo("Subordinate opened at FD:%d", PtsFd);

    //
    // Set UTF-8 mode.
    //

    LxtCheckResult(TerminalSettingsGetInputFlags(PtsFd, &InputFlags));
    LxtCheckResult(TerminalSettingsSetInputFlags(PtsFd, InputFlags | IUTF8));

    //
    // Fetch special characters.
    //

    LxtCheckResult(TerminalSettingsGetControlArray(PtsFd, ControlArray));
    EndString[0] = ControlArray[VERASE];

    //
    // Write non-terminated string to the master.
    //

    LxtLogInfo("Writing to master");
    ExpectedResult = strlen(Utf8String);
    LxtCheckErrno(BytesReadWrite = write(PtmFd, Utf8String, ExpectedResult));
    LxtCheckFnResults("write", BytesReadWrite, ExpectedResult);
    LxtLogInfo("Master(FD:%d) --> subordinate(FD:%d):%*s", PtmFd, PtsFd, BytesReadWrite, Utf8String);

    //
    // Canonical mode should echo the input back to the master.
    //

    LxtCheckErrno(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)));
    ReadBuffer[BytesReadWrite] = '\0';
    LxtLogInfo("Echo received by master(FD:%d):%s", PtmFd, ReadBuffer);
    LxtCheckFnResults("read", BytesReadWrite, ExpectedResult);
    if (memcmp(ReadBuffer, Utf8String, ExpectedResult) != 0)
    {
        LxtLogError(
            "Echo to master(FD:%d) does not match what was "
            "written.",
            PtmFd);

        Result = -1;
        goto ErrorExit;
    }

    //
    // Now send delete character followed by the newline.
    //

    LxtLogInfo("Writing to master");
    ExpectedResult = strlen(EndString);
    LxtCheckErrno(BytesReadWrite = write(PtmFd, EndString, ExpectedResult));
    LxtCheckFnResults("write", BytesReadWrite, ExpectedResult);
    LxtLogInfo("Master(FD:%d) --> subordinate(FD:%d):%*s", PtmFd, PtsFd, BytesReadWrite, EndString);

    //
    // Canonical mode should echo the input back to the master.
    //

    ExpectedResult = strlen(EndStringEcho);
    LxtCheckErrno(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)));
    ReadBuffer[BytesReadWrite] = '\0';
    LxtLogInfo("Echo received by master(FD:%d):%s", PtmFd, ReadBuffer);
    LxtCheckFnResults("read", BytesReadWrite, ExpectedResult);
    if (memcmp(ReadBuffer, EndStringEcho, ExpectedResult) != 0)
    {
        LxtLogError("Echo to master(FD:%d) does not match expected value.", PtmFd);

        Result = -1;
        goto ErrorExit;
    }

    //
    // Read the message from the subordinate.
    //

    LxtLogInfo("Reading from subordinate");
    ExpectedResult = strlen(Utf8StringFinal);
    LxtCheckErrno(BytesReadWrite = read(PtsFd, ReadBuffer, sizeof(ReadBuffer)));
    LxtLogInfo("Message received by subordinate(FD:%d):%s", PtsFd, ReadBuffer);
    LxtCheckFnResults("read", BytesReadWrite, ExpectedResult);

    //
    // Compare the messages.
    //

    if (memcmp(ReadBuffer, Utf8StringFinal, BytesReadWrite) != 0)
    {
        LxtLogError(
            "Data read from subordinate(FD:%d) does not match what was "
            "written by master(FD:%d).",
            PtsFd,
            PtmFd);

        Result = -1;
        goto ErrorExit;
    }

    Result = 0;

ErrorExit:
    if (PtmFd != -1)
    {
        close(PtmFd);
    }

    if (PtsFd != -1)
    {
        close(PtsFd);
    }

    return Result;
}

int PtUTF8Malformed3(PLXT_ARGS Args)

/*++

Routine Description:

    This routine turns on UTF8 mode and then sends two strings, the last
    consisting of only a malformed UTF-8 character. Then it sends the delete
    char.

    The observed behavior is to do a simple removal of all bytes
    beginning with 0b10 until it hits the beginning of the line. Apparently
    treating this as an error, no echo is done.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    ssize_t BytesReadWrite;
    cc_t ControlArray[NCCS];
    ssize_t ExpectedResult;
    tcflag_t InputFlags;
    int PtmFd;
    int PtsFd;
    char ReadBuffer[15];
    int Result;
    int SerialNumber;
    const char* Utf8String = "howdy\n\x80\x80";
    const char* Utf8StringEcho = "howdy\r\n\x80\x80";
    const char* Utf8StringFinal = "howdy\n";
    char EndString[3] = {0, '\n', '\0'};
    const char* EndStringEcho = "\r\n";

    //
    // Initialize locals
    //

    PtmFd = -1;
    PtsFd = -1;

    //
    // Open Master-Subordinate
    //

    LxtCheckErrno(OpenMasterSubordinate(&PtmFd, &PtsFd, NULL, &SerialNumber));
    LxtLogInfo("Master opened at FD:%d", PtmFd);
    LxtLogInfo("Subordinate Serial Number: %d", SerialNumber);
    LxtLogInfo("Subordinate opened at FD:%d", PtsFd);

    //
    // Set UTF-8 mode.
    //

    LxtCheckResult(TerminalSettingsGetInputFlags(PtsFd, &InputFlags));
    LxtCheckResult(TerminalSettingsSetInputFlags(PtsFd, InputFlags | IUTF8));

    //
    // Fetch special characters.
    //

    LxtCheckResult(TerminalSettingsGetControlArray(PtsFd, ControlArray));
    EndString[0] = ControlArray[VERASE];

    //
    // Write non-terminated string to the master.
    //

    LxtLogInfo("Writing to master");
    ExpectedResult = strlen(Utf8String);
    LxtCheckErrno(BytesReadWrite = write(PtmFd, Utf8String, ExpectedResult));
    LxtCheckFnResults("write", BytesReadWrite, ExpectedResult);
    LxtLogInfo("Master(FD:%d) --> subordinate(FD:%d):%*s", PtmFd, PtsFd, BytesReadWrite, Utf8String);

    //
    // Canonical mode should echo the input back to the master.
    //

    ExpectedResult = strlen(Utf8StringEcho);
    LxtCheckErrno(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)));
    ReadBuffer[BytesReadWrite] = '\0';
    LxtLogInfo("Echo received by master(FD:%d):%s", PtmFd, ReadBuffer);
    LxtCheckFnResults("read", BytesReadWrite, ExpectedResult);
    if (memcmp(ReadBuffer, Utf8StringEcho, ExpectedResult) != 0)
    {
        LxtLogError(
            "Echo to master(FD:%d) does not match what was "
            "written.",
            PtmFd);

        Result = -1;
        goto ErrorExit;
    }

    //
    // Now send delete character followed by the newline.
    //

    LxtLogInfo("Writing to master");
    ExpectedResult = strlen(EndString);
    LxtCheckErrno(BytesReadWrite = write(PtmFd, EndString, ExpectedResult));
    LxtCheckFnResults("write", BytesReadWrite, ExpectedResult);
    LxtLogInfo("Master(FD:%d) --> subordinate(FD:%d):%*s", PtmFd, PtsFd, BytesReadWrite, EndString);

    //
    // Canonical mode should echo the input back to the master.
    //

    ExpectedResult = strlen(EndStringEcho);
    LxtCheckErrno(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)));
    ReadBuffer[BytesReadWrite] = '\0';
    LxtLogInfo("Echo received by master(FD:%d):%s", PtmFd, ReadBuffer);
    LxtCheckFnResults("read", BytesReadWrite, ExpectedResult);
    if (memcmp(ReadBuffer, EndStringEcho, ExpectedResult) != 0)
    {
        LxtLogError("Echo to master(FD:%d) does not match expected value.", PtmFd);

        Result = -1;
        goto ErrorExit;
    }

    //
    // Read the message from the subordinate.
    //

    LxtLogInfo("Reading from subordinate");
    ExpectedResult = strlen(Utf8StringFinal);
    LxtCheckErrno(BytesReadWrite = read(PtsFd, ReadBuffer, sizeof(ReadBuffer)));
    LxtLogInfo("Message received by subordinate(FD:%d):%s", PtsFd, ReadBuffer);
    LxtCheckFnResults("read", BytesReadWrite, ExpectedResult);

    //
    // Compare the messages.
    //

    if (memcmp(ReadBuffer, Utf8StringFinal, BytesReadWrite) != 0)
    {
        LxtLogError(
            "Data read from subordinate(FD:%d) does not match what was "
            "written by master(FD:%d).",
            PtsFd,
            PtmFd);

        Result = -1;
        goto ErrorExit;
    }

    Result = 0;

ErrorExit:
    if (PtmFd != -1)
    {
        close(PtmFd);
    }

    if (PtsFd != -1)
    {
        close(PtsFd);
    }

    return Result;
}

int PtUTF8Malformed4(PLXT_ARGS Args)

/*++

Routine Description:

    This routine turns on UTF8 mode and then sends a malformed UTF-8 character,
    followed by an erase character.

    The observed behavior is to do a simple removal of all bytes beginning with
    0b10 until it hits the beginning of the buffer. Apparently treating this as
    an error, it leaves the data unchanged and does no echo.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    ssize_t BytesReadWrite;
    cc_t ControlArray[NCCS];
    ssize_t ExpectedResult;
    tcflag_t InputFlags;
    int PtmFd;
    int PtsFd;
    char ReadBuffer[15];
    int Result;
    int SerialNumber;
    const char* Utf8String = "\x80\x80\x80\x80";
    const char* Utf8StringFinal = "\x80\x80\x80\x80\n";
    char EndString[3] = {0, '\n', '\0'};
    const char* EndStringEcho = "\r\n";

    //
    // Initialize locals
    //

    PtmFd = -1;
    PtsFd = -1;

    //
    // Open Master-Subordinate
    //

    LxtCheckErrno(OpenMasterSubordinate(&PtmFd, &PtsFd, NULL, &SerialNumber));
    LxtLogInfo("Master opened at FD:%d", PtmFd);
    LxtLogInfo("Subordinate Serial Number: %d", SerialNumber);
    LxtLogInfo("Subordinate opened at FD:%d", PtsFd);

    //
    // Set UTF-8 mode.
    //

    LxtCheckResult(TerminalSettingsGetInputFlags(PtsFd, &InputFlags));
    LxtCheckResult(TerminalSettingsSetInputFlags(PtsFd, InputFlags | IUTF8));

    //
    // Fetch special characters.
    //

    LxtCheckResult(TerminalSettingsGetControlArray(PtsFd, ControlArray));
    EndString[0] = ControlArray[VERASE];

    //
    // Write non-terminated string to the master.
    //

    LxtLogInfo("Writing to master");
    ExpectedResult = strlen(Utf8String);
    LxtCheckErrno(BytesReadWrite = write(PtmFd, Utf8String, ExpectedResult));
    LxtCheckFnResults("write", BytesReadWrite, ExpectedResult);
    LxtLogInfo("Master(FD:%d) --> subordinate(FD:%d):%*s", PtmFd, PtsFd, BytesReadWrite, Utf8String);

    //
    // Canonical mode should echo the input back to the master.
    //

    LxtCheckErrno(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)));
    ReadBuffer[BytesReadWrite] = '\0';
    LxtLogInfo("Echo received by master(FD:%d):%s", PtmFd, ReadBuffer);
    LxtCheckFnResults("read", BytesReadWrite, ExpectedResult);
    if (memcmp(ReadBuffer, Utf8String, ExpectedResult) != 0)
    {
        LxtLogError(
            "Echo to master(FD:%d) does not match what was "
            "written.",
            PtmFd);

        Result = -1;
        goto ErrorExit;
    }

    //
    // Now send delete character followed by the newline.
    //

    LxtLogInfo("Writing to master");
    ExpectedResult = strlen(EndString);
    LxtCheckErrno(BytesReadWrite = write(PtmFd, EndString, ExpectedResult));
    LxtCheckFnResults("write", BytesReadWrite, ExpectedResult);
    LxtLogInfo("Master(FD:%d) --> subordinate(FD:%d):%*s", PtmFd, PtsFd, BytesReadWrite, EndString);

    //
    // Canonical mode should echo the input back to the master.
    //

    ExpectedResult = strlen(EndStringEcho);
    LxtCheckErrno(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)));
    ReadBuffer[BytesReadWrite] = '\0';
    LxtLogInfo("Echo received by master(FD:%d):%s", PtmFd, ReadBuffer);
    LxtCheckFnResults("read", BytesReadWrite, ExpectedResult);
    if (memcmp(ReadBuffer, EndStringEcho, ExpectedResult) != 0)
    {
        LxtLogError("Echo to master(FD:%d) does not match expected value.", PtmFd);

        Result = -1;
        goto ErrorExit;
    }

    //
    // Read the message from the subordinate.
    //

    LxtLogInfo("Reading from subordinate");
    ExpectedResult = strlen(Utf8StringFinal);
    LxtCheckErrno(BytesReadWrite = read(PtsFd, ReadBuffer, sizeof(ReadBuffer)));
    LxtLogInfo("Message received by subordinate(FD:%d):%s", PtsFd, ReadBuffer);
    LxtCheckFnResults("read", BytesReadWrite, ExpectedResult);

    //
    // Compare the messages.
    //

    if (memcmp(ReadBuffer, Utf8StringFinal, BytesReadWrite) != 0)
    {
        LxtLogError(
            "Data read from subordinate(FD:%d) does not match what was "
            "written by master(FD:%d).",
            PtsFd,
            PtmFd);

        Result = -1;
        goto ErrorExit;
    }

    Result = 0;

ErrorExit:
    if (PtmFd != -1)
    {
        close(PtmFd);
    }

    if (PtsFd != -1)
    {
        close(PtsFd);
    }

    return Result;
}

int PtWriteNoSub1(PLXT_ARGS Args)

/*++

Routine Description:

    The PtWriteNoSubxxx validate the behavior of write on the master, when
    there are no open handles to the subordinate; where 'xxx' is the
    sub-test case as described below:
    1. A handle to the subordinate was never opened
       Expected Result: The write should succeed.
    2. A handle to subordinate was opened and closed and then the write
       on master was attempted.
       Expected Result: Same as (1) above.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    char Buffer;
    int BytesReadWrite;
    int PtmFd;
    int Result;

    //
    // Initialize locals
    //

    PtmFd = -1;

    //
    // Open Master.
    //

    LxtCheckErrno((PtmFd = open("/dev/ptmx", O_RDWR)));
    LxtLogInfo("Master opened at FD:%d", PtmFd);

    //
    // Now attempt a write on the master.
    //

    Buffer = 'a';
    LxtCheckErrno(BytesReadWrite = write(PtmFd, &Buffer, 1));

    //
    // write should have written 1-byte.
    //

    LxtCheckFnResults("write", BytesReadWrite, 1);

ErrorExit:
    if (PtmFd != -1)
    {
        close(PtmFd);
    }

    return Result;
}

int PtWriteNoSub2(PLXT_ARGS Args)

/*++

Routine Description:

    See PtWriteNoSub1 for details.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    char Buffer;
    int BytesReadWrite;
    int PtmFd;
    int PtsFd;
    int Result;
    int SerialNumber;

    //
    // Initialize locals
    //

    PtmFd = -1;
    PtsFd = -1;

    //
    // Open Master-Subordinate
    //

    LxtCheckErrno(OpenMasterSubordinate(&PtmFd, &PtsFd, NULL, &SerialNumber));
    LxtLogInfo("Master opened at FD:%d", PtmFd);
    LxtLogInfo("Subordinate Serial Number: %d", SerialNumber);
    LxtLogInfo("Subordinate opened at FD:%d", PtsFd);

    //
    // Close the subordinate.
    //

    LxtClose(PtsFd);
    LxtLogInfo("Subordinate closed");

    //
    // Now attempt a write on the master.
    //

    Buffer = 'a';
    LxtCheckErrno(BytesReadWrite = write(PtmFd, &Buffer, 1));

    //
    // write should have written 1-byte.
    //

    LxtCheckFnResults("write", BytesReadWrite, 1);

ErrorExit:
    if (PtmFd != -1)
    {
        close(PtmFd);
    }

    if (PtsFd != -1)
    {
        close(PtsFd);
    }

    return Result;
}

int PtWriteToSubReadFromMaster1(PLXT_ARGS Args)

/*++

Routine Description:

    This routine validates the scenario where the subordinate does one write of
    'n' bytes and the master does 'm' reads each of (n/m) bytes where n = m * x.
    Each read should return (n/m) bytes and the data read should line up with
    the data written.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int Itr;
    int PtmFd;
    int PtsFd;
    size_t ReadSizes[50];
    int Result;
    int SerialNumber;
    int TotalWriteSize;
    size_t WriteSizes[2];

    //
    // Initialize locals
    //

    PtmFd = -1;
    PtsFd = -1;

    //
    // Open Master-Subordinate
    //

    LxtCheckErrno(OpenMasterSubordinate(&PtmFd, &PtsFd, NULL, &SerialNumber));
    LxtCheckErrno(RawInit(PtsFd));
    LxtLogInfo("Master opened at FD:%d", PtmFd);
    LxtLogInfo("Subordinate Serial Number: %d", SerialNumber);
    LxtLogInfo("Subordinate opened at FD:%d", PtsFd);

    //
    // Write 'n' bytes and read 'n' bytes. This is equivalent to a simple write
    // read check.
    //

    WriteSizes[0] = 50;
    ReadSizes[0] = 50;
    LxtCheckErrno(WriteReadFdCommon(PtsFd, WriteSizes, 1, PtmFd, ReadSizes, 1));

    LxtLogInfo("Case 1 passed");

    //
    // Write 'n' bytes and do 2 reads each of 'n/2' bytes.
    //

    WriteSizes[0] = 50;
    ReadSizes[0] = 25;
    ReadSizes[1] = 25;
    LxtCheckErrno(WriteReadFdCommon(PtsFd, WriteSizes, 1, PtmFd, ReadSizes, 2));

    LxtLogInfo("Case 2 passed");

    //
    // Write 'n' bytes and do 'n/2' reads each of 2 bytes.
    //

    WriteSizes[0] = 50;
    for (Itr = 0; Itr < 25; Itr++)
    {
        ReadSizes[Itr] = 2;
    }

    LxtCheckErrno(WriteReadFdCommon(PtsFd, WriteSizes, 1, PtmFd, ReadSizes, 25));

    LxtLogInfo("Case 3 passed");

    //
    // Write 'n' bytes and do 'n' reads each of 1 byte.
    //

    WriteSizes[0] = 50;
    for (Itr = 0; Itr < 50; Itr++)
    {
        ReadSizes[Itr] = 1;
    }

    LxtCheckErrno(WriteReadFdCommon(PtsFd, WriteSizes, 1, PtmFd, ReadSizes, 50));

    LxtLogInfo("Case 4 passed");

    //
    // Do 2 writes 'n' and 'm' bytes and do several reads totalling to a size
    // of = (m+n) bytes.
    //

    WriteSizes[0] = 50;
    WriteSizes[1] = 10;
    ReadSizes[0] = 55;
    ReadSizes[1] = 5;
    LxtCheckErrno(WriteReadFdCommon(PtsFd, WriteSizes, 2, PtmFd, ReadSizes, 2));

    LxtLogInfo("Case 5 passed");
    WriteSizes[0] = 50;
    WriteSizes[1] = 10;
    ReadSizes[0] = 40;
    ReadSizes[1] = 5;
    ReadSizes[2] = 10;
    ReadSizes[3] = 3;
    ReadSizes[4] = 2;
    LxtCheckErrno(WriteReadFdCommon(PtsFd, WriteSizes, 2, PtmFd, ReadSizes, 5));

    LxtLogInfo("Case 6 passed");

ErrorExit:
    if (PtmFd != -1)
    {
        close(PtmFd);
    }

    if (PtsFd != -1)
    {
        close(PtsFd);
    }

    return Result;
}

int PtWindowSizeCheck(PLXT_ARGS Args)

/*++

Routine Description:

    This routine checks that window size can be read and set from both the
    master and terminal, and that a change in size delivers a SIGWINCH signal.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    pid_t ChildPid;
    int ChildStatus;
    int PtmFd;
    int PtsFd;
    int Result;
    struct winsize WindowSizeM;
    struct winsize WindowSizeS;

    //
    // Initialize locals
    //

    ChildPid = -1;
    PtmFd = -1;
    PtsFd = -1;

    LxtCheckErrno(ChildPid = ForkPty(&PtmFd, &PtsFd));
    if (ChildPid == 0)
    {
        LxtCheckResult(LxtSignalInitialize());
        LxtCheckResult(LxtSignalSetupHandler(SIGWINCH, SA_SIGINFO));

        //
        // Test the master endpoint.
        //

        LxtCheckErrno(ioctl(PtmFd, TIOCGWINSZ, &WindowSizeM));
        LxtCheckErrno(ioctl(PtmFd, TIOCSWINSZ, &WindowSizeM));
        LxtCheckResult(LxtSignalCheckNoSignal());
        WindowSizeM.ws_row -= 10;
        LxtCheckErrno(ioctl(PtmFd, TIOCSWINSZ, &WindowSizeM));
        LxtCheckResult(LxtSignalCheckReceived(SIGWINCH));
        LxtSignalResetReceived();

        //
        // Test the subordinate endpoint.
        //

        LxtCheckErrno(ioctl(PtsFd, TIOCGWINSZ, &WindowSizeS));
        LxtCheckMemoryEqual(&WindowSizeM, &WindowSizeS, sizeof(WindowSizeM));
        LxtCheckErrno(ioctl(PtsFd, TIOCSWINSZ, &WindowSizeS));
        LxtCheckResult(LxtSignalCheckNoSignal());
        WindowSizeS.ws_row -= 10;
        LxtCheckErrno(ioctl(PtsFd, TIOCSWINSZ, &WindowSizeS));
        LxtCheckResult(LxtSignalCheckReceived(SIGWINCH));
        LxtSignalResetReceived();
        LxtCheckErrno(ioctl(PtmFd, TIOCGWINSZ, &WindowSizeM));
        LxtCheckMemoryEqual(&WindowSizeM, &WindowSizeS, sizeof(WindowSizeM));

        Result = 0;
    }
    else
    {
        LxtCheckErrno(TEMP_FAILURE_RETRY(Result = waitpid(ChildPid, &ChildStatus, 0)));
        LxtCheckResult(WIFEXITED(ChildStatus) ? 0 : -1);
        LxtCheckResult((int)(char)WEXITSTATUS(ChildStatus));
    }

ErrorExit:
    if (PtmFd != -1)
    {
        close(PtmFd);
    }

    if (PtsFd != -1)
    {
        close(PtsFd);
    }

    if (ChildPid == 0)
    {
        exit(Result);
    }

    return Result;
}

void TestFun(void)
{
    char Buffer[50];
    int Result;

    LxtCheckErrno(GetRandomMessage(Buffer, sizeof(Buffer), FALSE));
    DumpBuffer(Buffer, sizeof(Buffer));

ErrorExit:
    return;
}
