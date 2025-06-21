/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    dev_pt_2.c

Abstract:

    This file is a test for the Pseudo Terminals: /dev/ptmx, /dev/pts/<n>
    devices.

--*/

#include "dev_pt_common.h"
#include <sys/mount.h>
#include <libmount/libmount.h>

//
// Globals.
//

#define LXT_NAME "dev_pt_2"
#define PTS_START_CONTROL_CHAR "^S"
#define PTS_STOP_CONTROL_CHAR "^Q"
#define PTS_TEST_MNT "/data/pts"

typedef struct _PT_THREAD_PARAMETERS
{
    pid_t ForegroundId;
    int PtmFd;
    int PtsFd;
    pid_t SessionId;
    PLXT_SYNCHRONIZATION_EVENT SynchronizationEventChild;
    PLXT_SYNCHRONIZATION_EVENT SynchronizationEventParent;
} PT_THREAD_PARAMETERS, *PPT_THREAD_PARAMETERS;

//
// Functions.
//

void* PtBackgroundDisassociateTty6Thread(PPT_THREAD_PARAMETERS ThreadParameters);

int PtBackgroundSwitchToForegroundWorker(bool UseMasterEndpoint);

//
// Test cases.
//

LXT_VARIATION_HANDLER PtBackgroundBasic;
LXT_VARIATION_HANDLER PtBackgroundBlockedSignals;
LXT_VARIATION_HANDLER PtBackgroundDisassociateTty1;
LXT_VARIATION_HANDLER PtBackgroundDisassociateTty2;
LXT_VARIATION_HANDLER PtBackgroundDisassociateTty3;
LXT_VARIATION_HANDLER PtBackgroundDisassociateTty4;
LXT_VARIATION_HANDLER PtBackgroundDisassociateTty5;
LXT_VARIATION_HANDLER PtBackgroundDisassociateTty6;
LXT_VARIATION_HANDLER PtBackgroundSwitchToForeground;
LXT_VARIATION_HANDLER PtBufferTerminalFill;
LXT_VARIATION_HANDLER PtControllingTerminalForeground;
LXT_VARIATION_HANDLER PtControllingTerminalForeground2;
LXT_VARIATION_HANDLER PtControllingTerminalForeground3;
LXT_VARIATION_HANDLER PtControllingTerminalForeground4;
LXT_VARIATION_HANDLER PtControllingTerminalForeground5;
LXT_VARIATION_HANDLER PtControllingTerminalForeground6;
LXT_VARIATION_HANDLER PtControllingTerminalForeground7;
LXT_VARIATION_HANDLER PtMountBasic;
LXT_VARIATION_HANDLER PtPacketBasic1;
LXT_VARIATION_HANDLER PtPacketBasic2;
LXT_VARIATION_HANDLER PtPacketBasic3;
LXT_VARIATION_HANDLER PtPacketBasic4;
LXT_VARIATION_HANDLER PtPacketToggleMode1;
LXT_VARIATION_HANDLER PtPacketToggleMode2;
LXT_VARIATION_HANDLER PtPacketToggleMode3;
LXT_VARIATION_HANDLER PtPacketToggleMode4;
LXT_VARIATION_HANDLER PtPacketToggleMode5;
LXT_VARIATION_HANDLER PtPacketToggleMode6;
LXT_VARIATION_HANDLER PtPacketToggleMode7;
LXT_VARIATION_HANDLER PtPacketFlushRead1;
LXT_VARIATION_HANDLER PtPacketFlushRead2;
LXT_VARIATION_HANDLER PtPacketFlushRead3;
LXT_VARIATION_HANDLER PtPacketFlushWrite1;
LXT_VARIATION_HANDLER PtPacketFlushWrite2;
LXT_VARIATION_HANDLER PtPacketFlushReadWrite1;
LXT_VARIATION_HANDLER PtPacketFlushReadWrite2;
LXT_VARIATION_HANDLER PtPacketFlushReadWrite3;
LXT_VARIATION_HANDLER PtPacketFlushReadWrite4;
LXT_VARIATION_HANDLER PtPacketFlushReadWrite5;
LXT_VARIATION_HANDLER PtPacketHangup;
LXT_VARIATION_HANDLER PtPacketControlCharCheck1;
LXT_VARIATION_HANDLER PtPacketControlCharCheck2;
LXT_VARIATION_HANDLER PtPacketControlCharCheck3;
LXT_VARIATION_HANDLER PtPacketToggleWithControlByte;
LXT_VARIATION_HANDLER PtSessionBasicMaster;
LXT_VARIATION_HANDLER PtSuspendOutput1;
LXT_VARIATION_HANDLER PtSuspendOutput2;
LXT_VARIATION_HANDLER PtSuspendOutput3;
LXT_VARIATION_HANDLER PtSuspendOutput4;
LXT_VARIATION_HANDLER PtSuspendOutput5;
LXT_VARIATION_HANDLER PtSuspendOutput6;
LXT_VARIATION_HANDLER PtSuspendOutput7;
LXT_VARIATION_HANDLER PtSuspendOutput8;
LXT_VARIATION_HANDLER PtSuspendOutput9;
LXT_VARIATION_HANDLER PtSuspendOutput10;
LXT_VARIATION_HANDLER PtSuspendOutput11;
LXT_VARIATION_HANDLER PtSuspendOutput12;
LXT_VARIATION_HANDLER PtSuspendOutput13;
LXT_VARIATION_HANDLER PtSuspendOutput14;
LXT_VARIATION_HANDLER PtSuspendOutput15;
LXT_VARIATION_HANDLER PtSuspendOutput16;

//
// Global constants
//

static const LXT_VARIATION g_LxtVariations[] = {
    {"Controlling terminal foreground tests", PtControllingTerminalForeground},
    {"Controlling terminal foreground tests (part 2)", PtControllingTerminalForeground2},
    {"Controlling terminal foreground tests (part 3)", PtControllingTerminalForeground3},
    {"Controlling terminal foreground tests (part 4)", PtControllingTerminalForeground4},
    {"Controlling terminal foreground tests (part 5)", PtControllingTerminalForeground5},
    {"Controlling terminal foreground tests (part 6)", PtControllingTerminalForeground6},
    {"Controlling terminal foreground tests (part 7)", PtControllingTerminalForeground7},
    {"Basic background IO", PtBackgroundBasic},
    {"Background IO with signals blocked", PtBackgroundBlockedSignals},
    {"Disassociate from a controlling terminal", PtBackgroundDisassociateTty1},
    {"Disassociate from a controlling terminal (part 2)", PtBackgroundDisassociateTty2},
    {"Disassociate from a controlling terminal (part 3)", PtBackgroundDisassociateTty3},
    {"Disassociate from a controlling terminal (part 4)", PtBackgroundDisassociateTty4},
    {"Disassociate from a controlling terminal (part 5)", PtBackgroundDisassociateTty5},
    {"Disassociate from a controlling terminal (part 6)", PtBackgroundDisassociateTty6},
    {"Background switching to foreground", PtBackgroundSwitchToForeground},

    //
    // TODO_LX: Implement master endpoint that can be a controlling terminal.
    //
    //{ "Session with basic controlling terminal IO (master endpoint)", PtSessionBasicMaster },
    //

    {"PT terminal buffer fill", PtBufferTerminalFill},
    {"PT basic mount verification", PtMountBasic},
    {"PT Basic packet-mode", PtPacketBasic1},
    {"PT Basic packet-mode (part 2)", PtPacketBasic2},
    {"PT Basic packet-mode (part 3)", PtPacketBasic3},
    {"PT Basic packet-mode (part 4)", PtPacketBasic4},
    {"PT toggle packet-mode", PtPacketToggleMode1},
    {"PT toggle packet-mode (part 2)", PtPacketToggleMode2},
    {"PT toggle packet-mode (part 3)", PtPacketToggleMode3},
    {"PT toggle packet-mode (part 4)", PtPacketToggleMode4},
    {"PT toggle packet-mode (part 5)", PtPacketToggleMode5},
    {"PT toggle packet-mode (part 6)", PtPacketToggleMode6},
    {"PT toggle packet-mode (part 7)", PtPacketToggleMode7},
    {"PT packet-mode flush read queue", PtPacketFlushRead1},
    {"PT packet-mode flush read queue (part 2)", PtPacketFlushRead2},
    {"PT packet-mode flush read queue (part 3)", PtPacketFlushRead3},
    {"PT packet-mode flush write queue", PtPacketFlushWrite1},
    {"PT packet-mode flush write queue (part 2)", PtPacketFlushWrite2},
    {"PT packet-mode flush read/write queue", PtPacketFlushReadWrite1},
    {"PT packet-mode flush read/write queue (part 2)", PtPacketFlushReadWrite2},
    {"PT packet-mode flush read/write queue (part 3)", PtPacketFlushReadWrite3},
    {"PT packet-mode flush read/write queue (part 4)", PtPacketFlushReadWrite4},
    {"PT packet-mode flush read/write queue (part 5)", PtPacketFlushReadWrite5},
    {"PT packet-mode hangup", PtPacketHangup},
    {"PT packet-mode Ctrl-C", PtPacketControlCharCheck1},
    {"PT packet-mode START/STOP assignment", PtPacketControlCharCheck2},
    {"PT packet-mode START/STOP", PtPacketControlCharCheck3},
    {"PT packet-mode toggle with control byte", PtPacketToggleWithControlByte},
    {"PT suspend output", PtSuspendOutput1},
    {"PT suspend output (part 2)", PtSuspendOutput2},
    {"PT suspend output (part 3)", PtSuspendOutput3},
    {"PT suspend output (part 4)", PtSuspendOutput4},
    {"PT suspend output (part 5)", PtSuspendOutput5},
    {"PT suspend output (part 6)", PtSuspendOutput6},
    {"PT suspend output (part 7)", PtSuspendOutput7},
    {"PT suspend output (part 8)", PtSuspendOutput8},
    {"PT suspend output (part 9)", PtSuspendOutput9},
    {"PT suspend output (part 10)", PtSuspendOutput10},
    {"PT suspend output (part 11)", PtSuspendOutput11},
    {"PT suspend output (part 12)", PtSuspendOutput12},
    {"PT suspend output (part 13)", PtSuspendOutput13},
    {"PT suspend output (part 14)", PtSuspendOutput14},
    {"PT suspend output (part 15)", PtSuspendOutput15},
    {"PT suspend output (part 16)", PtSuspendOutput16},
};

int DevPtTwoTestEntry(int Argc, char* Argv[])

/*++

Routine Description:

    This routine main entry point for the pty(2) test.

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
    LxtCheckErrno(LxtRunVariations(&Args, g_LxtVariations, LXT_COUNT_OF(g_LxtVariations)));

ErrorExit:
    LXT_SYNCHRONIZATION_POINT_DESTROY();
    LxtUninitialize();
    return !LXT_SUCCESS(Result);
}

int PtBackgroundBasic(PLXT_ARGS Args)

/*++

Routine Description:

    This routine performs basic IO checks from a background process.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    pid_t ChildPid;
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
    PtmFd = -1;
    PtsFd = -1;

    LXT_SYNCHRONIZATION_POINT_START();
    LxtCheckErrno(ChildPid = ForkPtyBackground(&PtmFd, &PtsFd, &ForegroundId));
    if (ChildPid == 0)
    {
        LxtCheckResult(LxtSignalInitialize());
        LxtCheckResult(LxtSignalSetupHandler(SIGTTOU, SA_SIGINFO));
        LxtCheckResult(LxtSignalSetupHandler(SIGTTIN, SA_SIGINFO));
        LxtCheckResult(SessionId = getsid(0));
        LxtCheckErrno(TerminalSessionId = tcgetsid(PtsFd));
        LxtCheckEqual(SessionId, TerminalSessionId, "%d");
        LxtCheckErrno(TerminalForegroundId = tcgetpgrp(PtsFd));
        LxtCheckEqual(ForegroundId, TerminalForegroundId, "%d");
        LxtCheckErrnoFailure(RawInit(PtsFd), EINTR);
        LxtCheckResult(LxtSignalCheckReceived(SIGTTOU));
        LxtSignalResetReceived();
        LxtCheckErrno(SimpleReadWriteCheckEx(PtmFd, PtsFd, SimpleReadWriteBackgroundSignalNoStop));
        LxtCheckResult(LxtSignalCheckReceived(SIGTTIN));
        LxtSignalResetReceived();
        LxtCheckErrno(tcflush(PtmFd, TCIFLUSH));

        //
        // Temporarily block SIGTTOU in order to enable TOSTOP
        //

        LxtCheckErrnoZeroSuccess(LxtSignalBlock(SIGTTOU));
        LxtCheckErrno(RawInit(PtsFd));
        LxtCheckResult(LxtSignalCheckNoSignal());
        LxtCheckErrnoZeroSuccess(LxtSignalUnblock(SIGTTOU));

        //
        // Try again with TOSTOP enabled
        //

        LxtLogInfo("Check with TOSTOP flag enabled");
        LxtSignalSetAllowMultiple(TRUE);
        LxtCheckErrno(SimpleReadWriteCheckEx(PtmFd, PtsFd, SimpleReadWriteBackgroundSignal));
        LxtCheckResult(LxtSignalCheckReceived(SIGTTIN));
        LxtCheckResult(LxtSignalCheckReceived(SIGTTOU));
        LxtSignalResetReceived();
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

    LXT_SYNCHRONIZATION_POINT_END();
    return Result;
}

int PtBackgroundBlockedSignals(PLXT_ARGS Args)

/*++

Routine Description:

    This routine performs IO checks from a background thread that has blocked
    SIGTTIN and SIGTTOU.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    pid_t ChildPid;
    pid_t ForegroundId;
    int PtmFd;
    int PtsFd;
    int Result;
    int Status;

    //
    // Initialize locals
    //

    ChildPid = -1;
    PtmFd = -1;
    PtsFd = -1;

    LXT_SYNCHRONIZATION_POINT_START();
    LxtCheckErrno(ChildPid = ForkPtyBackground(&PtmFd, &PtsFd, &ForegroundId));
    if (ChildPid == 0)
    {
        LxtCheckResult(LxtSignalInitialize());
        LxtCheckResult(LxtSignalSetupHandler(SIGTTOU, SA_SIGINFO));
        LxtCheckResult(LxtSignalSetupHandler(SIGTTIN, SA_SIGINFO));
        LxtCheckErrnoZeroSuccess(LxtSignalBlock(SIGTTIN));
        LxtCheckErrnoZeroSuccess(LxtSignalBlock(SIGTTOU));
        LxtCheckErrno(RawInit(PtsFd));
        LxtCheckErrno(SimpleReadWriteCheckEx(PtmFd, PtsFd, SimpleReadWriteBackgroundNoSignal));
        LxtCheckResult(LxtSignalCheckNoSignal());
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

    LXT_SYNCHRONIZATION_POINT_END();
    return Result;
}

int PtBackgroundSwitchToForeground(PLXT_ARGS Args)

/*++

Routine Description:

    This routine moves from a background process to a foreground process, with
    sanity IO checks.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int Result;

    LxtCheckErrno(PtBackgroundSwitchToForegroundWorker(false));

    //
    // TODO_LX: Implement master endpoint that can be a controlling terminal.
    //

    // LxtCheckErrno(PtBackgroundSwitchToForegroundWorker(true));

ErrorExit:
    return Result;
}

int PtBackgroundSwitchToForegroundWorker(bool UseMasterEndpoint)

/*++

Routine Description:

    This routine moves from a background process to a foreground process, with
    sanity IO checks.

Arguments:

    UseMasterEndpoint - Supplies the flag indicating whether the call should be
        made on the master or the subordinate endpoints.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    pid_t ChildPid;
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
    PtmFd = -1;
    PtsFd = -1;

    LXT_SYNCHRONIZATION_POINT_START();
    LxtCheckErrno(ChildPid = ForkPtyBackground(&PtmFd, &PtsFd, &ForegroundId));
    if (ChildPid == 0)
    {
        LxtCheckResult(LxtSignalInitialize());
        LxtCheckResult(LxtSignalSetupHandler(SIGTTOU, SA_SIGINFO));
        LxtCheckResult(LxtSignalSetupHandler(SIGTTIN, SA_SIGINFO));
        LxtCheckErrnoFailure(tcsetpgrp((UseMasterEndpoint) ? PtmFd : PtsFd, getpgid(0)), EINTR);

        LxtCheckResult(LxtSignalCheckReceived(SIGTTOU));
        LxtSignalResetReceived();

        //
        // Temporarily block SIGTTOU to force process to foreground.
        //

        LxtCheckErrnoZeroSuccess(LxtSignalBlock(SIGTTOU));
        LxtCheckErrno(tcsetpgrp((UseMasterEndpoint) ? PtmFd : PtsFd, getpgid(0)));

        LxtCheckErrnoZeroSuccess(LxtSignalUnblock(SIGTTOU));

        //
        // Verify foreground IO behavior.
        //

        LxtCheckResult(SessionId = getsid(0));
        LxtCheckErrno(TerminalSessionId = tcgetsid(PtsFd));
        LxtCheckEqual(SessionId, TerminalSessionId, "%d");
        LxtCheckErrno(TerminalForegroundId = tcgetpgrp(PtsFd));
        LxtCheckNotEqual(ForegroundId, TerminalForegroundId, "%d");
        LxtCheckErrno(RawInit(PtsFd));
        LxtCheckErrno(SimpleReadWriteCheck(PtmFd, PtsFd));
        LxtCheckResult(LxtSignalCheckNoSignal());
        LxtSignalResetReceived();
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

    LXT_SYNCHRONIZATION_POINT_END();
    return Result;
}

int PtBackgroundDisassociateTty1(PLXT_ARGS Args)

/*++

Routine Description:

    This routine removes the controlling terminal from a background thread and
    then tests ioctl behavior.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    pid_t ChildPid;
    tcflag_t ControlFlags;
    pid_t ForegroundId;
    int PtmFd;
    int PtsFd;
    int Result;
    pid_t SessionId;
    int Status;
    pid_t TerminalForegroundId;
    pid_t TerminalSessionId;
    struct winsize WindowSizeM;

    //
    // Initialize locals
    //

    ChildPid = -1;
    PtmFd = -1;
    PtsFd = -1;

    LXT_SYNCHRONIZATION_POINT_START();
    LxtCheckErrno(ChildPid = ForkPtyBackground(&PtmFd, &PtsFd, &ForegroundId));
    if (ChildPid == 0)
    {
        LxtCheckResult(LxtSignalInitialize());
        LxtCheckResult(LxtSignalSetupHandler(SIGTTOU, SA_SIGINFO));
        LxtCheckResult(LxtSignalSetupHandler(SIGTTIN, SA_SIGINFO));
        LxtCheckResult(LxtSignalSetupHandler(SIGHUP, SA_SIGINFO));
        LxtCheckResult(LxtSignalSetupHandler(SIGCONT, SA_SIGINFO));
        LxtCheckResult(SessionId = getsid(0));

        //
        // Disconnect the controlling terminal.
        //

        LxtCheckErrno(ioctl(PtsFd, TIOCNOTTY, (char*)NULL));
        LxtCheckResult(LxtSignalCheckNoSignal());

        //
        // Test various ioctl behavior on the subordinate endpoint.
        //

        LxtCheckErrno(TerminalSettingsGetControlFlags(PtsFd, &ControlFlags));
        LxtCheckErrno(TerminalSettingsSetControlFlags(PtsFd, ControlFlags));
        LxtCheckErrnoFailure(tcgetpgrp(PtsFd), ENOTTY);
        LxtCheckErrnoFailure(tcsetpgrp(PtsFd, getpgid(0)), ENOTTY);

        LxtCheckErrnoFailure(tcgetsid(PtsFd), ENOTTY);
        LxtCheckErrno(ioctl(PtsFd, TIOCGWINSZ, &WindowSizeM));
        LxtCheckErrno(ioctl(PtsFd, TIOCSWINSZ, &WindowSizeM));
        LxtCheckErrno(tcflow(PtsFd, TCOOFF));
        LxtCheckErrno(tcflow(PtsFd, TCOON));
        LxtCheckErrno(tcflow(PtsFd, TCIOFF));
        LxtCheckErrno(tcflow(PtsFd, TCION));
        LxtCheckErrnoFailure(ioctl(PtsFd, TIOCSCTTY, (char*)NULL), EPERM);
        LxtCheckErrnoFailure(ioctl(PtsFd, TIOCNOTTY, (char*)NULL), ENOTTY);
        LxtCheckErrno(tcdrain(PtsFd));
        LxtCheckErrno(tcflush(PtsFd, TCIOFLUSH));
        LxtCheckErrno(ioctl(PtsFd, TIOCSTI, "x"));
        LxtCheckErrnoFailure(ioctl(PtsFd, TIOCSTI, (char*)NULL), EFAULT);

        //
        // Test various ioctl behavior on the master endpoint.
        //

        LxtCheckErrno(TerminalSettingsGetControlFlags(PtmFd, &ControlFlags));
        LxtCheckErrno(TerminalSettingsSetControlFlags(PtmFd, ControlFlags));

        //
        // On Linux, The master endpoint returns the foreground/session state.
        //

        LxtCheckErrno(TerminalForegroundId = tcgetpgrp(PtmFd));
        LxtCheckEqual(TerminalForegroundId, ForegroundId, "%d");
        LxtCheckErrnoFailure(tcsetpgrp(PtmFd, getpgid(0)), ENOTTY);

        LxtCheckErrno(TerminalSessionId = tcgetsid(PtmFd));
        LxtCheckEqual(TerminalSessionId, SessionId, "%d");
        LxtCheckErrno(ioctl(PtmFd, TIOCGWINSZ, &WindowSizeM));
        LxtCheckErrno(ioctl(PtmFd, TIOCSWINSZ, &WindowSizeM));
        LxtCheckErrno(tcflow(PtmFd, TCOOFF));
        LxtCheckErrno(tcflow(PtmFd, TCOON));
        LxtCheckErrno(tcflow(PtmFd, TCIOFF));
        LxtCheckErrno(tcflow(PtmFd, TCION));
        LxtCheckErrnoFailure(ioctl(PtmFd, TIOCSCTTY, (char*)NULL), EPERM);
        LxtCheckErrnoFailure(ioctl(PtmFd, TIOCNOTTY, (char*)NULL), ENOTTY);
        LxtCheckErrno(tcdrain(PtmFd));
        LxtCheckErrno(tcflush(PtmFd, TCIOFLUSH));
        LxtCheckErrno(ioctl(PtmFd, TIOCSTI, "x"));
        LxtCheckErrnoFailure(ioctl(PtmFd, TIOCSTI, (char*)NULL), EFAULT);
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

    LXT_SYNCHRONIZATION_POINT_END();

    return Result;
}

int PtBackgroundDisassociateTty2(PLXT_ARGS Args)

/*++

Routine Description:

    This routine removes the controlling terminal from a background thread.

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
    LxtCheckErrno(ChildPid = ForkPtyBackground(&PtmFd, &PtsFd, &ForegroundId));
    if (ChildPid == 0)
    {
        LxtCheckResult(LxtSignalInitialize());
        LxtCheckResult(LxtSignalSetupHandler(SIGTTOU, SA_SIGINFO));
        LxtCheckResult(LxtSignalSetupHandler(SIGTTIN, SA_SIGINFO));
        LxtCheckResult(LxtSignalSetupHandler(SIGHUP, SA_SIGINFO));
        LxtCheckResult(LxtSignalSetupHandler(SIGCONT, SA_SIGINFO));
        LxtCheckResult(SessionId = getsid(0));

        //
        // Allow the other thread to try to disassociate the terminal, and wait
        // for that to complete.
        //

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
        LxtCheckResult(LxtSignalCheckNoSignal());

        //
        // Trying to disconnect again should fail.
        //

        LxtCheckErrnoFailure(ioctl(PtsFd, TIOCNOTTY, (char*)NULL), ENOTTY);

        //
        // The terminal is no longer associated, so it is expected to fail the
        // commands to retrieve session and foreground process group.
        //

        LxtCheckErrnoFailure(TerminalSessionId = tcgetsid(PtsFd), ENOTTY);
        LxtCheckErrnoFailure(TerminalForegroundId = tcgetpgrp(PtsFd), ENOTTY);

        //
        // On Linux, The master endpoint returns the foreground/session state.
        //

        LxtCheckErrno(TerminalSessionId = tcgetsid(PtmFd));
        LxtCheckEqual(TerminalSessionId, SessionId, "%d");
        LxtCheckErrno(TerminalForegroundId = tcgetpgrp(PtmFd));
        LxtCheckEqual(TerminalForegroundId, ForegroundId, "%d");

        //
        // Do a simple IO check.
        //

        LxtCheckErrno(RawInit(PtsFd));
        LxtCheckErrno(SimpleReadWriteCheck(PtmFd, PtsFd));
        LxtCheckResult(LxtSignalCheckNoSignal());
    }
    else
    {

        //
        // Try to disassociate terminal from another session.
        //

        LxtCheckErrnoFailure(ioctl(PtsFd, TIOCNOTTY, (char*)NULL), ENOTTY);
        LXT_SYNCHRONIZATION_POINT();
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

int PtBackgroundDisassociateTty3(PLXT_ARGS Args)

/*++

Routine Description:

    This routine removes the controlling terminal from a foreground thread and
    checks the behavior on both foreground and background threads.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    pid_t ChildPid;
    BOOLEAN EndChildPidSynchronization;
    pid_t ForegroundId;
    pid_t GrandChildPid;
    int PtmFd;
    int PtsFd;
    int Result;
    pid_t SessionId;
    int Status;
    pid_t TerminalForegroundId;
    pid_t TerminalSessionId;
    LXT_SYNCHRONIZATION_POINT_DECLARE_FOR(GrandChildPid);

    //
    // Initialize locals
    //

    ChildPid = -1;
    EndChildPidSynchronization = TRUE;
    GrandChildPid = -1;
    PtmFd = -1;
    PtsFd = -1;
    LXT_SYNCHRONIZATION_POINT_INIT_FOR(GrandChildPid);

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
        // Fork again to create a foreground and background thread.
        //

        LXT_SYNCHRONIZATION_POINT_START_FOR(GrandChildPid);
        LxtCheckErrno(GrandChildPid = fork());
        if (GrandChildPid == 0)
        {
            LxtCheckResult(LxtSignalInitialize());
            LxtSignalSetAllowMultiple(TRUE);
            LxtCheckResult(LxtSignalSetupHandler(SIGTTOU, SA_SIGINFO));
            LxtCheckResult(LxtSignalSetupHandler(SIGTTIN, SA_SIGINFO));
            LxtCheckResult(LxtSignalSetupHandler(SIGHUP, SA_SIGINFO));
            LxtCheckResult(LxtSignalSetupHandler(SIGCONT, SA_SIGINFO));

            //
            // Move to the background.
            //

            LxtLogInfo("Moving thread %d to the background.", getpid());
            LxtCheckErrno(setpgid(0, 0));

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
            // Signal the foreground thread to disconnect the controlling
            // terminal, and wait for the signal that it has completed.
            //

            LXT_SYNCHRONIZATION_POINT_FOR(GrandChildPid);
            LXT_SYNCHRONIZATION_POINT_FOR(GrandChildPid);
            LxtCheckResult(LxtSignalCheckNoSignal());
            LxtCheckErrnoFailure(tcgetsid(PtsFd), ENOTTY);
            LxtCheckErrnoFailure(tcgetpgrp(PtsFd), ENOTTY);

            //
            // On Linux, The master endpoint returns foreground/session state,
            // but instead of failing the foreground group query will just
            // return 0.
            //

            LxtCheckErrnoFailure(tcgetsid(PtmFd), ENOTTY);
            LxtCheckErrno(TerminalForegroundId = tcgetpgrp(PtmFd));
            LxtCheckEqual(TerminalForegroundId, 0, "%d");

            //
            // Do a simple IO test.
            //

            LxtCheckErrno(RawInit(PtsFd));
            LxtCheckErrno(SimpleReadWriteCheck(PtmFd, PtsFd));
            LxtCheckResult(LxtSignalCheckNoSignal());
            LXT_SYNCHRONIZATION_POINT_FOR(GrandChildPid);
        }
        else
        {
            LXT_SYNCHRONIZATION_POINT_FOR(GrandChildPid);
            LxtCheckErrno(ioctl(PtsFd, TIOCNOTTY, (char*)NULL));
            LXT_SYNCHRONIZATION_POINT_FOR(GrandChildPid);

            //
            // TODO_LX: Support SIGCONT.
            //
            // LxtCheckResult(LxtSignalCheckReceived(SIGCONT));
            //

            LxtCheckResult(LxtSignalCheckReceived(SIGHUP));
            LxtSignalResetReceived();
            LxtCheckErrnoFailure(ioctl(PtsFd, TIOCNOTTY, (char*)NULL), ENOTTY);

            //
            // The terminal is no longer associated, so it is expected to fail the
            // commands to retrieve session and foreground process group.
            //

            LxtCheckErrnoFailure(tcgetsid(PtsFd), ENOTTY);
            LxtCheckErrnoFailure(tcgetpgrp(PtsFd), ENOTTY);

            //
            // On Linux, The master endpoint returns foreground/session state,
            // but instead of failing the foreground group query will just
            // return 0.
            //

            LxtCheckErrnoFailure(tcgetsid(PtmFd), ENOTTY);
            LxtCheckErrno(TerminalForegroundId = tcgetpgrp(PtmFd));
            LxtCheckEqual(TerminalForegroundId, 0, "%d");

            //
            // Wait for other thread to finish its IO test, then do an IO test
            // here.
            //

            LXT_SYNCHRONIZATION_POINT_FOR(GrandChildPid);
            LxtCheckErrno(RawInit(PtsFd));
            LxtCheckErrno(SimpleReadWriteCheck(PtmFd, PtsFd));
            LxtCheckResult(LxtSignalCheckNoSignal());
        }
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

    LXT_SYNCHRONIZATION_POINT_END_FOR(GrandChildPid, TRUE);
    if (EndChildPidSynchronization != FALSE)
    {
        LXT_SYNCHRONIZATION_POINT_END();
    }

    return Result;
}

int PtBackgroundDisassociateTty4(PLXT_ARGS Args)

/*++

Routine Description:

    This routine removes the controlling terminal from a background thread and
    checks the behavior on both foreground and background threads.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    pid_t ChildPid;
    BOOLEAN EndChildPidSynchronization;
    pid_t ForegroundId;
    pid_t GrandChildPid;
    int PtmFd;
    int PtsFd;
    int Result;
    pid_t SessionId;
    int Status;
    pid_t TerminalForegroundId;
    pid_t TerminalSessionId;
    LXT_SYNCHRONIZATION_POINT_DECLARE_FOR(GrandChildPid);

    //
    // Initialize locals
    //

    ChildPid = -1;
    EndChildPidSynchronization = TRUE;
    GrandChildPid = -1;
    PtmFd = -1;
    PtsFd = -1;
    LXT_SYNCHRONIZATION_POINT_INIT_FOR(GrandChildPid);

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
        // Fork again to create a foreground and background thread.
        //

        LXT_SYNCHRONIZATION_POINT_START_FOR(GrandChildPid);
        LxtCheckErrno(GrandChildPid = fork());
        if (GrandChildPid == 0)
        {
            EndChildPidSynchronization = FALSE;
            LxtCheckResult(LxtSignalInitialize());
            LxtSignalSetAllowMultiple(TRUE);
            LxtCheckResult(LxtSignalSetupHandler(SIGTTOU, SA_SIGINFO));
            LxtCheckResult(LxtSignalSetupHandler(SIGTTIN, SA_SIGINFO));
            LxtCheckResult(LxtSignalSetupHandler(SIGHUP, SA_SIGINFO));
            LxtCheckResult(LxtSignalSetupHandler(SIGCONT, SA_SIGINFO));

            //
            // Move to the background.
            //

            LxtLogInfo("Moving thread %d to the background.", getpid());
            LxtCheckErrno(setpgid(0, 0));

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

            LxtLogInfo(
                "Disconnecting controlling terminal from background "
                "thread %d.",
                getpid());

            LxtCheckErrno(ioctl(PtsFd, TIOCNOTTY, (char*)NULL));
            LXT_SYNCHRONIZATION_POINT_FOR(GrandChildPid);
            LxtCheckResult(LxtSignalCheckNoSignal());

            //
            // Check session and foreground process group again.
            //

            LxtLogInfo("Checking ioctls from thread %d after disconnect.", getpid());
            LxtCheckErrnoFailure(tcgetsid(PtsFd), ENOTTY);
            LxtCheckErrnoFailure(tcgetpgrp(PtsFd), ENOTTY);

            //
            // On Linux, The master endpoint returns the foreground/session
            // state.
            //

            LxtCheckErrno(TerminalSessionId = tcgetsid(PtmFd));
            LxtCheckEqual(TerminalSessionId, SessionId, "%d");
            LxtCheckErrno(TerminalForegroundId = tcgetpgrp(PtmFd));
            LxtCheckEqual(TerminalForegroundId, ForegroundId, "%d");

            //
            // Do a simple IO test.
            //

            LxtCheckErrno(RawInit(PtsFd));
            LxtCheckErrno(SimpleReadWriteCheck(PtmFd, PtsFd));
            LxtCheckResult(LxtSignalCheckNoSignal());
            LXT_SYNCHRONIZATION_POINT_FOR(GrandChildPid);
        }
        else
        {

            //
            // Wait for the background thread to disconnect from the
            // controlling terminal.
            //

            LXT_SYNCHRONIZATION_POINT_FOR(GrandChildPid);
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
            // Wait for other thread to finish its IO test, then do an IO test
            // here.
            //

            LXT_SYNCHRONIZATION_POINT_FOR(GrandChildPid);
            LxtCheckErrno(RawInit(PtsFd));
            LxtCheckErrno(SimpleReadWriteCheck(PtmFd, PtsFd));
            LxtCheckResult(LxtSignalCheckNoSignal());
        }
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

        LxtLogInfo("Waiting for child thread %d to exit.", ChildPid);
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
    LxtLogInfo("Exiting thread %d with Result = %d.", getpid(), Result);
    if ((ChildPid != 0) && (PtmFd != -1))
    {
        close(PtmFd);
    }

    if ((ChildPid != 0) && (PtsFd != -1))
    {
        close(PtsFd);
    }

    LXT_SYNCHRONIZATION_POINT_END_FOR(GrandChildPid, TRUE);
    if (EndChildPidSynchronization != FALSE)
    {
        LXT_SYNCHRONIZATION_POINT_END();
    }

    return Result;
}

int PtBackgroundDisassociateTty5(PLXT_ARGS Args)

/*++

Routine Description:

    This routine removes the controlling terminal from a background thread,
    switches to a new session and establishes a new controlling terminal.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    pid_t ChildPid;
    pid_t ForegroundId;
    pid_t GrandChildPid;
    pid_t GrandChildSessionId;
    int PtmFd;
    int Ptm2Fd;
    int PtsFd;
    int Pts2Fd;
    int Result;
    int SerialNumber;
    pid_t SessionId;
    int Status;
    pid_t TerminalForegroundId;
    pid_t TerminalSessionId;
    struct winsize WindowSizeM;
    LXT_SYNCHRONIZATION_POINT_DECLARE_FOR(GrandChildPid);

    //
    // Initialize locals
    //

    ChildPid = -1;
    GrandChildPid = -1;
    PtmFd = -1;
    Ptm2Fd = -1;
    PtsFd = -1;
    Pts2Fd = -1;
    LXT_SYNCHRONIZATION_POINT_INIT_FOR(GrandChildPid);

    LXT_SYNCHRONIZATION_POINT_START();
    LxtCheckErrno(ChildPid = ForkPtyBackground(&PtmFd, &PtsFd, &ForegroundId));
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
        // Disconnect the controlling terminal.
        //

        LxtLogInfo(
            "Disconnecting controlling terminal from background "
            "thread %d.",
            getpid());

        LxtCheckErrno(ioctl(PtsFd, TIOCNOTTY, (char*)NULL));
        LxtCheckResult(LxtSignalCheckNoSignal());

        //
        // Create a second set of endpoints.
        //

        LxtCheckErrno(OpenMasterSubordinate(&Ptm2Fd, &Pts2Fd, NULL, &SerialNumber));
        LxtLogInfo("Second master opened at FD:%d", Ptm2Fd);
        LxtLogInfo("Second subordinate Serial Number: %d", SerialNumber);
        LxtLogInfo("Second subordinate opened at FD:%d", Pts2Fd);

        //
        // Fork again to test terminal behavior on new thread after disconnect.
        //

        LxtCheckErrno(GrandChildPid = fork());
        if (GrandChildPid == 0)
        {
            LxtCheckResult(LxtSignalInitialize());
            LxtSignalSetAllowMultiple(TRUE);
            LxtCheckResult(LxtSignalSetupHandler(SIGTTOU, SA_SIGINFO));
            LxtCheckResult(LxtSignalSetupHandler(SIGTTIN, SA_SIGINFO));
            LxtCheckResult(LxtSignalSetupHandler(SIGHUP, SA_SIGINFO));
            LxtCheckResult(LxtSignalSetupHandler(SIGCONT, SA_SIGINFO));

            //
            // Check that the new thread is still disconnected from the
            // original endpoints, and not associated with the new endpoints.
            //

            LxtCheckErrnoFailure(tcgetsid(PtsFd), ENOTTY);
            LxtCheckErrnoFailure(tcgetpgrp(PtsFd), ENOTTY);
            LxtCheckErrnoFailure(tcgetsid(Pts2Fd), ENOTTY);
            LxtCheckErrnoFailure(tcgetpgrp(Pts2Fd), ENOTTY);

            //
            // Try to add a controlling terminal before creating a new session.
            //

            LxtCheckErrnoFailure(ioctl(Pts2Fd, TIOCSCTTY, (char*)NULL), EPERM);
            LxtCheckErrnoFailure(ioctl(PtsFd, TIOCNOTTY, (char*)NULL), ENOTTY);

            //
            // Create a new session.
            //

            LxtCheckErrno(GrandChildSessionId = setsid());

            //
            // Check that the thread, now inside a new session is still
            // disconnected from any endpoints.
            //

            LxtCheckErrnoFailure(tcgetsid(PtsFd), ENOTTY);
            LxtCheckErrnoFailure(tcgetpgrp(PtsFd), ENOTTY);
            LxtCheckErrnoFailure(tcgetsid(Pts2Fd), ENOTTY);
            LxtCheckErrnoFailure(tcgetpgrp(Pts2Fd), ENOTTY);

            //
            // Try to add a controlling terminal inside the new session.
            //

            LxtCheckErrno(ioctl(Pts2Fd, TIOCSCTTY, (char*)NULL));
            LxtCheckResult(ForegroundId = getpid());

            //
            // Check session and foreground process group again.
            //

            LxtCheckErrno(TerminalSessionId = tcgetsid(Pts2Fd));
            LxtCheckEqual(GrandChildSessionId, TerminalSessionId, "%d");
            LxtCheckErrno(TerminalSessionId = tcgetsid(Ptm2Fd));
            LxtCheckEqual(GrandChildSessionId, TerminalSessionId, "%d");
            LxtCheckErrno(TerminalForegroundId = tcgetpgrp(Pts2Fd));
            LxtCheckEqual(ForegroundId, TerminalForegroundId, "%d");
            LxtCheckErrno(TerminalForegroundId = tcgetpgrp(Ptm2Fd));
            LxtCheckEqual(ForegroundId, TerminalForegroundId, "%d");
            LxtCheckErrno(RawInit(Pts2Fd));
            LxtCheckErrno(SimpleReadWriteCheck(Ptm2Fd, Pts2Fd));
            LxtCheckResult(LxtSignalCheckNoSignal());
            LXT_SYNCHRONIZATION_POINT_FOR(GrandChildPid);
            LXT_SYNCHRONIZATION_POINT_FOR(GrandChildPid);
        }
        else
        {

            //
            // Try cross-session access to the endpoints.
            //

            LXT_SYNCHRONIZATION_POINT_FOR(GrandChildPid);
            LxtCheckErrnoFailure(tcgetsid(Pts2Fd), ENOTTY);
            LxtCheckErrno(TerminalSessionId = tcgetsid(Ptm2Fd));
            LxtCheckNotEqual(SessionId, TerminalSessionId, "%d");
            LxtCheckErrnoFailure(tcgetpgrp(Pts2Fd), ENOTTY);
            LxtCheckErrno(TerminalForegroundId = tcgetpgrp(Ptm2Fd));
            LxtCheckNotEqual(ForegroundId, TerminalForegroundId, "%d");
            LxtCheckErrnoFailure(tcsetpgrp(Pts2Fd, getpgid(0)), ENOTTY);
            LxtCheckErrno(ioctl(Pts2Fd, TIOCGWINSZ, &WindowSizeM));
            LxtCheckErrno(ioctl(Ptm2Fd, TIOCGWINSZ, &WindowSizeM));
            LxtCheckErrno(ioctl(Pts2Fd, TIOCSWINSZ, &WindowSizeM));
            LxtCheckErrno(ioctl(Ptm2Fd, TIOCSWINSZ, &WindowSizeM));
            LxtCheckErrno(tcflow(Pts2Fd, TCOOFF));
            LxtCheckErrno(tcflow(Ptm2Fd, TCOOFF));
            LxtCheckErrno(tcflow(Pts2Fd, TCOON));
            LxtCheckErrno(tcflow(Ptm2Fd, TCOON));
            LxtCheckErrno(tcflow(Pts2Fd, TCIOFF));
            LxtCheckErrno(tcflow(Ptm2Fd, TCIOFF));
            LxtCheckErrno(tcflow(Pts2Fd, TCION));
            LxtCheckErrno(tcflow(Ptm2Fd, TCION));
            LxtCheckErrnoFailure(ioctl(Pts2Fd, TIOCSCTTY, (char*)NULL), EPERM);
            LxtCheckErrnoFailure(ioctl(Ptm2Fd, TIOCSCTTY, (char*)NULL), EPERM);
            LxtCheckErrnoFailure(ioctl(Pts2Fd, TIOCNOTTY, (char*)NULL), ENOTTY);
            LxtCheckErrnoFailure(ioctl(Ptm2Fd, TIOCNOTTY, (char*)NULL), ENOTTY);
            LxtCheckErrno(tcdrain(Pts2Fd));
            LxtCheckErrno(tcdrain(Ptm2Fd));
            LxtCheckErrno(tcflush(Pts2Fd, TCIOFLUSH));
            LxtCheckErrno(tcflush(Ptm2Fd, TCIOFLUSH));

            //
            // Test IO.
            //

            LxtCheckErrno(RawInit(Pts2Fd));
            LxtCheckErrno(SimpleReadWriteCheck(Ptm2Fd, Pts2Fd));
            LxtCheckResult(LxtSignalCheckNoSignal());

            //
            // Test TIOCSTI.
            //

            LxtCheckErrno(ioctl(Pts2Fd, TIOCSTI, "x"));
            LxtCheckErrno(ioctl(Ptm2Fd, TIOCSTI, "x"));
            LxtCheckErrnoFailure(ioctl(Pts2Fd, TIOCSTI, (char*)NULL), EFAULT);
            LxtCheckErrnoFailure(ioctl(Ptm2Fd, TIOCSTI, (char*)NULL), EFAULT);
            LXT_SYNCHRONIZATION_POINT_FOR(GrandChildPid);
        }
    }

ErrorExit:
    LxtLogInfo("Exiting thread %d with Result = %d.", getpid(), Result);
    if (Ptm2Fd != -1)
    {
        close(Ptm2Fd);
    }

    if (Pts2Fd != -1)
    {
        close(Pts2Fd);
    }

    if ((ChildPid != 0) && (PtmFd != -1))
    {
        close(PtmFd);
    }

    if ((ChildPid != 0) && (PtsFd != -1))
    {
        close(PtsFd);
    }

    LXT_SYNCHRONIZATION_POINT_END_FOR(GrandChildPid, TRUE);
    if (GrandChildPid != 0)
    {
        LXT_SYNCHRONIZATION_POINT_END();
    }

    return Result;
}

int PtBackgroundDisassociateTty6(PLXT_ARGS Args)

/*++

Routine Description:

    This routine removes the controlling terminal from another thread created
    with CLONE_THREAD and checks the behavior on all threads.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    pid_t ChildPid;
    LXT_CLONE_ARGS CloneArgs;
    pid_t ForegroundId;
    pthread_t GrandChildTid;
    void* GrandChildResult;
    LXT_SYNCHRONIZATION_POINT_DECLARE_FOR(GrandChildTid);
    int PtmFd;
    int PtsFd;
    int Result;
    pid_t SessionId;
    int Status;
    pid_t TerminalForegroundId;
    pid_t TerminalSessionId;
    PT_THREAD_PARAMETERS ThreadParameters;

    //
    // Initialize locals
    //

    ChildPid = -1;
    GrandChildTid = 0;
    memset(&CloneArgs, 0, sizeof(CloneArgs));
    PtmFd = -1;
    PtsFd = -1;
    LXT_SYNCHRONIZATION_POINT_INIT_FOR(GrandChildTid);

    LxtCheckErrno(ChildPid = ForkPtyBackground(&PtmFd, &PtsFd, &ForegroundId));
    if (ChildPid == 0)
    {
        LxtCheckResult(LxtSignalInitialize());
        LxtSignalSetAllowMultiple(TRUE);
        LxtCheckResult(LxtSignalSetupHandler(SIGTTOU, SA_SIGINFO));
        LxtCheckResult(LxtSignalSetupHandler(SIGTTIN, SA_SIGINFO));
        LxtCheckResult(LxtSignalSetupHandler(SIGHUP, SA_SIGINFO));
        LxtCheckResult(LxtSignalSetupHandler(SIGCONT, SA_SIGINFO));
        LxtCheckResult(SessionId = getsid(0));

        //
        // Clone a new thread.
        //

        memset(&ThreadParameters, 0, sizeof(ThreadParameters));
        ThreadParameters.ForegroundId = ForegroundId;
        ThreadParameters.PtmFd = PtmFd;
        ThreadParameters.PtsFd = PtsFd;
        ThreadParameters.SessionId = SessionId;
        ThreadParameters.SynchronizationEventChild = LxtSyncGrandChildTidChild;
        ThreadParameters.SynchronizationEventParent = LxtSyncGrandChildTidParent;
        LxtCheckErrno(pthread_create(&GrandChildTid, NULL, (void* (*)(void*))PtBackgroundDisassociateTty6Thread, &ThreadParameters));

        //
        // Wait for the other thread to disconnect from the
        // controlling terminal.
        //

        LXT_SYNCHRONIZATION_POINT_FOR(GrandChildTid);
        LxtCheckResult(LxtSignalCheckNoSignal());

        LxtCheckErrnoFailure(tcgetsid(PtsFd), ENOTTY);
        LxtCheckErrnoFailure(tcgetpgrp(PtsFd), ENOTTY);

        //
        // On Linux, The master endpoint returns the foreground/session
        // state.
        //

        LxtCheckErrno(TerminalSessionId = tcgetsid(PtmFd));
        LxtCheckEqual(TerminalSessionId, SessionId, "%d");
        LxtCheckErrno(TerminalForegroundId = tcgetpgrp(PtmFd));
        LxtCheckEqual(TerminalForegroundId, ForegroundId, "%d");

        //
        // Wait for other thread to finish its IO test, then do an IO test
        // here.
        //

        LXT_SYNCHRONIZATION_POINT_FOR(GrandChildTid);
        LxtCheckErrno(RawInit(PtsFd));
        LxtCheckErrno(SimpleReadWriteCheck(PtmFd, PtsFd));
        LxtCheckResult(LxtSignalCheckNoSignal());
    }
    else
    {

        //
        // Wait for the child here in order to run more tests after the session
        // has been destroyed.
        //

        LxtLogInfo("Waiting for child thread %d to exit.", ChildPid);
        LxtCheckErrno(TEMP_FAILURE_RETRY(Result = waitpid(ChildPid, &Status, 0)));
        LxtCheckResult(WIFEXITED(Status) ? 0 : -1);
        LxtCheckResult((int)(char)WEXITSTATUS(Status));

        //
        // Check status of master endpoint after session is gone.
        //

        LxtCheckErrnoFailure(tcgetsid(PtmFd), ENOTTY);
        LxtCheckErrno(tcgetpgrp(PtmFd));
    }

ErrorExit:
    LxtLogInfo("Exiting thread %d with Result = %d.", getpid(), Result);
    if (GrandChildTid > 0)
    {
        if (Result < 0)
        {
            LxtSynchronizationEventFail(LxtSyncGrandChildTidChild);
        }

        Result = pthread_join(GrandChildTid, &GrandChildResult);
        if (Result != 0)
        {
            LxtLogError("Failed pthread_join with error %d", Result);
        }
        else
        {

            Result = (int)(long)GrandChildResult;
        }

        exit(Result);
    }

    if ((ChildPid != 0) && (PtmFd != -1))
    {
        close(PtmFd);
    }

    if ((ChildPid != 0) && (PtsFd != -1))
    {
        close(PtsFd);
    }

    return Result;
}

void* PtBackgroundDisassociateTty6Thread(PPT_THREAD_PARAMETERS ThreadParameters)

/*++

Routine Description:

    This routine is called on a new thread from PtBackgroundDisassociateTty6.

Arguments:

    ThreadParameters - Supplies thread parameters.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int Result;
    pid_t TerminalForegroundId;
    pid_t TerminalSessionId;

    LxtCheckResult(LxtSignalInitializeThread());
    LxtSignalSetAllowMultiple(TRUE);
    LxtCheckResult(LxtSignalSetupHandler(SIGTTOU, SA_SIGINFO));
    LxtCheckResult(LxtSignalSetupHandler(SIGTTIN, SA_SIGINFO));
    LxtCheckResult(LxtSignalSetupHandler(SIGHUP, SA_SIGINFO));
    LxtCheckResult(LxtSignalSetupHandler(SIGCONT, SA_SIGINFO));

    //
    // Check session and foreground process group for both endpoints of
    // the psuedo-terminal.
    //

    LxtCheckErrno(TerminalSessionId = tcgetsid(ThreadParameters->PtsFd));
    LxtCheckEqual(TerminalSessionId, ThreadParameters->SessionId, "%d");
    LxtCheckErrno(TerminalSessionId = tcgetsid(ThreadParameters->PtmFd));
    LxtCheckEqual(TerminalSessionId, ThreadParameters->SessionId, "%d");
    LxtCheckErrno(TerminalForegroundId = tcgetpgrp(ThreadParameters->PtsFd));
    LxtCheckEqual(TerminalForegroundId, ThreadParameters->ForegroundId, "%d");
    LxtCheckErrno(TerminalForegroundId = tcgetpgrp(ThreadParameters->PtmFd));
    LxtCheckEqual(TerminalForegroundId, ThreadParameters->ForegroundId, "%d");

    //
    // Disconnect the controlling terminal.
    //

    LxtLogInfo("Disconnecting controlling terminal from thread %d.", gettid());

    LxtCheckErrno(ioctl(ThreadParameters->PtsFd, TIOCNOTTY, (char*)NULL));
    LxtCheckResult(LxtSignalCheckNoSignal());
    LXT_SYNCHRONIZATION_POINT_SYNCVARS(TRUE, ThreadParameters->SynchronizationEventParent, ThreadParameters->SynchronizationEventChild);

    //
    // Check session and foreground process group again.
    //

    LxtLogInfo("Checking ioctls from thread %d after disconnect.", gettid());
    LxtCheckErrnoFailure(tcgetsid(ThreadParameters->PtsFd), ENOTTY);
    LxtCheckErrnoFailure(tcgetpgrp(ThreadParameters->PtsFd), ENOTTY);

    //
    // On Linux, The master endpoint returns the foreground/session
    // state.
    //

    LxtCheckErrno(TerminalSessionId = tcgetsid(ThreadParameters->PtmFd));
    LxtCheckEqual(TerminalSessionId, ThreadParameters->SessionId, "%d");
    LxtCheckErrno(TerminalForegroundId = tcgetpgrp(ThreadParameters->PtmFd));
    LxtCheckEqual(TerminalForegroundId, ThreadParameters->ForegroundId, "%d");

    //
    // Do a simple IO test.
    //

    LxtCheckErrno(RawInit(ThreadParameters->PtsFd));
    LxtCheckErrno(SimpleReadWriteCheck(ThreadParameters->PtmFd, ThreadParameters->PtsFd));

    LxtCheckResult(LxtSignalCheckNoSignal());
    LXT_SYNCHRONIZATION_POINT_SYNCVARS(TRUE, ThreadParameters->SynchronizationEventParent, ThreadParameters->SynchronizationEventChild);

ErrorExit:
    LxtLogInfo("Exiting thread %d with Result = %d", gettid(), Result);
    if (Result < 0)
    {
        LxtSynchronizationEventFail(ThreadParameters->SynchronizationEventParent);
    }

    return (void*)(long)Result;
}

int PtBufferTerminalFill(PLXT_ARGS Args)

/*++

Routine Description:

    This routine checks the internal implementation of the buffer by attempting
    to fill it.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int BytesReadWrite;
    int PtmFd;
    int PtmFlags;
    int PtsFd;
    int Result;
    int SerialNumber;
    struct timeval Timeout;
    char WriteBuffer[] = "abcdefghijklmn";
    char WriteBuffer2[] = "0123456\n789ABC";
    size_t WriteBufferLen;
    fd_set WriteFds;

    //
    // Initialize locals.
    //

    PtmFd = -1;
    PtsFd = -1;
    WriteBufferLen = sizeof(WriteBuffer) - 1;

    //
    // Open Master-Subordinate.
    //

    LxtCheckErrno(OpenMasterSubordinate(&PtmFd, &PtsFd, NULL, &SerialNumber));
    LxtLogInfo("Master opened at FD:%d", PtmFd);
    LxtLogInfo("Subordinate Serial Number: %d", SerialNumber);
    LxtLogInfo("Subordinate opened at FD:%d", PtsFd);

    //
    // Mark the master non-blocking and write to it in a loop.
    // When it is out of room, it will return with EAGAIN.
    //

    fcntl(PtmFd, F_SETFL, O_NONBLOCK);
    LxtLogInfo("Filling up the buffer - this might take some time...");
    for (;;)
    {
        BytesReadWrite = write(PtmFd, WriteBuffer, WriteBufferLen);
        if (BytesReadWrite != WriteBufferLen)
        {
            break;
        }
    }

    //
    // Given the odd number of bytes written, it is expected for the write
    // to fail partway through the last transfer, returning a non-zero
    // number of bytes written.
    //
    // N.B. On Ubuntu16, because the buffer size grows asynchronously under
    //      pressure the buffer may be writeable again by this point.
    //

    if (BytesReadWrite < 0)
    {
        LxtLogError("Write failed with errno %d: %s", errno, strerror(errno));
        Result = -1;
        goto ErrorExit;
    }

    LxtLogInfo("Last write was %d bytes of the %d byte buffer", BytesReadWrite, WriteBufferLen);

    LxtCheckNotEqual(BytesReadWrite, WriteBufferLen, "%llu");

    memset(&Timeout, 0, sizeof(Timeout));
    Timeout.tv_sec = 2;
    FD_ZERO(&WriteFds);
    FD_SET(PtmFd, &WriteFds);
    LxtCheckErrno(select((PtmFd + 1), NULL, &WriteFds, NULL, &Timeout));
    LxtCheckEqual(Result, 1, "%d");

    LxtCheckErrno(BytesReadWrite = write(PtmFd, WriteBuffer2, WriteBufferLen));
    LxtLogInfo("Last write was %d bytes of the %d byte buffer", BytesReadWrite, WriteBufferLen);

    memset(&Timeout, 0, sizeof(Timeout));
    Timeout.tv_sec = 2;
    FD_ZERO(&WriteFds);
    FD_SET(PtmFd, &WriteFds);
    LxtCheckErrno(select((PtmFd + 1), NULL, &WriteFds, NULL, &Timeout));

    //
    // On Ubuntu16 the characters after the '\n' are added to the next
    // allocated page of terminal buffer. On WSL there is no dynamic allocation
    // so this will return after writing the '\n'. In both cases, the
    // characters before the '\n' are discarded by virtue of being replaced
    // by the subsequent character until finally hitting the '\n'.
    //

    if (BytesReadWrite == WriteBufferLen)
    {
        LxtCheckEqual(Result, 1, "%d");
    }
    else
    {
        LxtCheckEqual(Result, 0, "%d");
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

int PtControllingTerminalForeground(PLXT_ARGS Args)

/*++

Routine Description:

    This routine terminates the session leader of a terminal and checks various
    foreground behaviors.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    pid_t ChildPid;
    bool EndChildPidSynchronization;
    bool EndGrandChildPidSynchronization;
    pid_t ForegroundId;
    pid_t GrandChildPid;
    int PtmFd;
    int PtsFd;
    int Result;
    pid_t SelfPid;
    int Status;
    LXT_SYNCHRONIZATION_POINT_DECLARE_FOR(GrandChildPid);

    ChildPid = -1;
    EndChildPidSynchronization = true;
    EndGrandChildPidSynchronization = true;
    GrandChildPid = -1;
    PtmFd = -1;
    PtsFd = -1;

    LXT_SYNCHRONIZATION_POINT_INIT_FOR(GrandChildPid);
    LXT_SYNCHRONIZATION_POINT_START();
    LxtCheckErrno(ChildPid = ForkPty(&PtmFd, &PtsFd));
    if (ChildPid == 0)
    {
        LxtCheckResult(LxtSignalInitialize());
        LxtSignalSetAllowMultiple(TRUE);
        LxtCheckResult(LxtSignalSetupHandler(SIGHUP, SA_SIGINFO));

        //
        // Verify current foreground process group.
        //

        SelfPid = getpid();
        LxtCheckErrno(ForegroundId = tcgetpgrp(PtsFd));
        LxtCheckEqual(SelfPid, ForegroundId, "%d");

        LxtClose(PtmFd);
        PtmFd = -1;

        LXT_SYNCHRONIZATION_POINT_START_FOR(GrandChildPid);
        LxtCheckErrno(GrandChildPid = fork());
        if (GrandChildPid == 0)
        {
            EndChildPidSynchronization = false;
            LxtCheckResult(LxtSignalInitialize());
            LxtSignalSetAllowMultiple(TRUE);
            LxtCheckResult(LxtSignalSetupHandler(SIGHUP, SA_SIGINFO));

            //
            // Move to standalone process group.
            //

            LxtCheckErrno(setpgid(0, 0));

            //
            // Have parent set this process as foreground group.
            //

            LXT_SYNCHRONIZATION_POINT_FOR(GrandChildPid);
            LxtSignalWait();
            LxtCheckResult(LxtSignalCheckNoSignal());
            LXT_SYNCHRONIZATION_POINT_FOR(GrandChildPid);

            //
            // Wait for session leader to terminate.
            //

            LXT_SYNCHRONIZATION_POINT_FOR(GrandChildPid);
            EndChildPidSynchronization = true;
            LXT_SYNCHRONIZATION_POINT();
            LxtSignalWait();
            LxtCheckResult(LxtSignalCheckReceived(SIGHUP));
            LxtSignalResetReceived();
            LxtCheckErrnoFailure(tcgetpgrp(PtsFd), ENOTTY);
            LxtCheckErrnoFailure(tcgetsid(PtsFd), ENOTTY);

            //
            // Wait for master endpoint to close.
            //

            LXT_SYNCHRONIZATION_POINT();
            LxtSignalWait();
            LxtCheckResult(LxtSignalCheckNoSignal());
            LxtCheckErrnoFailure(tcgetpgrp(PtsFd), EIO);
            LxtCheckErrnoFailure(tcgetsid(PtsFd), EIO);
            LXT_SYNCHRONIZATION_POINT();
        }
        else
        {

            LXT_SYNCHRONIZATION_POINT_FOR(GrandChildPid);

            //
            // Set (grand)child as the foreground process group.
            //

            LxtCheckErrno(tcsetpgrp(PtsFd, GrandChildPid));
            LxtCheckErrno(ForegroundId = tcgetpgrp(PtsFd));
            LxtCheckEqual(GrandChildPid, ForegroundId, "%d");

            //
            // Terminating before child.
            //

            EndGrandChildPidSynchronization = false;

            //
            // Communication with parent is now via grandchild.
            //

            EndChildPidSynchronization = false;
            LXT_SYNCHRONIZATION_POINT_FOR(GrandChildPid);
            LXT_SYNCHRONIZATION_POINT();
            LXT_SYNCHRONIZATION_POINT_FOR(GrandChildPid);
            _exit(0);
        }
    }
    else
    {

        //
        // Wait for the child to terminate. The grandchild should still be
        // running.
        //

        LXT_SYNCHRONIZATION_POINT();
        EndChildPidSynchronization = false;
        LxtCheckErrno(TEMP_FAILURE_RETRY(Result = waitpid(ChildPid, &Status, 0)));

        EndChildPidSynchronization = FALSE;
        LxtCheckResult(WIFEXITED(Status) ? 0 : -1);
        LxtCheckResult((int)(char)WEXITSTATUS(Status));

        //
        // Signal grandchild that its parent has terminated.
        //

        LXT_SYNCHRONIZATION_POINT();

        //
        // Close the master endpoint and signal the grandchild.
        //

        LXT_SYNCHRONIZATION_POINT();
        LxtClose(PtmFd);
        PtmFd = -1;
        LXT_SYNCHRONIZATION_POINT();
    }

    Result = 0;

ErrorExit:
    LxtLogInfo("Thread exit: %d, Result=%d", getpid(), Result);
    if (PtmFd != -1)
    {
        close(PtmFd);
    }

    if (PtsFd != -1)
    {
        close(PtsFd);
    }

    if (EndGrandChildPidSynchronization)
    {
        LXT_SYNCHRONIZATION_POINT_END_FOR(GrandChildPid, TRUE);
    }

    if (EndChildPidSynchronization)
    {
        LXT_SYNCHRONIZATION_POINT_END();
    }

    return Result;
}

int PtControllingTerminalForeground2(PLXT_ARGS Args)

/*++

Routine Description:

    This routine closes the master endpoint of a terminal and checks various
    foreground behaviors.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    pid_t ChildPid;
    bool EndChildPidSynchronization;
    bool EndGrandChildPidSynchronization;
    pid_t ForegroundId;
    pid_t GrandChildPid;
    int PtmFd;
    int PtsFd;
    int Result;
    pid_t SelfPid;
    int Status;
    LXT_SYNCHRONIZATION_POINT_DECLARE_FOR(GrandChildPid);

    ChildPid = -1;
    EndChildPidSynchronization = true;
    EndGrandChildPidSynchronization = true;
    GrandChildPid = -1;
    PtmFd = -1;
    PtsFd = -1;

    LXT_SYNCHRONIZATION_POINT_INIT_FOR(GrandChildPid);
    LXT_SYNCHRONIZATION_POINT_START();
    LxtCheckErrno(ChildPid = ForkPty(&PtmFd, &PtsFd));
    if (ChildPid == 0)
    {
        LxtCheckResult(LxtSignalInitialize());
        LxtSignalSetAllowMultiple(TRUE);
        LxtCheckResult(LxtSignalSetupHandler(SIGHUP, SA_SIGINFO));

        //
        // Verify current foreground process group.
        //

        SelfPid = getpid();
        LxtCheckErrno(ForegroundId = tcgetpgrp(PtsFd));
        LxtCheckEqual(SelfPid, ForegroundId, "%d");

        LxtClose(PtmFd);
        PtmFd = -1;

        LXT_SYNCHRONIZATION_POINT_START_FOR(GrandChildPid);
        LxtCheckErrno(GrandChildPid = fork());
        if (GrandChildPid == 0)
        {
            EndChildPidSynchronization = false;
            LxtCheckResult(LxtSignalInitialize());
            LxtSignalSetAllowMultiple(TRUE);
            LxtCheckResult(LxtSignalSetupHandler(SIGHUP, SA_SIGINFO));

            //
            // Move to standalone process group.
            //

            LxtCheckErrno(setpgid(0, 0));

            //
            // Have parent set this process as foreground group and close the
            // master endpoint.
            //

            LXT_SYNCHRONIZATION_POINT_FOR(GrandChildPid);
            LXT_SYNCHRONIZATION_POINT_FOR(GrandChildPid);
            LxtSignalWait();
            LxtCheckResult(LxtSignalCheckNoSignal());

            //
            // Signal parent to terminate and take over communication with
            // grand-parent.
            //

            LXT_SYNCHRONIZATION_POINT_FOR(GrandChildPid);
            LxtCheckErrnoFailure(tcgetpgrp(PtsFd), EIO);
            LxtCheckErrnoFailure(tcgetsid(PtsFd), EIO);

            LXT_SYNCHRONIZATION_POINT_FOR(GrandChildPid);
            EndChildPidSynchronization = true;
            LxtSignalWait();
            LxtCheckResult(LxtSignalCheckReceived(SIGHUP));
            LxtSignalResetReceived();
            LxtCheckErrnoFailure(tcgetpgrp(PtsFd), EIO);
            LxtCheckErrnoFailure(tcgetsid(PtsFd), EIO);

            //
            // Wait for session leader to terminate.
            //

            LXT_SYNCHRONIZATION_POINT();
            LxtSignalWait();
            LxtCheckResult(LxtSignalCheckNoSignal());
            LxtCheckErrnoFailure(tcgetpgrp(PtsFd), EIO);
            LxtCheckErrnoFailure(tcgetsid(PtsFd), EIO);
        }
        else
        {

            LXT_SYNCHRONIZATION_POINT_FOR(GrandChildPid);

            //
            // Set (grand)child as the foreground process group.
            //

            LxtCheckErrno(tcsetpgrp(PtsFd, GrandChildPid));

            //
            // Signal parent to close last master endpoint descriptor.
            //

            LXT_SYNCHRONIZATION_POINT();
            LXT_SYNCHRONIZATION_POINT_FOR(GrandChildPid);
            LxtSignalWait();
            LxtCheckResult(LxtSignalCheckReceived(SIGHUP));
            LxtSignalResetReceived();
            LxtCheckErrnoFailure(tcgetpgrp(PtsFd), EIO);
            LxtCheckErrnoFailure(tcgetsid(PtsFd), EIO);
            LXT_SYNCHRONIZATION_POINT_FOR(GrandChildPid);

            //
            // Terminating before child.
            //

            EndGrandChildPidSynchronization = false;

            //
            // Communication with parent is now via grandchild.
            //

            EndChildPidSynchronization = false;
            LXT_SYNCHRONIZATION_POINT();
            LXT_SYNCHRONIZATION_POINT_FOR(GrandChildPid);
            _exit(0);
        }
    }
    else
    {
        LXT_SYNCHRONIZATION_POINT();
        LxtClose(PtmFd);
        PtmFd = -1;
        LXT_SYNCHRONIZATION_POINT();

        //
        // Wait for the child to terminate. The grandchild should still be
        // running.
        //

        EndChildPidSynchronization = false;
        LxtCheckErrno(TEMP_FAILURE_RETRY(Result = waitpid(ChildPid, &Status, 0)));

        LxtCheckResult(WIFEXITED(Status) ? 0 : -1);
        LxtCheckResult((int)(char)WEXITSTATUS(Status));

        //
        // Signal grandchild that its parent has terminated.
        //

        LXT_SYNCHRONIZATION_POINT();
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

    if (EndGrandChildPidSynchronization)
    {
        LXT_SYNCHRONIZATION_POINT_END_FOR(GrandChildPid, TRUE);
    }

    if (EndChildPidSynchronization)
    {
        LXT_SYNCHRONIZATION_POINT_END();
    }

    return Result;
}

int PtControllingTerminalForeground3(PLXT_ARGS Args)

/*++

Routine Description:

    This routine disconnects the terminal via TIOCNOTTY and checks various
    foreground behaviors.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    pid_t ChildPid;
    bool EndChildPidSynchronization;
    bool EndGrandChildPidSynchronization;
    pid_t ForegroundId;
    pid_t GrandChildPid;
    int PtmFd;
    int PtsFd;
    int Result;
    pid_t SelfPid;
    int Status;
    LXT_SYNCHRONIZATION_POINT_DECLARE_FOR(GrandChildPid);

    ChildPid = -1;
    EndChildPidSynchronization = true;
    EndGrandChildPidSynchronization = true;
    GrandChildPid = -1;
    PtmFd = -1;
    PtsFd = -1;

    LXT_SYNCHRONIZATION_POINT_INIT_FOR(GrandChildPid);
    LXT_SYNCHRONIZATION_POINT_START();
    LxtCheckErrno(ChildPid = ForkPty(&PtmFd, &PtsFd));
    if (ChildPid == 0)
    {
        LxtCheckResult(LxtSignalInitialize());
        LxtSignalSetAllowMultiple(TRUE);
        LxtCheckResult(LxtSignalSetupHandler(SIGHUP, SA_SIGINFO));

        //
        // Verify current foreground process group.
        //

        SelfPid = getpid();
        LxtCheckErrno(ForegroundId = tcgetpgrp(PtsFd));
        LxtCheckEqual(SelfPid, ForegroundId, "%d");

        LxtClose(PtmFd);
        PtmFd = -1;

        LXT_SYNCHRONIZATION_POINT_START_FOR(GrandChildPid);
        LxtCheckErrno(GrandChildPid = fork());
        if (GrandChildPid == 0)
        {
            EndChildPidSynchronization = false;
            LxtCheckResult(LxtSignalInitialize());
            LxtSignalSetAllowMultiple(TRUE);
            LxtCheckResult(LxtSignalSetupHandler(SIGHUP, SA_SIGINFO));

            //
            // Move to standalone process group.
            //

            LxtCheckErrno(setpgid(0, 0));

            //
            // Have parent set this process as foreground group and disconnect
            // the session terminal.
            //

            LXT_SYNCHRONIZATION_POINT_FOR(GrandChildPid);
            LXT_SYNCHRONIZATION_POINT_FOR(GrandChildPid);
            LxtSignalWait();
            LxtCheckResult(LxtSignalCheckReceived(SIGHUP));
            LxtSignalResetReceived();
            LxtCheckErrnoFailure(tcgetpgrp(PtsFd), ENOTTY);
            LxtCheckErrnoFailure(tcgetsid(PtsFd), ENOTTY);

            //
            // Signal parent to close last descriptor.
            //

            LXT_SYNCHRONIZATION_POINT_FOR(GrandChildPid);
            LXT_SYNCHRONIZATION_POINT_FOR(GrandChildPid);
            EndChildPidSynchronization = true;
            LxtSignalWait();
            LxtCheckResult(LxtSignalCheckNoSignal());
            LxtCheckErrnoFailure(tcgetpgrp(PtsFd), EIO);
            LxtCheckErrnoFailure(tcgetsid(PtsFd), EIO);

            //
            // Wait for session leader to terminate.
            //

            LXT_SYNCHRONIZATION_POINT_FOR(GrandChildPid);
            LXT_SYNCHRONIZATION_POINT_FOR(GrandChildPid);
            LXT_SYNCHRONIZATION_POINT();
            LxtSignalWait();
            LxtCheckResult(LxtSignalCheckNoSignal());
            LxtCheckErrnoFailure(tcgetpgrp(PtsFd), EIO);
            LxtCheckErrnoFailure(tcgetsid(PtsFd), EIO);
        }
        else
        {

            LXT_SYNCHRONIZATION_POINT_FOR(GrandChildPid);

            //
            // Set (grand)child as the foreground process group.
            //

            LxtCheckErrno(tcsetpgrp(PtsFd, GrandChildPid));

            //
            // Disassociate terminal.
            //

            LxtCheckErrno(ioctl(PtsFd, TIOCNOTTY, (char*)NULL));
            LXT_SYNCHRONIZATION_POINT_FOR(GrandChildPid);
            LXT_SYNCHRONIZATION_POINT_FOR(GrandChildPid);

            //
            // Signal parent to close last master endpoint descriptor. No
            // signal is expected because the terminal has been disconnected.
            //

            LXT_SYNCHRONIZATION_POINT();
            LXT_SYNCHRONIZATION_POINT_FOR(GrandChildPid);
            LxtSignalWait();
            LxtCheckResult(LxtSignalCheckNoSignal());
            LxtCheckErrnoFailure(tcgetpgrp(PtsFd), EIO);
            LxtCheckErrnoFailure(tcgetsid(PtsFd), EIO);
            LXT_SYNCHRONIZATION_POINT_FOR(GrandChildPid);

            //
            // Terminating before child.
            //

            EndGrandChildPidSynchronization = false;

            //
            // Communication with parent is now via grandchild.
            //

            EndChildPidSynchronization = false;
            LXT_SYNCHRONIZATION_POINT();
            LXT_SYNCHRONIZATION_POINT_FOR(GrandChildPid);
            _exit(0);
        }
    }
    else
    {
        LXT_SYNCHRONIZATION_POINT();
        LxtClose(PtmFd);
        PtmFd = -1;
        LXT_SYNCHRONIZATION_POINT();

        //
        // Wait for the child to terminate. The grandchild should still be
        // running.
        //

        EndChildPidSynchronization = false;
        LxtCheckErrno(TEMP_FAILURE_RETRY(Result = waitpid(ChildPid, &Status, 0)));

        LxtCheckResult(WIFEXITED(Status) ? 0 : -1);
        LxtCheckResult((int)(char)WEXITSTATUS(Status));

        //
        // Signal grandchild that its parent has terminated.
        //

        LXT_SYNCHRONIZATION_POINT();
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

    if (EndGrandChildPidSynchronization)
    {
        LXT_SYNCHRONIZATION_POINT_END_FOR(GrandChildPid, TRUE);
    }

    if (EndChildPidSynchronization)
    {
        LXT_SYNCHRONIZATION_POINT_END();
    }

    return Result;
}

int PtControllingTerminalForeground4(PLXT_ARGS Args)

/*++

Routine Description:

    This routine closes the master endpoint of a terminal where the session
    leader is ignoring SIGHUP, and checks various foreground behaviors.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    pid_t ChildPid;
    bool EndChildPidSynchronization;
    bool EndGrandChildPidSynchronization;
    pid_t ForegroundId;
    pid_t GrandChildPid;
    int PtmFd;
    int PtsFd;
    int Result;
    pid_t SelfPid;
    int Status;
    LXT_SYNCHRONIZATION_POINT_DECLARE_FOR(GrandChildPid);

    ChildPid = -1;
    EndChildPidSynchronization = true;
    EndGrandChildPidSynchronization = true;
    GrandChildPid = -1;
    PtmFd = -1;
    PtsFd = -1;

    LXT_SYNCHRONIZATION_POINT_INIT_FOR(GrandChildPid);
    LXT_SYNCHRONIZATION_POINT_START();
    LxtCheckErrno(ChildPid = ForkPty(&PtmFd, &PtsFd));
    if (ChildPid == 0)
    {
        LxtCheckResult(LxtSignalInitialize());
        LxtSignalSetAllowMultiple(TRUE);
        LxtCheckResult(LxtSignalSetupHandler(SIGHUP, SA_SIGINFO));

        //
        // Verify current foreground process group.
        //

        SelfPid = getpid();
        LxtCheckErrno(ForegroundId = tcgetpgrp(PtsFd));
        LxtCheckEqual(SelfPid, ForegroundId, "%d");

        LxtClose(PtmFd);
        PtmFd = -1;

        LXT_SYNCHRONIZATION_POINT_START_FOR(GrandChildPid);
        LxtCheckErrno(GrandChildPid = fork());
        if (GrandChildPid == 0)
        {
            EndChildPidSynchronization = false;
            LxtCheckResult(LxtSignalInitialize());
            LxtSignalSetAllowMultiple(TRUE);
            LxtCheckResult(LxtSignalSetupHandler(SIGHUP, SA_SIGINFO));

            //
            // Move to standalone process group.
            //

            LxtCheckErrno(setpgid(0, 0));

            //
            // Have parent set this process as foreground group and close the
            // master endpoint.
            //

            LXT_SYNCHRONIZATION_POINT_FOR(GrandChildPid);
            LXT_SYNCHRONIZATION_POINT_FOR(GrandChildPid);
            LxtSignalWait();
            LxtCheckResult(LxtSignalCheckNoSignal());

            //
            // Signal parent to terminate and take over communication with
            // grand-parent.
            //

            LXT_SYNCHRONIZATION_POINT_FOR(GrandChildPid);
            LxtCheckErrnoFailure(tcgetpgrp(PtsFd), EIO);
            LxtCheckErrnoFailure(tcgetsid(PtsFd), EIO);

            LXT_SYNCHRONIZATION_POINT_FOR(GrandChildPid);
            EndChildPidSynchronization = true;
            LxtSignalWait();
            LxtCheckResult(LxtSignalCheckReceived(SIGHUP));
            LxtSignalResetReceived();
            LxtCheckErrnoFailure(tcgetpgrp(PtsFd), EIO);
            LxtCheckErrnoFailure(tcgetsid(PtsFd), EIO);

            //
            // Wait for session leader to terminate.
            //

            LXT_SYNCHRONIZATION_POINT();
            LxtSignalWait();
            LxtCheckResult(LxtSignalCheckNoSignal());
            LxtCheckErrnoFailure(tcgetpgrp(PtsFd), EIO);
            LxtCheckErrnoFailure(tcgetsid(PtsFd), EIO);
        }
        else
        {

            LXT_SYNCHRONIZATION_POINT_FOR(GrandChildPid);

            //
            // Set (grand)child as the foreground process group.
            //

            LxtCheckErrno(tcsetpgrp(PtsFd, GrandChildPid));

            //
            // Ignore SIGHUP on the session leader.
            //

            LxtCheckErrno(LxtSignalIgnore(SIGHUP));

            //
            // Signal parent to close last master endpoint descriptor.
            //

            LXT_SYNCHRONIZATION_POINT();
            LXT_SYNCHRONIZATION_POINT_FOR(GrandChildPid);
            LxtSignalWait();
            LxtCheckResult(LxtSignalCheckNoSignal());
            LxtCheckErrnoFailure(tcgetpgrp(PtsFd), EIO);
            LxtCheckErrnoFailure(tcgetsid(PtsFd), EIO);
            LXT_SYNCHRONIZATION_POINT_FOR(GrandChildPid);

            //
            // Terminating before child.
            //

            EndGrandChildPidSynchronization = false;

            //
            // Communication with parent is now via grandchild.
            //

            EndChildPidSynchronization = false;
            LXT_SYNCHRONIZATION_POINT();
            LXT_SYNCHRONIZATION_POINT_FOR(GrandChildPid);
            _exit(0);
        }
    }
    else
    {
        LXT_SYNCHRONIZATION_POINT();
        LxtClose(PtmFd);
        PtmFd = -1;
        LXT_SYNCHRONIZATION_POINT();

        //
        // Wait for the child to terminate. The grandchild should still be
        // running.
        //

        EndChildPidSynchronization = false;
        LxtCheckErrno(TEMP_FAILURE_RETRY(Result = waitpid(ChildPid, &Status, 0)));

        LxtCheckResult(WIFEXITED(Status) ? 0 : -1);
        LxtCheckResult((int)(char)WEXITSTATUS(Status));

        //
        // Signal grandchild that its parent has terminated.
        //

        LXT_SYNCHRONIZATION_POINT();
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

    if (EndGrandChildPidSynchronization)
    {
        LXT_SYNCHRONIZATION_POINT_END_FOR(GrandChildPid, TRUE);
    }

    if (EndChildPidSynchronization)
    {
        LXT_SYNCHRONIZATION_POINT_END();
    }

    return Result;
}

int PtControllingTerminalForeground5(PLXT_ARGS Args)

/*++

Routine Description:

    This routine connects a second process to the current foreground process
    group and checks various foreground properties.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    pid_t ChildPid;
    bool EndChildPidSynchronization;
    pid_t ForegroundId;
    pid_t GrandChildPid;
    pid_t GrandChildPid2;
    int PtmFd;
    int PtsFd;
    int Result;
    pid_t SelfPid;
    int Status;
    LXT_SYNCHRONIZATION_POINT_DECLARE_FOR(GrandChildPid);
    LXT_SYNCHRONIZATION_POINT_DECLARE_FOR(GrandChildPid2);

    ChildPid = -1;
    EndChildPidSynchronization = true;
    GrandChildPid = -1;
    GrandChildPid2 = -1;
    PtmFd = -1;
    PtsFd = -1;

    LXT_SYNCHRONIZATION_POINT_INIT_FOR(GrandChildPid);
    LXT_SYNCHRONIZATION_POINT_INIT_FOR(GrandChildPid2);
    LXT_SYNCHRONIZATION_POINT_START();
    LxtCheckErrno(ChildPid = ForkPty(&PtmFd, &PtsFd));
    if (ChildPid == 0)
    {
        LxtCheckResult(LxtSignalInitialize());
        LxtSignalSetAllowMultiple(TRUE);
        LxtCheckResult(LxtSignalSetupHandler(SIGHUP, SA_SIGINFO));

        //
        // Verify current foreground process group.
        //

        SelfPid = getpid();
        LxtCheckErrno(ForegroundId = tcgetpgrp(PtsFd));
        LxtCheckEqual(SelfPid, ForegroundId, "%d");

        LxtClose(PtmFd);
        PtmFd = -1;

        LXT_SYNCHRONIZATION_POINT_START_FOR(GrandChildPid);
        LxtCheckErrno(GrandChildPid = fork());
        if (GrandChildPid == 0)
        {
            EndChildPidSynchronization = false;
            LxtCheckResult(LxtSignalInitialize());
            LxtSignalSetAllowMultiple(TRUE);
            LxtCheckResult(LxtSignalSetupHandler(SIGHUP, SA_SIGINFO));

            //
            // Move to standalone process group.
            //

            LxtCheckErrno(setpgid(0, 0));

            //
            // Have parent set this process as foreground group.
            //

            LXT_SYNCHRONIZATION_POINT_FOR(GrandChildPid);

            //
            // Wait for test to finish before terminating.
            //

            LXT_SYNCHRONIZATION_POINT_FOR(GrandChildPid);
            LxtCheckResult(LxtSignalCheckReceived(SIGHUP));
            goto ErrorExit;
        }

        LXT_SYNCHRONIZATION_POINT_FOR(GrandChildPid);

        //
        // Set (grand)child as the foreground process group.
        //

        LxtCheckErrno(tcsetpgrp(PtsFd, GrandChildPid));
        LxtCheckErrno(ForegroundId = tcgetpgrp(PtsFd));
        LxtCheckEqual(ForegroundId, GrandChildPid, "%d");
        LxtCheckErrno(tcgetsid(PtsFd));

        //
        // Start another child and try to connect to previous process group.
        //

        LXT_SYNCHRONIZATION_POINT_START_FOR(GrandChildPid2);
        LxtCheckErrno(GrandChildPid2 = fork());
        if (GrandChildPid2 == 0)
        {
            EndChildPidSynchronization = false;
            LxtCheckResult(LxtSignalInitialize());
            LxtSignalSetAllowMultiple(TRUE);
            LxtCheckResult(LxtSignalSetupHandler(SIGHUP, SA_SIGINFO));

            //
            // Attempt to move to previously created process group.
            //

            LxtCheckErrno(setpgid(0, GrandChildPid));

            //
            // Signal parent to disconnect from the terminal.
            //

            LXT_SYNCHRONIZATION_POINT_FOR(GrandChildPid2);
            LXT_SYNCHRONIZATION_POINT_FOR(GrandChildPid2);

            LxtSignalWait();
            LxtCheckResult(LxtSignalCheckReceived(SIGHUP));
            LxtSignalResetReceived();
            LxtCheckErrnoFailure(tcgetpgrp(PtsFd), ENOTTY);
            LxtCheckErrnoFailure(tcgetsid(PtsFd), ENOTTY);

            //
            // Signal parent to close the terminal descriptor.
            //

            LXT_SYNCHRONIZATION_POINT_FOR(GrandChildPid2);
            LXT_SYNCHRONIZATION_POINT_FOR(GrandChildPid2);

            LxtSignalWait();
            LxtCheckResult(LxtSignalCheckNoSignal());
            LxtCheckErrnoFailure(tcgetpgrp(PtsFd), EIO);
            LxtCheckErrnoFailure(tcgetsid(PtsFd), EIO);

            //
            // Terminate.
            //

            goto ErrorExit;
        }

        LXT_SYNCHRONIZATION_POINT_FOR(GrandChildPid2);

        //
        // Disconnect terminal.
        //

        LxtCheckErrno(ioctl(PtsFd, TIOCNOTTY, (char*)NULL));
        LxtSignalWait();
        LxtCheckResult(LxtSignalCheckNoSignal());
        LXT_SYNCHRONIZATION_POINT_FOR(GrandChildPid2);

        //
        // Signal parent to close last master endpoint descriptor.
        //

        LXT_SYNCHRONIZATION_POINT_FOR(GrandChildPid2);
        LXT_SYNCHRONIZATION_POINT();
        LxtSignalWait();
        LxtCheckResult(LxtSignalCheckNoSignal());
        LXT_SYNCHRONIZATION_POINT_FOR(GrandChildPid2);
        LxtCheckErrnoFailure(tcgetpgrp(PtsFd), EIO);
        LxtCheckErrnoFailure(tcgetsid(PtsFd), EIO);

        //
        // Signal original child to exit.
        //

        LXT_SYNCHRONIZATION_POINT_FOR(GrandChildPid);
    }
    else
    {
        LXT_SYNCHRONIZATION_POINT();
        LxtClose(PtmFd);
        PtmFd = -1;
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

    LXT_SYNCHRONIZATION_POINT_END_FOR(GrandChildPid2, TRUE);
    LXT_SYNCHRONIZATION_POINT_END_FOR(GrandChildPid, TRUE);
    if (EndChildPidSynchronization)
    {
        LXT_SYNCHRONIZATION_POINT_END();
    }

    return Result;
}

int PtControllingTerminalForeground6(PLXT_ARGS Args)

/*++

Routine Description:

    This routine terminates the current foreground process group and checks
    various foreground properties.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    pid_t ChildPid;
    bool EndChildPidSynchronization;
    bool EndGrandChildPidSynchronization;
    pid_t ForegroundId;
    pid_t GrandChildPid;
    pid_t GrandChildPid2;
    int PtmFd;
    int PtsFd;
    int Result;
    pid_t SelfPid;
    int Status;
    LXT_SYNCHRONIZATION_POINT_DECLARE_FOR(GrandChildPid);
    LXT_SYNCHRONIZATION_POINT_DECLARE_FOR(GrandChildPid2);

    ChildPid = -1;
    EndChildPidSynchronization = true;
    EndGrandChildPidSynchronization = true;
    GrandChildPid = -1;
    GrandChildPid2 = -1;
    PtmFd = -1;
    PtsFd = -1;

    LXT_SYNCHRONIZATION_POINT_INIT_FOR(GrandChildPid);
    LXT_SYNCHRONIZATION_POINT_INIT_FOR(GrandChildPid2);
    LXT_SYNCHRONIZATION_POINT_START();
    LxtCheckErrno(ChildPid = ForkPty(&PtmFd, &PtsFd));
    if (ChildPid == 0)
    {
        LxtCheckResult(LxtSignalInitialize());
        LxtSignalSetAllowMultiple(TRUE);
        LxtCheckResult(LxtSignalSetupHandler(SIGHUP, SA_SIGINFO));

        //
        // Verify current foreground process group.
        //

        SelfPid = getpid();
        LxtCheckErrno(ForegroundId = tcgetpgrp(PtsFd));
        LxtCheckEqual(SelfPid, ForegroundId, "%d");

        LxtClose(PtmFd);
        PtmFd = -1;

        LXT_SYNCHRONIZATION_POINT_START_FOR(GrandChildPid);
        LxtCheckErrno(GrandChildPid = fork());
        if (GrandChildPid == 0)
        {
            EndChildPidSynchronization = false;
            LxtCheckResult(LxtSignalInitialize());
            LxtSignalSetAllowMultiple(TRUE);
            LxtCheckResult(LxtSignalSetupHandler(SIGHUP, SA_SIGINFO));

            //
            // Move to standalone process group.
            //

            LxtCheckErrno(setpgid(0, 0));

            //
            // Have parent set this process as foreground group.
            //

            LXT_SYNCHRONIZATION_POINT_FOR(GrandChildPid);
            LXT_SYNCHRONIZATION_POINT_FOR(GrandChildPid);

            //
            // Terminate.
            //

            goto ErrorExit;
        }

        LXT_SYNCHRONIZATION_POINT_FOR(GrandChildPid);

        //
        // Set (grand)child as the foreground process group.
        //

        LxtCheckErrno(tcsetpgrp(PtsFd, GrandChildPid));

        //
        // Wait for child to terminate.
        //

        LXT_SYNCHRONIZATION_POINT_FOR(GrandChildPid);
        EndGrandChildPidSynchronization = false;
        LxtCheckErrno(TEMP_FAILURE_RETRY(Result = waitpid(GrandChildPid, &Status, 0)));

        LxtCheckResult(WIFEXITED(Status) ? 0 : -1);
        LxtCheckResult((int)(char)WEXITSTATUS(Status));

        LxtCheckErrno(ForegroundId = tcgetpgrp(PtsFd));
        LxtCheckEqual(ForegroundId, GrandChildPid, "%d");
        LxtCheckErrno(tcgetsid(PtsFd));

        //
        // Start another child and try to connect to previous process group.
        //

        LXT_SYNCHRONIZATION_POINT_START_FOR(GrandChildPid2);
        LxtCheckErrno(GrandChildPid2 = fork());
        if (GrandChildPid2 == 0)
        {
            EndChildPidSynchronization = false;
            LxtCheckResult(LxtSignalInitialize());
            LxtSignalSetAllowMultiple(TRUE);
            LxtCheckResult(LxtSignalSetupHandler(SIGHUP, SA_SIGINFO));

            //
            // Attempt to move to previously created process group of now
            // terminated process.
            //

            LxtCheckErrnoFailure(setpgid(0, GrandChildPid), EPERM);

            //
            // Signal parent to disconnect from the terminal.
            //

            LXT_SYNCHRONIZATION_POINT_FOR(GrandChildPid2);
            LXT_SYNCHRONIZATION_POINT_FOR(GrandChildPid2);

            LxtSignalWait();
            LxtCheckResult(LxtSignalCheckNoSignal());
            LxtCheckErrnoFailure(tcgetpgrp(PtsFd), ENOTTY);
            LxtCheckErrnoFailure(tcgetsid(PtsFd), ENOTTY);

            //
            // Signal parent to close the terminal descriptor.
            //

            LXT_SYNCHRONIZATION_POINT_FOR(GrandChildPid2);
            LXT_SYNCHRONIZATION_POINT_FOR(GrandChildPid2);

            LxtSignalWait();
            LxtCheckResult(LxtSignalCheckNoSignal());
            LxtCheckErrnoFailure(tcgetpgrp(PtsFd), EIO);
            LxtCheckErrnoFailure(tcgetsid(PtsFd), EIO);

            //
            // Terminate.
            //

            goto ErrorExit;
        }

        LXT_SYNCHRONIZATION_POINT_FOR(GrandChildPid2);

        //
        // Disconnect terminal.
        //

        LxtCheckErrno(ioctl(PtsFd, TIOCNOTTY, (char*)NULL));
        LxtSignalWait();
        LxtCheckResult(LxtSignalCheckNoSignal());
        LXT_SYNCHRONIZATION_POINT_FOR(GrandChildPid2);

        //
        // Signal parent to close last master endpoint descriptor.
        //

        LXT_SYNCHRONIZATION_POINT_FOR(GrandChildPid2);
        LXT_SYNCHRONIZATION_POINT();
        LxtSignalWait();
        LXT_SYNCHRONIZATION_POINT_FOR(GrandChildPid2);
        LxtCheckErrnoFailure(tcgetpgrp(PtsFd), EIO);
        LxtCheckErrnoFailure(tcgetsid(PtsFd), EIO);
    }
    else
    {
        LXT_SYNCHRONIZATION_POINT();
        LxtClose(PtmFd);
        PtmFd = -1;
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

    LXT_SYNCHRONIZATION_POINT_END_FOR(GrandChildPid2, TRUE);
    if (EndGrandChildPidSynchronization)
    {
        LXT_SYNCHRONIZATION_POINT_END_FOR(GrandChildPid, TRUE);
    }

    if (EndChildPidSynchronization)
    {
        LXT_SYNCHRONIZATION_POINT_END();
    }

    return Result;
}

int PtControllingTerminalForeground7(PLXT_ARGS Args)

/*++

Routine Description:

    This routine connects a second process to an existing foreground group,
    disconnects it from the controlling terminal and then tests various
    properties.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    pid_t ChildPid;
    bool EndChildPidSynchronization;
    pid_t ForegroundId;
    pid_t GrandChildPid;
    pid_t GrandChildPid2;
    int PtmFd;
    int PtsFd;
    int Result;
    pid_t SelfPid;
    int Status;
    LXT_SYNCHRONIZATION_POINT_DECLARE_FOR(GrandChildPid);
    LXT_SYNCHRONIZATION_POINT_DECLARE_FOR(GrandChildPid2);

    ChildPid = -1;
    EndChildPidSynchronization = true;
    GrandChildPid = -1;
    GrandChildPid2 = -1;
    PtmFd = -1;
    PtsFd = -1;

    LXT_SYNCHRONIZATION_POINT_INIT_FOR(GrandChildPid);
    LXT_SYNCHRONIZATION_POINT_INIT_FOR(GrandChildPid2);
    LXT_SYNCHRONIZATION_POINT_START();
    LxtCheckErrno(ChildPid = ForkPty(&PtmFd, &PtsFd));
    if (ChildPid == 0)
    {
        LxtCheckResult(LxtSignalInitialize());
        LxtSignalSetAllowMultiple(TRUE);
        LxtCheckResult(LxtSignalSetupHandler(SIGHUP, SA_SIGINFO));

        //
        // Verify current foreground process group.
        //

        SelfPid = getpid();
        LxtCheckErrno(ForegroundId = tcgetpgrp(PtsFd));
        LxtCheckEqual(SelfPid, ForegroundId, "%d");

        LxtClose(PtmFd);
        PtmFd = -1;

        LXT_SYNCHRONIZATION_POINT_START_FOR(GrandChildPid);
        LxtCheckErrno(GrandChildPid = fork());
        if (GrandChildPid == 0)
        {
            EndChildPidSynchronization = false;
            LxtCheckResult(LxtSignalInitialize());
            LxtSignalSetAllowMultiple(TRUE);
            LxtCheckResult(LxtSignalSetupHandler(SIGHUP, SA_SIGINFO));

            //
            // Move to standalone process group.
            //

            LxtCheckErrno(setpgid(0, 0));

            //
            // Have parent set this process as foreground group.
            //

            LXT_SYNCHRONIZATION_POINT_FOR(GrandChildPid);

            //
            // Wait for test to finish before terminating.
            //

            LXT_SYNCHRONIZATION_POINT_FOR(GrandChildPid);
            LxtCheckResult(LxtSignalCheckReceived(SIGHUP));
            goto ErrorExit;
        }

        LXT_SYNCHRONIZATION_POINT_FOR(GrandChildPid);

        //
        // Set (grand)child as the foreground process group.
        //

        LxtCheckErrno(tcsetpgrp(PtsFd, GrandChildPid));
        LxtCheckErrno(ForegroundId = tcgetpgrp(PtsFd));
        LxtCheckEqual(ForegroundId, GrandChildPid, "%d");
        LxtCheckErrno(tcgetsid(PtsFd));

        //
        // Start another child and try to connect to previous process group.
        //

        LXT_SYNCHRONIZATION_POINT_START_FOR(GrandChildPid2);
        LxtCheckErrno(GrandChildPid2 = fork());
        if (GrandChildPid2 == 0)
        {
            EndChildPidSynchronization = false;
            LxtCheckResult(LxtSignalInitialize());
            LxtSignalSetAllowMultiple(TRUE);
            LxtCheckResult(LxtSignalSetupHandler(SIGHUP, SA_SIGINFO));

            //
            // Attempt to move to previously created process group.
            //

            LxtCheckErrno(setpgid(0, GrandChildPid));

            //
            // Disconnect from the controlling terminal.
            //

            LxtCheckErrno(ioctl(PtsFd, TIOCNOTTY, (char*)NULL));

            //
            // Signal parent to disconnect from the terminal.
            //

            LXT_SYNCHRONIZATION_POINT_FOR(GrandChildPid2);
            LXT_SYNCHRONIZATION_POINT_FOR(GrandChildPid2);

            LxtSignalWait();
            LxtCheckResult(LxtSignalCheckReceived(SIGHUP));
            LxtSignalResetReceived();
            LxtCheckErrnoFailure(tcgetpgrp(PtsFd), ENOTTY);
            LxtCheckErrnoFailure(tcgetsid(PtsFd), ENOTTY);

            //
            // Signal parent to close the terminal descriptor.
            //

            LXT_SYNCHRONIZATION_POINT_FOR(GrandChildPid2);
            LXT_SYNCHRONIZATION_POINT_FOR(GrandChildPid2);

            LxtSignalWait();
            LxtCheckResult(LxtSignalCheckNoSignal());
            LxtCheckErrnoFailure(tcgetpgrp(PtsFd), EIO);
            LxtCheckErrnoFailure(tcgetsid(PtsFd), EIO);

            //
            // Terminate.
            //

            goto ErrorExit;
        }

        LXT_SYNCHRONIZATION_POINT_FOR(GrandChildPid2);

        //
        // Disconnect terminal.
        //

        LxtCheckErrno(ioctl(PtsFd, TIOCNOTTY, (char*)NULL));
        LxtSignalWait();
        LxtCheckResult(LxtSignalCheckNoSignal());
        LXT_SYNCHRONIZATION_POINT_FOR(GrandChildPid2);

        //
        // Signal parent to close last master endpoint descriptor.
        //

        LXT_SYNCHRONIZATION_POINT_FOR(GrandChildPid2);
        LXT_SYNCHRONIZATION_POINT();
        LxtSignalWait();
        LxtCheckResult(LxtSignalCheckNoSignal());
        LXT_SYNCHRONIZATION_POINT_FOR(GrandChildPid2);
        LxtCheckErrnoFailure(tcgetpgrp(PtsFd), EIO);
        LxtCheckErrnoFailure(tcgetsid(PtsFd), EIO);

        //
        // Signal original child to exit.
        //

        LXT_SYNCHRONIZATION_POINT_FOR(GrandChildPid);
    }
    else
    {
        LXT_SYNCHRONIZATION_POINT();
        LxtClose(PtmFd);
        PtmFd = -1;
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

    LXT_SYNCHRONIZATION_POINT_END_FOR(GrandChildPid2, TRUE);
    LXT_SYNCHRONIZATION_POINT_END_FOR(GrandChildPid, TRUE);
    if (EndChildPidSynchronization)
    {
        LXT_SYNCHRONIZATION_POINT_END();
    }

    return Result;
}

int PtMountBasic(PLXT_ARGS Args)

/*++

Routine Description:

    This routine verifies basic mount operations on the devpts file system.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    gid_t CurrentGid;
    uid_t CurrentUid;
    char EndpointName[sizeof(PTS_TEST_MNT) + 4];
    struct stat EndpointStat;
    int PtmFd;
    int PtmFd2;
    char* PtmxName = PTS_TEST_MNT "/ptmx";
    struct stat PtmxStat;
    int PtsFd;
    int Result;
    int SerialNumber;

    PtmFd = -1;
    PtmFd2 = -1;
    PtsFd = -1;

    CurrentUid = geteuid();
    CurrentGid = getegid();

    //
    // Create an endpoint to test default vs new mounts.
    //

    LxtCheckErrno(OpenMasterSubordinate(&PtmFd, &PtsFd, NULL, &SerialNumber));
    if (SerialNumber > 999)
    {
        LxtLogError("Unexpectedly large number of opened ptys!");
        Result = -1;
        goto ErrorExit;
    }

    sprintf(EndpointName, "%s/%d", PTS_TEST_MNT, SerialNumber);

    //
    // Create a temporary directory to create mounts.
    //

    LxtCheckErrnoZeroSuccess(mkdir(PTS_TEST_MNT, 0777));

    //
    // Mount the default devpts instance.
    //

    LxtCheckErrnoZeroSuccess(mount(NULL, PTS_TEST_MNT, "devpts", MS_NOEXEC | MS_NOSUID | MS_RELATIME, NULL));

    //
    // Verify previously created endpoint exists in the new mount.
    //

    LxtCheckErrno(stat(EndpointName, &EndpointStat));
    LxtCheckErrno(stat(PtmxName, &PtmxStat));
    LxtCheckErrno(umount(PTS_TEST_MNT));

    //
    // Mount with "newinstance" option.
    //

    LxtCheckErrnoZeroSuccess(mount(NULL, PTS_TEST_MNT, "devpts", MS_NOEXEC | MS_NOSUID | MS_RELATIME, "newinstance"));

    //
    // Verify previously created endpoint does not exist in the new mount.
    //

    LxtCheckErrnoFailure(stat(EndpointName, &EndpointStat), ENOENT);
    LxtCheckErrno(stat(PtmxName, &PtmxStat));

    //
    // Check default ptmxmode settings.
    //

    LxtCheckEqual(PtmxStat.st_mode, S_IFCHR, "%d");

    //
    // Create a new endpoint and check default UID/GID/mode settings.
    //

    sprintf(EndpointName, "%s/0", PTS_TEST_MNT);
    LxtCheckErrno((PtmFd2 = open(PtmxName, O_RDWR)));
    LxtCheckErrno(stat(EndpointName, &EndpointStat));
    LxtCheckEqual(EndpointStat.st_uid, CurrentUid, "%d");
    LxtCheckEqual(EndpointStat.st_gid, CurrentGid, "%d");
    LxtCheckEqual(EndpointStat.st_mode, (S_IFCHR | 0600), "%d");
    LxtClose(PtmFd2);
    LxtCheckErrno(umount(PTS_TEST_MNT));

    //
    // Mount with "newinstance" and specify UID/GID/mode/ptmxmode options.
    //

    LxtCheckErrnoZeroSuccess(mount(
        NULL, PTS_TEST_MNT, "devpts", MS_NOEXEC | MS_NOSUID | MS_RELATIME, "newinstance,uid=0,gid=5,mode=0620,ptmxmode=666"));

    LxtCheckErrno(stat(PtmxName, &PtmxStat));

    //
    // Check default ptmxmode settings.
    //

    LxtCheckEqual(PtmxStat.st_mode, (S_IFCHR | 0666), "%d");

    //
    // Create a new endpoint and check default UID/GID/mode settings.
    //

    sprintf(EndpointName, "%s/0", PTS_TEST_MNT);
    LxtCheckErrno((PtmFd2 = open(PtmxName, O_RDWR)));
    LxtCheckErrno(stat(EndpointName, &EndpointStat));
    LxtCheckEqual(EndpointStat.st_uid, 0, "%d");
    LxtCheckEqual(EndpointStat.st_gid, 5, "%d");
    LxtCheckEqual(EndpointStat.st_mode, (S_IFCHR | 0620), "%d");
    LxtClose(PtmFd2);
    LxtCheckErrno(umount(PTS_TEST_MNT));

ErrorExit:
    if (PtmFd != -1)
    {
        close(PtmFd);
    }

    if (PtmFd2 != -1)
    {
        close(PtmFd2);
    }

    if (PtsFd != -1)
    {
        close(PtsFd);
    }

    umount(PTS_TEST_MNT);
    rmdir(PTS_TEST_MNT);
    return Result;
}

int PtPacketBasic1(PLXT_ARGS Args)

/*++

Routine Description:

    This routine sets packet-mode and checks the set value.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int BytesReadWrite;
    char ControlByte;
    int PacketMode;
    int PtmFd;
    int PtmFlags;
    int PtsFd;
    fd_set ReadFds;
    int Result;
    int SerialNumber;
    struct timeval Timeout;

    //
    // Initialize locals.
    //

    PtmFd = -1;
    PtsFd = -1;

    //
    // Open Master-Subordinate.
    //

    LxtCheckErrno(OpenMasterSubordinate(&PtmFd, &PtsFd, NULL, &SerialNumber));
    LxtLogInfo("Master opened at FD:%d", PtmFd);
    LxtLogInfo("Subordinate Serial Number: %d", SerialNumber);
    LxtLogInfo("Subordinate opened at FD:%d", PtsFd);

    //
    // Verify packet-mode is off (0) to start.
    //

    LxtCheckErrno(ioctl(PtmFd, TIOCGPKT, &PacketMode));
    LxtCheckEqual(PacketMode, 0, "%d");

    //
    // Turn on packet-mode using a non-zero value.
    //

    PacketMode = 0x123;
    LxtCheckErrno(ioctl(PtmFd, TIOCPKT, &PacketMode));

    //
    // Verify packet-mode value.
    //

    LxtCheckErrno(ioctl(PtmFd, TIOCGPKT, &PacketMode));
    LxtCheckEqual(PacketMode, 1, "%d");

    //
    // Verify no data waiting.
    //

    memset(&Timeout, 0, sizeof(Timeout));
    FD_ZERO(&ReadFds);
    FD_SET(PtmFd, &ReadFds);
    LxtCheckErrno(select((PtmFd + 1), &ReadFds, NULL, NULL, &Timeout));
    LxtCheckEqual(Result, 0, "%d");

    //
    // Verify no data available to read.
    //

    LxtCheckErrno(PtmFlags = fcntl(PtmFd, F_GETFL, 0));
    LxtCheckErrno(fcntl(PtmFd, F_SETFL, (PtmFlags | O_NONBLOCK)));
    LxtCheckErrnoFailure(BytesReadWrite = read(PtmFd, &ControlByte, sizeof(ControlByte)), EAGAIN);
    LxtCheckErrno(fcntl(PtmFd, F_SETFL, PtmFlags));

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

int PtPacketBasic2(PLXT_ARGS Args)

/*++

Routine Description:

    This routine performs a basic packet-mode pseudo terminal test. The steps are:
    - Turn on packet mode.
    - Perform simple read/write check on the master-subordinate.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int PacketMode;
    int PtmFd;
    int PtsFd;
    int Result;
    int SerialNumber;

    //
    // Initialize locals.
    //

    PtmFd = -1;
    PtsFd = -1;

    //
    // Open Master-Subordinate.
    //

    LxtCheckErrno(OpenMasterSubordinate(&PtmFd, &PtsFd, NULL, &SerialNumber));
    LxtLogInfo("Master opened at FD:%d", PtmFd);
    LxtLogInfo("Subordinate Serial Number: %d", SerialNumber);
    LxtLogInfo("Subordinate opened at FD:%d", PtsFd);

    //
    // Turn on packet-mode.
    //

    PacketMode = 1;
    LxtCheckErrno(ioctl(PtmFd, TIOCPKT, &PacketMode));

    //
    // Test simple send/receive.
    //

    LxtCheckErrno(SimpleReadWriteCheck(PtmFd, PtsFd));

    //
    // Enable raw mode.
    //

    LxtCheckErrno(RawInit(PtsFd));

    //
    // Test simple send/receive.
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

int PtPacketBasic3(PLXT_ARGS Args)

/*++

Routine Description:

    This routine performs a basic packet-mode pseudo terminal test. The steps are:
    - Turns off canonical mode to avoid line discipline.
    - Turn on packet mode.
    - Read back partial message.


Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int BytesReadWrite;
    int ExpectedResult;
    const char* Greetings = "Hi there!!";
    int PacketMode;
    int PtmFd;
    int PtsFd;
    char ReadBuffer[20];
    int Result;
    int SerialNumber;

    //
    // Initialize locals.
    //

    PtmFd = -1;
    PtsFd = -1;

    //
    // Open Master-Subordinate.
    //

    LxtCheckErrno(OpenMasterSubordinate(&PtmFd, &PtsFd, NULL, &SerialNumber));
    LxtLogInfo("Master opened at FD:%d", PtmFd);
    LxtLogInfo("Subordinate Serial Number: %d", SerialNumber);
    LxtLogInfo("Subordinate opened at FD:%d", PtsFd);

    //
    // Configure as raw / packet-mode.
    //

    LxtCheckErrno(RawInit(PtsFd));
    PacketMode = 1;
    LxtCheckErrno(ioctl(PtmFd, TIOCPKT, &PacketMode));

    //
    // Write to subordinate.
    //

    ExpectedResult = strlen(Greetings);
    LxtCheckErrno(BytesReadWrite = write(PtsFd, Greetings, ExpectedResult));
    LxtCheckFnResults("write", BytesReadWrite, ExpectedResult);
    LxtCheckErrno(tcdrain(PtsFd));

    //
    // Read just header byte from master.
    //

    LxtLogInfo("Reading header byte from master");
    LxtCheckErrno(BytesReadWrite = read(PtmFd, ReadBuffer, 1));
    LxtCheckFnResults("read", BytesReadWrite, 1);
    LxtCheckEqual(ReadBuffer[0], 0, "%hhd");

    //
    // Original message should still be there.
    //

    LxtLogInfo("Checking for remaining message from master");
    memset(ReadBuffer, 0, sizeof(ReadBuffer));
    LxtCheckErrno(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)));
    LxtCheckFnResults("read", BytesReadWrite, ExpectedResult + 1);
    ReadBuffer[BytesReadWrite] = '\0';
    LxtCheckEqual(ReadBuffer[0], 0, "%hhd");
    LxtCheckStringEqual(ReadBuffer + 1, Greetings);

    //
    // Write to master.
    //

    LxtCheckErrno(BytesReadWrite = write(PtmFd, Greetings, ExpectedResult));
    LxtCheckFnResults("write", BytesReadWrite, ExpectedResult);
    LxtCheckErrno(tcdrain(PtmFd));

    //
    // Read from subordinate.
    //

    LxtLogInfo("Reading from subordinate");
    LxtCheckErrno(BytesReadWrite = read(PtsFd, ReadBuffer, 1));
    LxtCheckFnResults("read", BytesReadWrite, 1);
    LxtCheckEqual(ReadBuffer[0], Greetings[0], "%hhd");
    memset(ReadBuffer, 0, sizeof(ReadBuffer));
    LxtCheckErrno(BytesReadWrite = read(PtsFd, ReadBuffer, sizeof(ReadBuffer)));
    LxtCheckFnResults("read", BytesReadWrite, ExpectedResult - 1);
    ReadBuffer[BytesReadWrite] = '\0';
    LxtCheckStringEqual(ReadBuffer, Greetings + 1);

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

int PtPacketBasic4(PLXT_ARGS Args)

/*++

Routine Description:

    This routine performs a basic packet-mode pseudo terminal test. The steps are:
    - Turns off canonical mode to avoid line discipline.
    - Turn on packet mode.
    - Read back partial messages.


Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int BytesReadWrite;
    int ExpectedResult;
    const char* Greetings = "Hi there!!";
    int PacketMode;
    int PtmFd;
    int PtsFd;
    char ReadBuffer[20];
    int Result;
    int SerialNumber;

    //
    // Initialize locals.
    //

    PtmFd = -1;
    PtsFd = -1;

    //
    // Open Master-Subordinate.
    //

    LxtCheckErrno(OpenMasterSubordinate(&PtmFd, &PtsFd, NULL, &SerialNumber));
    LxtLogInfo("Master opened at FD:%d", PtmFd);
    LxtLogInfo("Subordinate Serial Number: %d", SerialNumber);
    LxtLogInfo("Subordinate opened at FD:%d", PtsFd);

    //
    // Configure as raw / packet-mode.
    //

    LxtCheckErrno(RawInit(PtsFd));
    PacketMode = 1;
    LxtCheckErrno(ioctl(PtmFd, TIOCPKT, &PacketMode));

    //
    // Write to subordinate.
    //

    ExpectedResult = strlen(Greetings);
    LxtCheckErrno(BytesReadWrite = write(PtsFd, Greetings, ExpectedResult));
    LxtCheckFnResults("write", BytesReadWrite, ExpectedResult);
    LxtCheckErrno(tcdrain(PtsFd));

    //
    // Read first two bytes from master.
    //

    LxtLogInfo("Reading two bytes from master");
    LxtCheckErrno(BytesReadWrite = read(PtmFd, ReadBuffer, 2));
    LxtCheckFnResults("read", BytesReadWrite, 2);
    LxtCheckEqual(ReadBuffer[0], 0, "%hhd");
    LxtCheckEqual(ReadBuffer[1], Greetings[0], "%hhd");

    //
    // Check for remaining message bytes.
    //

    LxtLogInfo("Checking for remaining message from master");
    memset(ReadBuffer, 0, sizeof(ReadBuffer));
    LxtCheckErrno(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)));
    LxtCheckFnResults("read", BytesReadWrite, ExpectedResult);
    ReadBuffer[BytesReadWrite] = '\0';
    LxtCheckEqual(ReadBuffer[0], 0, "%hhd");
    LxtCheckStringEqual(ReadBuffer + 1, Greetings + 1);

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

int PtPacketToggleMode1(PLXT_ARGS Args)

/*++

Routine Description:

    This routine sends a message to the subordinate with packet-mode off, turns
    it on and reads from master.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int BytesReadWrite;
    int ExpectedResult;
    const char* Greetings = "Hi there!!";
    int PacketMode;
    int PtmFd;
    int PtsFd;
    char ReadBuffer[20];
    int Result;
    int SerialNumber;

    //
    // Initialize locals.
    //

    PtmFd = -1;
    PtsFd = -1;

    //
    // Open Master-Subordinate.
    //

    LxtCheckErrno(OpenMasterSubordinate(&PtmFd, &PtsFd, NULL, &SerialNumber));
    LxtLogInfo("Master opened at FD:%d", PtmFd);
    LxtLogInfo("Subordinate Serial Number: %d", SerialNumber);
    LxtLogInfo("Subordinate opened at FD:%d", PtsFd);

    //
    // Configure as raw.
    //

    LxtCheckErrno(RawInit(PtsFd));

    //
    // Write to subordinate.
    //

    ExpectedResult = strlen(Greetings);
    LxtCheckErrno(BytesReadWrite = write(PtsFd, Greetings, ExpectedResult));
    LxtCheckFnResults("write", BytesReadWrite, ExpectedResult);
    LxtCheckErrno(tcdrain(PtsFd));

    //
    // Configure as packet-mode.
    //

    LxtCheckErrno(RawInit(PtsFd));
    PacketMode = 1;
    LxtCheckErrno(ioctl(PtmFd, TIOCPKT, &PacketMode));

    //
    // Read message from master.
    //

    LxtLogInfo("Reading from master");
    memset(ReadBuffer, 0, sizeof(ReadBuffer));
    LxtCheckErrno(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)));
    LxtCheckFnResults("read", BytesReadWrite, ExpectedResult + 1);
    ReadBuffer[BytesReadWrite] = '\0';
    LxtCheckEqual(ReadBuffer[0], 0, "%hhd");
    LxtCheckStringEqual(ReadBuffer + 1, Greetings);

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

int PtPacketToggleMode2(PLXT_ARGS Args)

/*++

Routine Description:

    This routine sends a message to the subordinate with packet-mode on, turns
    it off and reads from master.


Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int BytesReadWrite;
    int ExpectedResult;
    const char* Greetings = "Hi there!!";
    int PacketMode;
    int PtmFd;
    int PtsFd;
    char ReadBuffer[20];
    int Result;
    int SerialNumber;

    //
    // Initialize locals.
    //

    PtmFd = -1;
    PtsFd = -1;

    //
    // Open Master-Subordinate.
    //

    LxtCheckErrno(OpenMasterSubordinate(&PtmFd, &PtsFd, NULL, &SerialNumber));
    LxtLogInfo("Master opened at FD:%d", PtmFd);
    LxtLogInfo("Subordinate Serial Number: %d", SerialNumber);
    LxtLogInfo("Subordinate opened at FD:%d", PtsFd);

    //
    // Configure as raw / packet-mode.
    //

    LxtCheckErrno(RawInit(PtsFd));
    PacketMode = 1;
    LxtCheckErrno(ioctl(PtmFd, TIOCPKT, &PacketMode));

    //
    // Write to subordinate.
    //

    ExpectedResult = strlen(Greetings);
    LxtCheckErrno(BytesReadWrite = write(PtsFd, Greetings, ExpectedResult));
    LxtCheckFnResults("write", BytesReadWrite, ExpectedResult);
    LxtCheckErrno(tcdrain(PtsFd));

    //
    // Turn off packet-mode.
    //

    PacketMode = 0;
    LxtCheckErrno(ioctl(PtmFd, TIOCPKT, &PacketMode));

    //
    // Read message from master.
    //

    LxtLogInfo("Reading from master");
    memset(ReadBuffer, 0, sizeof(ReadBuffer));
    LxtCheckErrno(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)));
    LxtCheckFnResults("read", BytesReadWrite, ExpectedResult);
    ReadBuffer[BytesReadWrite] = '\0';
    LxtCheckStringEqual(ReadBuffer, Greetings);

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

int PtPacketToggleMode3(PLXT_ARGS Args)

/*++

Routine Description:

    This routine performs operations that would normally result in a packet
    mode status, then turns on packet mode and checks the control byte.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int BytesReadWrite;
    cc_t ControlArray[NCCS];
    char DefaultStartChar;
    int ExpectedResult;
    int FileFlags;
    int PacketMode;
    int PtmFd;
    int PtsFd;
    char ReadBuffer[20];
    fd_set ReadFds;
    int Result;
    int SerialNumber;
    struct timeval Timeout;

    //
    // Initialize locals.
    //

    PtmFd = -1;
    PtsFd = -1;

    //
    // Open Master-Subordinate.
    //

    LxtCheckErrno(OpenMasterSubordinate(&PtmFd, &PtsFd, NULL, &SerialNumber));
    LxtLogInfo("Master opened at FD:%d", PtmFd);
    LxtLogInfo("Subordinate Serial Number: %d", SerialNumber);
    LxtLogInfo("Subordinate opened at FD:%d", PtsFd);

    //
    // Fetch the default control character array values.
    //

    LxtCheckResult(TerminalSettingsGetControlArray(PtmFd, ControlArray));
    DefaultStartChar = ControlArray[VSTART];

    //
    // Modify the start control character.
    //

    ControlArray[VSTART] = _POSIX_VDISABLE;
    LxtCheckResult(TerminalSettingsSetControlArray(PtmFd, ControlArray));

    //
    // Suspend output on subordinate.
    //

    LxtCheckErrno(tcflow(PtsFd, TCOOFF));

    //
    // Flush subordinate read and write queues.
    //

    LxtCheckErrno(tcflush(PtsFd, TCIOFLUSH));

    //
    // Configure packet-mode.
    //

    PacketMode = 1;
    LxtCheckErrno(ioctl(PtmFd, TIOCPKT, &PacketMode));

    //
    // Master endpoint should not be ready for read.
    //

    memset(&Timeout, 0, sizeof(Timeout));
    FD_ZERO(&ReadFds);
    FD_SET(PtmFd, &ReadFds);
    LxtCheckErrno(select((PtmFd + 1), &ReadFds, NULL, NULL, &Timeout));
    LxtCheckEqual(Result, 0, "%d");

    //
    // No data expected.
    //

    LxtCheckErrno(FileFlags = fcntl(PtmFd, F_GETFL, 0));
    LxtCheckErrno(fcntl(PtmFd, F_SETFL, (FileFlags | O_NONBLOCK)));
    LxtCheckErrnoFailure(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)), EAGAIN);
    LxtCheckErrno(fcntl(PtmFd, F_SETFL, FileFlags));

    //
    // Now with packet-mode enabled, undo/redo the previous steps.
    //

    //
    // Modify the start control character.
    //

    ControlArray[VSTART] = DefaultStartChar;
    LxtCheckResult(TerminalSettingsSetControlArray(PtmFd, ControlArray));

    //
    // Resume output on subordinate.
    //

    LxtCheckErrno(tcflow(PtsFd, TCOON));

    //
    // Flush subordinate read and write queues.
    //

    LxtCheckErrno(tcflush(PtsFd, TCIOFLUSH));

    //
    // Master endpoint should now be ready for read.
    //

    FD_SET(PtmFd, &ReadFds);
    LxtCheckErrno(select((PtmFd + 1), &ReadFds, NULL, NULL, &Timeout));
    LxtCheckEqual(Result, 1, "%d");

    //
    // Read control byte.
    //

    LxtLogInfo("Reading from master");
    LxtCheckErrno(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)));
    LxtCheckFnResults("read", BytesReadWrite, 1);
    LxtCheckEqual(ReadBuffer[0], (TIOCPKT_FLUSHREAD | TIOCPKT_FLUSHWRITE | TIOCPKT_START | TIOCPKT_DOSTOP), "%hhd");

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

int PtPacketToggleMode4(PLXT_ARGS Args)

/*++

Routine Description:

    This routine waits for data on the master subordinate on one thread, while
    a second thread switches the master endpoint to packet-mode and flushes
    the read queue.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int BytesReadWrite;
    pid_t ChildPid;
    int ExpectedResult;
    int PacketMode;
    int PtmFd;
    int PtsFd;
    char ReadBuffer[20];
    fd_set ReadFds;
    int Result;
    int Status;
    struct timeval Timeout;

    //
    // Initialize locals
    //

    ChildPid = -1;
    PtmFd = -1;
    PtsFd = -1;

    LXT_SYNCHRONIZATION_POINT_START();
    LxtCheckErrno(ChildPid = ForkPty(&PtmFd, &PtsFd));
    if (ChildPid == 0)
    {
        memset(&Timeout, 0, sizeof(Timeout));
        Timeout.tv_sec = 3;
        FD_ZERO(&ReadFds);
        FD_SET(PtmFd, &ReadFds);
        LxtCheckErrno(select((PtmFd + 1), &ReadFds, NULL, NULL, &Timeout));
        LxtCheckEqual(Result, 1, "%d");
    }
    else
    {

        //
        // Give child a chance to wait in select system call.
        //

        sleep(1);

        //
        // Enable packet mode.
        //

        PacketMode = 1;
        LxtCheckErrno(ioctl(PtmFd, TIOCPKT, &PacketMode));
        sleep(1);

        //
        // Flush subordinate read queue.
        //

        LxtCheckErrno(tcflush(PtsFd, TCIFLUSH));
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

    LXT_SYNCHRONIZATION_POINT_END();
    return Result;
}

int PtPacketToggleMode5(PLXT_ARGS Args)

/*++

Routine Description:

    This routine enables packet mode and then waits for data on the master
    subordinate on one thread. A second thread disables packet-mode on the
    master endpoint and flushes the read queue.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int BytesReadWrite;
    pid_t ChildPid;
    int ExpectedResult;
    int PacketMode;
    int PtmFd;
    int PtsFd;
    char ReadBuffer[20];
    fd_set ReadFds;
    int Result;
    int Status;
    struct timeval Timeout;

    //
    // Initialize locals
    //

    ChildPid = -1;
    PtmFd = -1;
    PtsFd = -1;

    LXT_SYNCHRONIZATION_POINT_START();
    LxtCheckErrno(ChildPid = ForkPty(&PtmFd, &PtsFd));
    if (ChildPid == 0)
    {

        //
        // Enable packet mode.
        //

        PacketMode = 1;
        LxtCheckErrno(ioctl(PtmFd, TIOCPKT, &PacketMode));

        //
        // Wait for data on master.
        //

        memset(&Timeout, 0, sizeof(Timeout));
        Timeout.tv_sec = 2;
        FD_ZERO(&ReadFds);
        FD_SET(PtmFd, &ReadFds);
        LxtCheckErrno(select((PtmFd + 1), &ReadFds, NULL, NULL, &Timeout));
        LxtCheckEqual(Result, 0, "%d");
    }
    else
    {

        //
        // Give child a chance to wait in select system call.
        //

        sleep(1);

        //
        // Disable packet mode.
        //

        PacketMode = 0;
        LxtCheckErrno(ioctl(PtmFd, TIOCPKT, &PacketMode));

        //
        // Flush subordinate read queue.
        //

        LxtCheckErrno(tcflush(PtsFd, TCIFLUSH));
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

    LXT_SYNCHRONIZATION_POINT_END();
    return Result;
}

int PtPacketToggleMode6(PLXT_ARGS Args)

/*++

Routine Description:

    This routine enables packet mode and starts a read. A second thread
    disables packet-mode and writes data.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int BytesReadWrite;
    pid_t ChildPid;
    int ExpectedResult;
    const char* Greetings = "Hi there!!";
    int PacketMode;
    int PtmFd;
    int PtsFd;
    char ReadBuffer[20];
    int Result;
    int Status;

    //
    // Initialize locals
    //

    ChildPid = -1;
    PtmFd = -1;
    PtsFd = -1;

    LXT_SYNCHRONIZATION_POINT_START()
    ExpectedResult = strlen(Greetings);
    LxtCheckErrno(ChildPid = ForkPty(&PtmFd, &PtsFd));
    if (ChildPid == 0)
    {

        //
        // Configure as raw / packet-mode.
        //

        LxtCheckErrno(RawInit(PtsFd));
        PacketMode = 1;
        LxtCheckErrno(ioctl(PtmFd, TIOCPKT, &PacketMode));

        //
        // Read message from master.
        //

        LxtLogInfo("Reading from master");
        memset(ReadBuffer, 0, sizeof(ReadBuffer));
        LxtCheckErrno(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)));
        LxtCheckFnResults("read", BytesReadWrite, ExpectedResult + 1);
        ReadBuffer[BytesReadWrite] = '\0';
        LxtCheckEqual(ReadBuffer[0], 0, "%hhd");
        LxtCheckStringEqual(ReadBuffer + 1, Greetings);
    }
    else
    {

        //
        // Give child a chance to wait in read system call.
        //

        sleep(1);

        //
        // Disable packet mode.
        //

        PacketMode = 0;
        LxtCheckErrno(ioctl(PtmFd, TIOCPKT, &PacketMode));

        //
        // Write to subordinate.
        //

        LxtCheckErrno(BytesReadWrite = write(PtsFd, Greetings, ExpectedResult));
        LxtCheckFnResults("write", BytesReadWrite, ExpectedResult);
        LxtCheckErrno(tcdrain(PtsFd));
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

    LXT_SYNCHRONIZATION_POINT_END();
    return Result;
}

int PtPacketToggleMode7(PLXT_ARGS Args)

/*++

Routine Description:

    This routine starts a read on the master endpoint. A second thread
    enables packet-mode and writes data.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int BytesReadWrite;
    pid_t ChildPid;
    int ExpectedResult;
    const char* Greetings = "Hi there!!";
    int PacketMode;
    int PtmFd;
    int PtsFd;
    char ReadBuffer[20];
    int Result;
    int Status;

    //
    // Initialize locals
    //

    ChildPid = -1;
    PtmFd = -1;
    PtsFd = -1;

    LXT_SYNCHRONIZATION_POINT_START();
    ExpectedResult = strlen(Greetings);
    LxtCheckErrno(ChildPid = ForkPty(&PtmFd, &PtsFd));
    if (ChildPid == 0)
    {

        //
        // Configure as raw.
        //

        LxtCheckErrno(RawInit(PtsFd));

        //
        // Read message from master.
        //

        LxtLogInfo("Reading from master");
        memset(ReadBuffer, 0, sizeof(ReadBuffer));
        LxtCheckErrno(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)));
        LxtCheckFnResults("read", BytesReadWrite, ExpectedResult);
        ReadBuffer[BytesReadWrite] = '\0';
        LxtCheckStringEqual(ReadBuffer, Greetings);
    }
    else
    {

        //
        // Give child a chance to wait in read system call.
        //

        sleep(1);

        //
        // Enable packet mode.
        //

        PacketMode = 1;
        LxtCheckErrno(ioctl(PtmFd, TIOCPKT, &PacketMode));

        //
        // Write to subordinate.
        //

        LxtCheckErrno(BytesReadWrite = write(PtsFd, Greetings, ExpectedResult));
        LxtCheckFnResults("write", BytesReadWrite, ExpectedResult);
        LxtCheckErrno(tcdrain(PtsFd));
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

    LXT_SYNCHRONIZATION_POINT_END();
    return Result;
}

int PtPacketFlushRead1(PLXT_ARGS Args)

/*++

Routine Description:

    This routine flushes the subordinate read queue.


Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int BytesReadWrite;
    int PacketMode;
    int PtmFd;
    int PtsFd;
    char ReadBuffer[1];
    fd_set ReadFds;
    int Result;
    int SerialNumber;
    struct timeval Timeout;

    //
    // Initialize locals.
    //

    PtmFd = -1;
    PtsFd = -1;

    //
    // Open Master-Subordinate.
    //

    LxtCheckErrno(OpenMasterSubordinate(&PtmFd, &PtsFd, NULL, &SerialNumber));
    LxtLogInfo("Master opened at FD:%d", PtmFd);
    LxtLogInfo("Subordinate Serial Number: %d", SerialNumber);
    LxtLogInfo("Subordinate opened at FD:%d", PtsFd);

    //
    // Configure as raw / packet-mode.
    //

    LxtCheckErrno(RawInit(PtsFd));
    PacketMode = 1;
    LxtCheckErrno(ioctl(PtmFd, TIOCPKT, &PacketMode));

    //
    // Flush subordinate read queue.
    //

    LxtCheckErrno(tcflush(PtsFd, TCIFLUSH));

    //
    // Check for available data.
    //

    LxtLogInfo("Verifying master has data to read...");
    memset(&Timeout, 0, sizeof(Timeout));
    FD_ZERO(&ReadFds);
    FD_SET(PtmFd, &ReadFds);
    LxtCheckErrno(select((PtmFd + 1), &ReadFds, NULL, NULL, &Timeout));
    LxtCheckEqual(Result, 1, "%d");

    //
    // Read control byte.
    //

    LxtLogInfo("Reading from master");
    LxtCheckErrno(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)));
    LxtCheckFnResults("read", BytesReadWrite, 1);
    LxtCheckEqual(ReadBuffer[0], TIOCPKT_FLUSHREAD, "%hhd");

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

int PtPacketFlushRead2(PLXT_ARGS Args)

/*++

Routine Description:

    This routine sends a message to the subordinate with packet-mode on, then
    flushes the subordinate read queue.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int BytesReadWrite;
    int ExpectedResult;
    const char* Greetings = "Hi there!!";
    int PacketMode;
    int PtmFd;
    int PtsFd;
    char ReadBuffer[20];
    fd_set ReadFds;
    int Result;
    int SerialNumber;
    struct timeval Timeout;

    //
    // Initialize locals.
    //

    PtmFd = -1;
    PtsFd = -1;

    //
    // Open Master-Subordinate.
    //

    LxtCheckErrno(OpenMasterSubordinate(&PtmFd, &PtsFd, NULL, &SerialNumber));
    LxtLogInfo("Master opened at FD:%d", PtmFd);
    LxtLogInfo("Subordinate Serial Number: %d", SerialNumber);
    LxtLogInfo("Subordinate opened at FD:%d", PtsFd);

    //
    // Configure as raw / packet-mode.
    //

    LxtCheckErrno(RawInit(PtsFd));
    PacketMode = 1;
    LxtCheckErrno(ioctl(PtmFd, TIOCPKT, &PacketMode));

    //
    // Write to subordinate.
    //

    ExpectedResult = strlen(Greetings);
    LxtCheckErrno(BytesReadWrite = write(PtsFd, Greetings, ExpectedResult));
    LxtCheckFnResults("write", BytesReadWrite, ExpectedResult);
    LxtCheckErrno(tcdrain(PtsFd));

    //
    // Flush subordinate read queue.
    //

    LxtCheckErrno(tcflush(PtsFd, TCIFLUSH));

    //
    // Check for available data.
    //

    LxtLogInfo("Verifying master has data to read...");
    memset(&Timeout, 0, sizeof(Timeout));
    FD_ZERO(&ReadFds);
    FD_SET(PtmFd, &ReadFds);
    LxtCheckErrno(select((PtmFd + 1), &ReadFds, NULL, NULL, &Timeout));
    LxtCheckEqual(Result, 1, "%d");

    //
    // Read control byte.
    //

    LxtLogInfo("Reading from master");
    LxtCheckErrno(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)));
    LxtCheckFnResults("read", BytesReadWrite, 1);
    LxtCheckEqual(ReadBuffer[0], TIOCPKT_FLUSHREAD, "%hhd");

    //
    // Read message from master.
    //

    LxtLogInfo("Reading from master");
    memset(ReadBuffer, 0, sizeof(ReadBuffer));
    LxtCheckErrno(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)));
    LxtCheckFnResults("read", BytesReadWrite, ExpectedResult + 1);
    ReadBuffer[BytesReadWrite] = '\0';
    LxtCheckEqual(ReadBuffer[0], 0, "%hhd");
    LxtCheckStringEqual(ReadBuffer + 1, Greetings);

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

int PtPacketFlushRead3(PLXT_ARGS Args)

/*++

Routine Description:

    This routine waits for data on the master subordinate on one thread, while
    a second thread flushes the read queue.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int BytesReadWrite;
    pid_t ChildPid;
    int ExpectedResult;
    int PacketMode;
    int PtmFd;
    int PtsFd;
    char ReadBuffer[20];
    int Result;
    int Status;

    //
    // Initialize locals
    //

    ChildPid = -1;
    PtmFd = -1;
    PtsFd = -1;

    LXT_SYNCHRONIZATION_POINT_START();
    LxtCheckErrno(ChildPid = ForkPty(&PtmFd, &PtsFd));
    if (ChildPid == 0)
    {

        //
        // Enable packet mode.
        //

        PacketMode = 1;
        LxtCheckErrno(ioctl(PtmFd, TIOCPKT, &PacketMode));

        //
        // Read from master.
        //

        LxtLogInfo("Reading from master");
        LxtCheckErrno(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)));
        LxtCheckFnResults("read", BytesReadWrite, 1);
        LxtCheckEqual(ReadBuffer[0], TIOCPKT_FLUSHREAD, "%hhd");
    }
    else
    {

        //
        // Give child a chance to wait in the read system call.
        //

        sleep(1);

        //
        // Flush subordinate read queue.
        //

        LxtCheckErrno(tcflush(PtsFd, TCIFLUSH));
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

    LXT_SYNCHRONIZATION_POINT_END();
    return Result;
}

int PtPacketFlushWrite1(PLXT_ARGS Args)

/*++

Routine Description:

    This routine flushes the subordinate write queue.


Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int BytesReadWrite;
    int PacketMode;
    int PtmFd;
    int PtsFd;
    char ReadBuffer[1];
    fd_set ReadFds;
    int Result;
    int SerialNumber;
    struct timeval Timeout;

    //
    // Initialize locals.
    //

    PtmFd = -1;
    PtsFd = -1;

    //
    // Open Master-Subordinate.
    //

    LxtCheckErrno(OpenMasterSubordinate(&PtmFd, &PtsFd, NULL, &SerialNumber));
    LxtLogInfo("Master opened at FD:%d", PtmFd);
    LxtLogInfo("Subordinate Serial Number: %d", SerialNumber);
    LxtLogInfo("Subordinate opened at FD:%d", PtsFd);

    //
    // Configure as raw / packet-mode.
    //

    LxtCheckErrno(RawInit(PtsFd));
    PacketMode = 1;
    LxtCheckErrno(ioctl(PtmFd, TIOCPKT, &PacketMode));

    //
    // Flush subordinate write queue.
    //

    LxtCheckErrno(tcflush(PtsFd, TCOFLUSH));

    //
    // Check for available data.
    //

    LxtLogInfo("Verifying master has data to read...");
    memset(&Timeout, 0, sizeof(Timeout));
    FD_ZERO(&ReadFds);
    FD_SET(PtmFd, &ReadFds);
    LxtCheckErrno(select((PtmFd + 1), &ReadFds, NULL, NULL, &Timeout));
    LxtCheckEqual(Result, 1, "%d");

    //
    // Read control byte.
    //

    LxtLogInfo("Reading from master");
    LxtCheckErrno(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)));
    LxtCheckFnResults("read", BytesReadWrite, 1);
    LxtCheckEqual(ReadBuffer[0], TIOCPKT_FLUSHWRITE, "%hhd");

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

int PtPacketFlushWrite2(PLXT_ARGS Args)

/*++

Routine Description:

    This routine sends a message to the subordinate with packet-mode on, then
    flushes the subordinate write queue.


Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int BytesReadWrite;
    int ExpectedResult;
    const char* Greetings = "Hi there!!";
    int PacketMode;
    int PtmFd;
    int PtmFlags;
    int PtsFd;
    char ReadBuffer[20];
    fd_set ReadFds;
    int Result;
    int SerialNumber;
    struct timeval Timeout;

    //
    // Initialize locals.
    //

    PtmFd = -1;
    PtsFd = -1;

    //
    // Open Master-Subordinate.
    //

    LxtCheckErrno(OpenMasterSubordinate(&PtmFd, &PtsFd, NULL, &SerialNumber));
    LxtLogInfo("Master opened at FD:%d", PtmFd);
    LxtLogInfo("Subordinate Serial Number: %d", SerialNumber);
    LxtLogInfo("Subordinate opened at FD:%d", PtsFd);

    //
    // Configure as raw / packet-mode.
    //

    LxtCheckErrno(RawInit(PtsFd));
    PacketMode = 1;
    LxtCheckErrno(ioctl(PtmFd, TIOCPKT, &PacketMode));

    //
    // Write to subordinate.
    //

    ExpectedResult = strlen(Greetings);
    LxtCheckErrno(BytesReadWrite = write(PtsFd, Greetings, ExpectedResult));
    LxtCheckFnResults("write", BytesReadWrite, ExpectedResult);
    LxtCheckErrno(tcdrain(PtsFd));

    //
    // Flush subordinate write queue.
    //

    LxtCheckErrno(tcflush(PtsFd, TCOFLUSH));

    //
    // Check for available data.
    //

    LxtLogInfo("Verifying master has data to read...");
    memset(&Timeout, 0, sizeof(Timeout));
    FD_ZERO(&ReadFds);
    FD_SET(PtmFd, &ReadFds);
    LxtCheckErrno(select((PtmFd + 1), &ReadFds, NULL, NULL, &Timeout));
    LxtCheckEqual(Result, 1, "%d");

    //
    // Read control byte.
    //

    LxtLogInfo("Reading from master");
    LxtCheckErrno(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)));
    LxtCheckFnResults("read", BytesReadWrite, 1);
    LxtCheckEqual(ReadBuffer[0], TIOCPKT_FLUSHWRITE, "%hhd");

    //
    // No message should be waiting because the write queue was flushed.
    //

    LxtCheckErrno(PtmFlags = fcntl(PtmFd, F_GETFL, 0));
    LxtCheckErrno(fcntl(PtmFd, F_SETFL, (PtmFlags | O_NONBLOCK)));
    LxtCheckErrnoFailure(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)), EAGAIN);
    LxtCheckErrno(fcntl(PtmFd, F_SETFL, PtmFlags));

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

int PtPacketFlushReadWrite1(PLXT_ARGS Args)

/*++

Routine Description:

    This routine flushes the subordinate read and write queues.


Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int BytesReadWrite;
    int PacketMode;
    int PtmFd;
    int PtmFlags;
    int PtsFd;
    char ReadBuffer[1];
    fd_set ReadFds;
    int Result;
    int SerialNumber;
    struct timeval Timeout;

    //
    // Initialize locals.
    //

    PtmFd = -1;
    PtsFd = -1;

    //
    // Open Master-Subordinate.
    //

    LxtCheckErrno(OpenMasterSubordinate(&PtmFd, &PtsFd, NULL, &SerialNumber));
    LxtLogInfo("Master opened at FD:%d", PtmFd);
    LxtLogInfo("Subordinate Serial Number: %d", SerialNumber);
    LxtLogInfo("Subordinate opened at FD:%d", PtsFd);

    //
    // Configure as raw / packet-mode.
    //

    LxtCheckErrno(RawInit(PtsFd));
    PacketMode = 1;
    LxtCheckErrno(ioctl(PtmFd, TIOCPKT, &PacketMode));

    //
    // Flush subordinate read and write queues.
    //

    LxtCheckErrno(tcflush(PtsFd, TCIOFLUSH));

    //
    // Check for available data.
    //

    LxtLogInfo("Verifying master has data to read...");
    memset(&Timeout, 0, sizeof(Timeout));
    FD_ZERO(&ReadFds);
    FD_SET(PtmFd, &ReadFds);
    LxtCheckErrno(select((PtmFd + 1), &ReadFds, NULL, NULL, &Timeout));
    LxtCheckEqual(Result, 1, "%d");

    //
    // Read control byte.
    //

    LxtLogInfo("Reading from master");
    LxtCheckErrno(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)));
    LxtCheckFnResults("read", BytesReadWrite, 1);
    LxtCheckEqual(ReadBuffer[0], (TIOCPKT_FLUSHREAD | TIOCPKT_FLUSHWRITE), "%hhd");

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

int PtPacketFlushReadWrite2(PLXT_ARGS Args)

/*++

Routine Description:

    This routine sends a message to the subordinate with packet-mode on, then
    flushes the subordinate read and write queues.


Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int BytesReadWrite;
    int ExpectedResult;
    const char* Greetings = "Hi there!!";
    int PacketMode;
    int PtmFd;
    int PtmFlags;
    int PtsFd;
    char ReadBuffer[20];
    fd_set ReadFds;
    int Result;
    int SerialNumber;
    struct timeval Timeout;

    //
    // Initialize locals.
    //

    PtmFd = -1;
    PtsFd = -1;

    //
    // Open Master-Subordinate.
    //

    LxtCheckErrno(OpenMasterSubordinate(&PtmFd, &PtsFd, NULL, &SerialNumber));
    LxtLogInfo("Master opened at FD:%d", PtmFd);
    LxtLogInfo("Subordinate Serial Number: %d", SerialNumber);
    LxtLogInfo("Subordinate opened at FD:%d", PtsFd);

    //
    // Configure as raw / packet-mode.
    //

    LxtCheckErrno(RawInit(PtsFd));
    PacketMode = 1;
    LxtCheckErrno(ioctl(PtmFd, TIOCPKT, &PacketMode));

    //
    // Write to subordinate.
    //

    ExpectedResult = strlen(Greetings);
    LxtCheckErrno(BytesReadWrite = write(PtsFd, Greetings, ExpectedResult));
    LxtCheckFnResults("write", BytesReadWrite, ExpectedResult);
    LxtCheckErrno(tcdrain(PtsFd));

    //
    // Flush subordinate read and write queues.
    //

    LxtCheckErrno(tcflush(PtsFd, TCIFLUSH));
    LxtCheckErrno(tcflush(PtsFd, TCOFLUSH));

    //
    // Check for available data.
    //

    LxtLogInfo("Verifying master has data to read...");
    memset(&Timeout, 0, sizeof(Timeout));
    FD_ZERO(&ReadFds);
    FD_SET(PtmFd, &ReadFds);
    LxtCheckErrno(select((PtmFd + 1), &ReadFds, NULL, NULL, &Timeout));
    LxtCheckEqual(Result, 1, "%d");

    //
    // Read control byte.
    //

    LxtLogInfo("Reading from master");
    LxtCheckErrno(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)));
    LxtCheckFnResults("read", BytesReadWrite, 1);
    LxtCheckEqual(ReadBuffer[0], (TIOCPKT_FLUSHREAD | TIOCPKT_FLUSHWRITE), "%hhd");

    //
    // No message should be waiting because the write queue was flushed.
    //

    LxtCheckErrno(PtmFlags = fcntl(PtmFd, F_GETFL, 0));
    LxtCheckErrno(fcntl(PtmFd, F_SETFL, (PtmFlags | O_NONBLOCK)));
    LxtCheckErrnoFailure(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)), EAGAIN);
    LxtCheckErrno(fcntl(PtmFd, F_SETFL, PtmFlags));

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

int PtPacketFlushReadWrite3(PLXT_ARGS Args)

/*++

Routine Description:

    This routine sends a message to the subordinate with packet-mode on, then
    flushes the subordinate read and write queues.


Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int BytesReadWrite;
    int ExpectedResult;
    const char* Greetings = "Hi there!!";
    int PacketMode;
    int PtmFd;
    int PtmFlags;
    int PtsFd;
    char ReadBuffer[20];
    fd_set ReadFds;
    int Result;
    int SerialNumber;
    struct timeval Timeout;

    //
    // Initialize locals.
    //

    PtmFd = -1;
    PtsFd = -1;

    //
    // Open Master-Subordinate.
    //

    LxtCheckErrno(OpenMasterSubordinate(&PtmFd, &PtsFd, NULL, &SerialNumber));
    LxtLogInfo("Master opened at FD:%d", PtmFd);
    LxtLogInfo("Subordinate Serial Number: %d", SerialNumber);
    LxtLogInfo("Subordinate opened at FD:%d", PtsFd);

    //
    // Configure as raw / packet-mode.
    //

    LxtCheckErrno(RawInit(PtsFd));
    PacketMode = 1;
    LxtCheckErrno(ioctl(PtmFd, TIOCPKT, &PacketMode));

    //
    // Write to subordinate.
    //

    ExpectedResult = strlen(Greetings);
    LxtCheckErrno(BytesReadWrite = write(PtsFd, Greetings, ExpectedResult));
    LxtCheckFnResults("write", BytesReadWrite, ExpectedResult);
    LxtCheckErrno(tcdrain(PtsFd));

    //
    // Flush subordinate read and write queues.
    //

    LxtCheckErrno(tcflush(PtsFd, TCIOFLUSH));

    //
    // Check for available data.
    //

    LxtLogInfo("Verifying master has data to read...");
    memset(&Timeout, 0, sizeof(Timeout));
    FD_ZERO(&ReadFds);
    FD_SET(PtmFd, &ReadFds);
    LxtCheckErrno(select((PtmFd + 1), &ReadFds, NULL, NULL, &Timeout));
    LxtCheckEqual(Result, 1, "%d");

    //
    // Read control byte.
    //

    LxtLogInfo("Reading from master");
    LxtCheckErrno(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)));
    LxtCheckFnResults("read", BytesReadWrite, 1);
    LxtCheckEqual(ReadBuffer[0], (TIOCPKT_FLUSHREAD | TIOCPKT_FLUSHWRITE), "%hhd");

    //
    // No message should be waiting because the write queue was flushed.
    //

    LxtCheckErrno(PtmFlags = fcntl(PtmFd, F_GETFL, 0));
    LxtCheckErrno(fcntl(PtmFd, F_SETFL, (PtmFlags | O_NONBLOCK)));
    LxtCheckErrnoFailure(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)), EAGAIN);
    LxtCheckErrno(fcntl(PtmFd, F_SETFL, PtmFlags));

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

int PtPacketFlushReadWrite4(PLXT_ARGS Args)

/*++

Routine Description:

    This routine sends a message to the subordinate with packet-mode on, then
    flushes the subordinate read and write queues.


Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int BytesReadWrite;
    int ExpectedResult;
    const char* Greetings = "Hi there!!";
    int PacketMode;
    int PtmFd;
    int PtmFlags;
    int PtsFd;
    char ReadBuffer[20];
    fd_set ReadFds;
    int Result;
    int SerialNumber;
    struct timeval Timeout;

    //
    // Initialize locals.
    //

    PtmFd = -1;
    PtsFd = -1;

    //
    // Open Master-Subordinate.
    //

    LxtCheckErrno(OpenMasterSubordinate(&PtmFd, &PtsFd, NULL, &SerialNumber));
    LxtLogInfo("Master opened at FD:%d", PtmFd);
    LxtLogInfo("Subordinate Serial Number: %d", SerialNumber);
    LxtLogInfo("Subordinate opened at FD:%d", PtsFd);

    //
    // Configure as raw / packet-mode.
    //

    LxtCheckErrno(RawInit(PtsFd));
    PacketMode = 1;
    LxtCheckErrno(ioctl(PtmFd, TIOCPKT, &PacketMode));

    //
    // Flush subordinate write queue.
    //

    LxtCheckErrno(tcflush(PtsFd, TCOFLUSH));

    //
    // Write to subordinate.
    //

    ExpectedResult = strlen(Greetings);
    LxtCheckErrno(BytesReadWrite = write(PtsFd, Greetings, ExpectedResult));
    LxtCheckFnResults("write", BytesReadWrite, ExpectedResult);
    LxtCheckErrno(tcdrain(PtsFd));

    //
    // Flush subordinate read queue.
    //

    LxtCheckErrno(tcflush(PtsFd, TCIFLUSH));

    //
    // Check for available data.
    //

    LxtLogInfo("Verifying master has data to read...");
    memset(&Timeout, 0, sizeof(Timeout));
    FD_ZERO(&ReadFds);
    FD_SET(PtmFd, &ReadFds);
    LxtCheckErrno(select((PtmFd + 1), &ReadFds, NULL, NULL, &Timeout));
    LxtCheckEqual(Result, 1, "%d");

    //
    // Read control byte.
    //

    LxtLogInfo("Reading from master");
    LxtCheckErrno(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)));
    LxtCheckFnResults("read", BytesReadWrite, 1);
    LxtCheckEqual(ReadBuffer[0], (TIOCPKT_FLUSHREAD | TIOCPKT_FLUSHWRITE), "%hhd");

    //
    // Read message from master.
    //

    LxtLogInfo("Reading from master");
    memset(ReadBuffer, 0, sizeof(ReadBuffer));
    LxtCheckErrno(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)));
    LxtCheckFnResults("read", BytesReadWrite, ExpectedResult + 1);
    ReadBuffer[BytesReadWrite] = '\0';
    LxtCheckEqual(ReadBuffer[0], 0, "%hhd");
    LxtCheckStringEqual(ReadBuffer + 1, Greetings);

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

int PtPacketFlushReadWrite5(PLXT_ARGS Args)

/*++

Routine Description:

    This routine sends a message to the subordinate with packet-mode on, then
    flushes the subordinate read and write queues.


Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int BytesReadWrite;
    int ExpectedResult;
    const char* Greetings = "Hi there!!";
    int PacketMode;
    int PtmFd;
    int PtmFlags;
    int PtsFd;
    char ReadBuffer[20];
    fd_set ReadFds;
    int Result;
    int SerialNumber;
    struct timeval Timeout;

    //
    // Initialize locals.
    //

    PtmFd = -1;
    PtsFd = -1;

    //
    // Open Master-Subordinate.
    //

    LxtCheckErrno(OpenMasterSubordinate(&PtmFd, &PtsFd, NULL, &SerialNumber));
    LxtLogInfo("Master opened at FD:%d", PtmFd);
    LxtLogInfo("Subordinate Serial Number: %d", SerialNumber);
    LxtLogInfo("Subordinate opened at FD:%d", PtsFd);

    //
    // Configure as raw / packet-mode.
    //

    LxtCheckErrno(RawInit(PtsFd));
    PacketMode = 1;
    LxtCheckErrno(ioctl(PtmFd, TIOCPKT, &PacketMode));

    //
    // Flush subordinate read queue.
    //

    LxtCheckErrno(tcflush(PtsFd, TCIFLUSH));

    //
    // Write to subordinate.
    //

    ExpectedResult = strlen(Greetings);
    LxtCheckErrno(BytesReadWrite = write(PtsFd, Greetings, ExpectedResult));
    LxtCheckFnResults("write", BytesReadWrite, ExpectedResult);
    LxtCheckErrno(tcdrain(PtsFd));

    //
    // Flush subordinate write queue.
    //

    LxtCheckErrno(tcflush(PtsFd, TCOFLUSH));

    //
    // Check for available data.
    //

    LxtLogInfo("Verifying master has data to read...");
    memset(&Timeout, 0, sizeof(Timeout));
    FD_ZERO(&ReadFds);
    FD_SET(PtmFd, &ReadFds);
    LxtCheckErrno(select((PtmFd + 1), &ReadFds, NULL, NULL, &Timeout));
    LxtCheckEqual(Result, 1, "%d");

    //
    // Read control byte.
    //

    LxtLogInfo("Reading from master");
    LxtCheckErrno(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)));
    LxtCheckFnResults("read", BytesReadWrite, 1);
    LxtCheckEqual(ReadBuffer[0], (TIOCPKT_FLUSHREAD | TIOCPKT_FLUSHWRITE), "%hhd");

    //
    // No message should be waiting because the write queue was flushed.
    //

    LxtCheckErrno(PtmFlags = fcntl(PtmFd, F_GETFL, 0));
    LxtCheckErrno(fcntl(PtmFd, F_SETFL, (PtmFlags | O_NONBLOCK)));
    LxtCheckErrnoFailure(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)), EAGAIN);
    LxtCheckErrno(fcntl(PtmFd, F_SETFL, PtmFlags));

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

int PtPacketHangup(PLXT_ARGS Args)

/*++

Routine Description:

    This routine enables packet mode, then does a read on the master endpoint.
    A second thread hangs up the subordinate.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int BytesReadWrite;
    pid_t ChildPid;
    int ExpectedResult;
    int PacketMode;
    int PtmFd;
    int PtsFd;
    char ReadBuffer[20];
    fd_set ReadFds;
    int Result;
    int Status;
    struct timeval Timeout;

    //
    // Initialize locals
    //

    ChildPid = -1;
    PtmFd = -1;
    PtsFd = -1;

    LXT_SYNCHRONIZATION_POINT_START();
    LxtCheckErrno(ChildPid = ForkPty(&PtmFd, &PtsFd));
    if (ChildPid == 0)
    {

        //
        // Close subordinate for this thread.
        //

        LxtClose(PtsFd);

        //
        // Enable packet mode.
        //

        PacketMode = 1;
        LxtCheckErrno(ioctl(PtmFd, TIOCPKT, &PacketMode));

        //
        // Read control byte.
        //

        LxtLogInfo("Reading from master");
        LxtCheckErrnoFailure(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)), EIO);
    }
    else
    {

        //
        // Give child a chance to wait in read system call.
        //

        sleep(1);

        //
        // Hang up subordinate.
        //

        LxtClose(PtsFd);
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

    LXT_SYNCHRONIZATION_POINT_END();
    return Result;
}

int PtPacketControlCharCheck1(PLXT_ARGS Args)

/*++

Routine Description:

    This routine checks that SIGINT is delivered with a ^C and that a flush
    packet is returned as a side-effect.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    char ReadBuffer[10];
    ssize_t BytesReadWrite;
    pid_t ChildPid;
    cc_t ControlArray[NCCS];
    int PacketMode;
    int PtmFd;
    int PtsFd;
    int PtsFlags;
    fd_set ReadFds;
    int Result;
    int SerialNumber;
    int Status;
    struct timeval Timeout;

    //
    // Initialize locals
    //

    ChildPid = -1;
    PtmFd = -1;
    PtsFd = -1;

    LXT_SYNCHRONIZATION_POINT_START();
    LxtCheckErrno(ChildPid = ForkPty(&PtmFd, &PtsFd));
    if (ChildPid == 0)
    {
        LxtCheckResult(LxtSignalInitialize());
        LxtCheckResult(TerminalSettingsGetControlArray(PtmFd, ControlArray));
        LxtCheckResult(LxtSignalSetupHandler(SIGINT, SA_SIGINFO));

        //
        // Configure as packet-mode.
        //

        PacketMode = 1;
        LxtCheckErrno(ioctl(PtmFd, TIOCPKT, &PacketMode));

        //
        // Write the interrupt control character.
        //

        LxtCheckErrno(BytesReadWrite = write(PtmFd, &ControlArray[VINTR], 1));
        LxtCheckFnResults("write", BytesReadWrite, 1);
        LxtCheckErrno(tcdrain(PtmFd));

        //
        // A SIGINT signal should be generated.
        //

        LxtSignalWait();
        LxtCheckResult(LxtSignalCheckReceived(SIGINT));
        LxtSignalResetReceived();

        //
        // Check for available data.
        //

        LxtLogInfo("Verifying master has data to read...");
        memset(&Timeout, 0, sizeof(Timeout));
        FD_ZERO(&ReadFds);
        FD_SET(PtmFd, &ReadFds);
        LxtCheckErrno(select((PtmFd + 1), &ReadFds, NULL, NULL, &Timeout));
        LxtCheckEqual(Result, 1, "%d");

        //
        // Read control byte.
        //

        LxtLogInfo("Reading from master");
        LxtCheckErrno(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)));
        LxtCheckFnResults("read", BytesReadWrite, 1);
        LxtCheckEqual(ReadBuffer[0], (TIOCPKT_FLUSHREAD | TIOCPKT_FLUSHWRITE), "%hhd");

        //
        // The control character sequence should have been echoed back.
        //

        LxtCheckErrno(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)));
        LxtCheckFnResults("read", BytesReadWrite, 3);
        LxtCheckEqual(ReadBuffer[0], 0, "%hhd");
        LxtCheckTrue(IS_CONTROL_CHAR_ECHO_STRING(ReadBuffer + 1, ControlArray[VINTR]));

        //
        // There should be no character waiting at the subordinate.
        //

        LxtCheckErrno(PtsFlags = fcntl(PtsFd, F_GETFL, 0));
        LxtCheckErrno(fcntl(PtsFd, F_SETFL, (PtsFlags | O_NONBLOCK)));
        LxtCheckErrnoFailure(BytesReadWrite = read(PtsFd, ReadBuffer, sizeof(ReadBuffer)), EAGAIN);
        LxtCheckErrno(fcntl(PtsFd, F_SETFL, PtsFlags));
        Result = 0;
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

    LXT_SYNCHRONIZATION_POINT_END();
    return Result;
}

int PtPacketControlCharCheck2(PLXT_ARGS Args)

/*++

Routine Description:

    This routine checks that changing the STOP/START control character delivers
    a control byte.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int BytesReadWrite;
    cc_t ControlArray[NCCS];
    char DefaultStartChar;
    char DefaultStopChar;
    int PacketMode;
    int PtmFd;
    int PtmFlags;
    int PtsFd;
    char ReadBuffer[1];
    fd_set ReadFds;
    int Result;
    int SerialNumber;
    struct timeval Timeout;

    //

    // Initialize locals.
    //

    PtmFd = -1;
    PtsFd = -1;

    //
    // Open Master-Subordinate.
    //

    LxtCheckErrno(OpenMasterSubordinate(&PtmFd, &PtsFd, NULL, &SerialNumber));
    LxtLogInfo("Master opened at FD:%d", PtmFd);
    LxtLogInfo("Subordinate Serial Number: %d", SerialNumber);
    LxtLogInfo("Subordinate opened at FD:%d", PtsFd);

    //
    // Configure for packet-mode.
    //

    PacketMode = 1;
    LxtCheckErrno(ioctl(PtmFd, TIOCPKT, &PacketMode));

    //
    // Fetch the default control character array values.
    //

    LxtCheckResult(TerminalSettingsGetControlArray(PtmFd, ControlArray));
    DefaultStartChar = ControlArray[VSTART];
    DefaultStopChar = ControlArray[VSTOP];

    //
    // Modify the start control character.
    //

    ControlArray[VSTART] = _POSIX_VDISABLE;
    LxtCheckResult(TerminalSettingsSetControlArray(PtmFd, ControlArray));

    //
    // Check for available data.
    //

    LxtLogInfo("Verifying master has data to read...");
    memset(&Timeout, 0, sizeof(Timeout));
    FD_ZERO(&ReadFds);
    FD_SET(PtmFd, &ReadFds);
    LxtCheckErrno(select((PtmFd + 1), &ReadFds, NULL, NULL, &Timeout));
    LxtCheckEqual(Result, 1, "%d");

    //
    // Read control byte.
    //

    LxtLogInfo("Reading from master");
    LxtCheckErrno(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)));
    LxtCheckFnResults("read", BytesReadWrite, 1);
    LxtCheckEqual(ReadBuffer[0], TIOCPKT_NOSTOP, "%hhd");

    //
    // Modify the stop control character.
    //

    ControlArray[VSTOP] = _POSIX_VDISABLE;
    LxtCheckResult(TerminalSettingsSetControlArray(PtmFd, ControlArray));

    //
    // Should be no change in state.
    //

    LxtLogInfo("Verifying master has data to read...");
    FD_SET(PtmFd, &ReadFds);
    LxtCheckErrno(select((PtmFd + 1), &ReadFds, NULL, NULL, &Timeout));
    LxtCheckEqual(Result, 0, "%d");

    //
    // Restore the start control character.
    //

    ControlArray[VSTART] = DefaultStartChar;
    LxtCheckResult(TerminalSettingsSetControlArray(PtmFd, ControlArray));

    //
    // Should be no change in state.
    //

    LxtLogInfo("Verifying master has data to read...");
    FD_SET(PtmFd, &ReadFds);
    LxtCheckErrno(select((PtmFd + 1), &ReadFds, NULL, NULL, &Timeout));
    LxtCheckEqual(Result, 0, "%d");

    //
    // Restore the stop control character.
    //

    ControlArray[VSTOP] = DefaultStopChar;
    LxtCheckResult(TerminalSettingsSetControlArray(PtmFd, ControlArray));

    //
    // Now should see a control byte.
    //

    LxtLogInfo("Verifying master has data to read...");
    FD_SET(PtmFd, &ReadFds);
    LxtCheckErrno(select((PtmFd + 1), &ReadFds, NULL, NULL, &Timeout));
    LxtCheckEqual(Result, 1, "%d");

    //
    // Read control byte.
    //

    LxtLogInfo("Reading from master");
    LxtCheckErrno(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)));
    LxtCheckFnResults("read", BytesReadWrite, 1);
    LxtCheckEqual(ReadBuffer[0], TIOCPKT_DOSTOP, "%hhd");

    //
    // Finally, modify just the stop control character.
    //

    ControlArray[VSTOP] = _POSIX_VDISABLE;
    LxtCheckResult(TerminalSettingsSetControlArray(PtmFd, ControlArray));

    //
    // Check for available data.
    //

    LxtLogInfo("Verifying master has data to read...");
    FD_SET(PtmFd, &ReadFds);
    LxtCheckErrno(select((PtmFd + 1), &ReadFds, NULL, NULL, &Timeout));
    LxtCheckEqual(Result, 1, "%d");

    //
    // Read control byte.
    //

    LxtLogInfo("Reading from master");
    LxtCheckErrno(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)));
    LxtCheckFnResults("read", BytesReadWrite, 1);
    LxtCheckEqual(ReadBuffer[0], TIOCPKT_NOSTOP, "%hhd");

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

int PtPacketControlCharCheck3(PLXT_ARGS Args)

/*++

Routine Description:

    This routine checks that the STOP/START control characters deliver
    control bytes.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int BytesReadWrite;
    cc_t ControlArray[NCCS];
    int PacketMode;
    int PtmFd;
    int PtmFlags;
    int PtsFd;
    char ReadBuffer[1];
    fd_set ReadFds;
    int Result;
    int SerialNumber;
    struct timeval Timeout;

    //
    // Initialize locals.
    //

    PtmFd = -1;
    PtsFd = -1;

    //
    // Open Master-Subordinate.
    //

    LxtCheckErrno(OpenMasterSubordinate(&PtmFd, &PtsFd, NULL, &SerialNumber));
    LxtLogInfo("Master opened at FD:%d", PtmFd);
    LxtLogInfo("Subordinate Serial Number: %d", SerialNumber);
    LxtLogInfo("Subordinate opened at FD:%d", PtsFd);

    //
    // Configure for packet-mode.
    //

    PacketMode = 1;
    LxtCheckErrno(ioctl(PtmFd, TIOCPKT, &PacketMode));

    //
    // Fetch the default control character array values.
    //

    LxtCheckResult(TerminalSettingsGetControlArray(PtmFd, ControlArray));

    //
    // Send the STOP control character.
    //

    LxtCheckErrno(BytesReadWrite = write(PtmFd, &ControlArray[VSTOP], 1));
    LxtCheckFnResults("write", BytesReadWrite, 1);
    LxtCheckErrno(tcdrain(PtmFd));

    //
    // Ubuntu16 asynchronous pty processing needs some time to process stop.
    //

    sleep(1);

    //
    // Check for available data.
    //

    LxtLogInfo("Verifying master has data to read...");
    memset(&Timeout, 0, sizeof(Timeout));
    FD_ZERO(&ReadFds);
    FD_SET(PtmFd, &ReadFds);
    LxtCheckErrno(select((PtmFd + 1), &ReadFds, NULL, NULL, &Timeout));
    LxtCheckEqual(Result, 1, "%d");

    //
    // Read control byte.
    //

    LxtLogInfo("Reading from master");
    LxtCheckErrno(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)));
    LxtCheckFnResults("read", BytesReadWrite, 1);
    LxtCheckEqual(ReadBuffer[0], TIOCPKT_STOP, "%hhd");

    //
    // Send a few extraneous STOP control character.
    //

    LxtCheckErrno(BytesReadWrite = write(PtmFd, &ControlArray[VSTOP], 1));
    LxtCheckFnResults("write", BytesReadWrite, 1);
    LxtCheckErrno(BytesReadWrite = write(PtmFd, &ControlArray[VSTOP], 1));
    LxtCheckFnResults("write", BytesReadWrite, 1);
    LxtCheckErrno(BytesReadWrite = write(PtmFd, &ControlArray[VSTOP], 1));
    LxtCheckFnResults("write", BytesReadWrite, 1);
    LxtCheckErrno(tcdrain(PtmFd));

    //
    // This should not result in a control byte as state did not change.
    //

    LxtLogInfo("Verifying master has no data to read...");
    FD_SET(PtmFd, &ReadFds);
    LxtCheckErrno(select((PtmFd + 1), &ReadFds, NULL, NULL, &Timeout));
    LxtCheckEqual(Result, 0, "%d");

    //
    // Send the START control character.
    //

    LxtCheckErrno(BytesReadWrite = write(PtmFd, &ControlArray[VSTART], 1));
    LxtCheckFnResults("write", BytesReadWrite, 1);
    LxtCheckErrno(tcdrain(PtmFd));

    //
    // Ubuntu16 asynchronous pty processing needs some time to process start.
    //

    sleep(1);

    //
    // Check for available data.
    //

    LxtLogInfo("Verifying master has data to read...");
    FD_SET(PtmFd, &ReadFds);
    LxtCheckErrno(select((PtmFd + 1), &ReadFds, NULL, NULL, &Timeout));
    LxtCheckEqual(Result, 1, "%d");

    //
    // Read control byte.
    //

    LxtLogInfo("Reading from master");
    LxtCheckErrno(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)));
    LxtCheckFnResults("read", BytesReadWrite, 1);
    LxtCheckEqual(ReadBuffer[0], TIOCPKT_START, "%hhd");

    //
    // Send both the STOP and START control characters.
    //

    LxtCheckErrno(BytesReadWrite = write(PtmFd, &ControlArray[VSTOP], 1));
    LxtCheckFnResults("write", BytesReadWrite, 1);
    LxtCheckErrno(BytesReadWrite = write(PtmFd, &ControlArray[VSTART], 1));
    LxtCheckFnResults("write", BytesReadWrite, 1);
    LxtCheckErrno(tcdrain(PtmFd));

    //
    // Ubuntu16 asynchronous pty processing needs some time to process start.
    //

    sleep(1);

    //
    // Check for control byte
    //

    LxtLogInfo("Verifying master has no data to read...");
    FD_SET(PtmFd, &ReadFds);
    LxtCheckErrno(select((PtmFd + 1), &ReadFds, NULL, NULL, &Timeout));
    LxtCheckEqual(Result, 1, "%d");

    //
    // Read control byte.
    //

    LxtLogInfo("Reading from master");
    LxtCheckErrno(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)));
    LxtCheckFnResults("read", BytesReadWrite, 1);
    LxtCheckEqual(ReadBuffer[0], TIOCPKT_START, "%hhd");

    //
    // Send a few extraneous START control characters.
    //

    LxtCheckErrno(BytesReadWrite = write(PtmFd, &ControlArray[VSTART], 1));
    LxtCheckFnResults("write", BytesReadWrite, 1);
    LxtCheckErrno(BytesReadWrite = write(PtmFd, &ControlArray[VSTART], 1));
    LxtCheckFnResults("write", BytesReadWrite, 1);
    LxtCheckErrno(BytesReadWrite = write(PtmFd, &ControlArray[VSTART], 1));
    LxtCheckFnResults("write", BytesReadWrite, 1);
    LxtCheckErrno(tcdrain(PtmFd));

    //
    // This should not result in a control byte as state did not change.
    //

    LxtLogInfo("Verifying master has no data to read...");
    FD_SET(PtmFd, &ReadFds);
    LxtCheckErrno(select((PtmFd + 1), &ReadFds, NULL, NULL, &Timeout));
    LxtCheckEqual(Result, 0, "%d");

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

int PtPacketToggleWithControlByte(PLXT_ARGS Args)

/*++

Routine Description:

    This routine causes a control byte to be generated and then toggles packet
    mode off and then on to check what state persists.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int BytesReadWrite;
    cc_t ControlArray[NCCS];
    char DefaultStartChar;
    char DefaultStopChar;
    int PacketMode;
    int PtmFd;
    int PtmFlags;
    int PtsFd;
    char ReadBuffer[1];
    fd_set ReadFds;
    int Result;
    int SerialNumber;
    struct timeval Timeout;

    //
    // Initialize locals.
    //

    PtmFd = -1;
    PtsFd = -1;

    //
    // Open Master-Subordinate.
    //

    LxtCheckErrno(OpenMasterSubordinate(&PtmFd, &PtsFd, NULL, &SerialNumber));
    LxtLogInfo("Master opened at FD:%d", PtmFd);
    LxtLogInfo("Subordinate Serial Number: %d", SerialNumber);
    LxtLogInfo("Subordinate opened at FD:%d", PtsFd);

    //
    // Configure for packet-mode.
    //

    PacketMode = 1;
    LxtCheckErrno(ioctl(PtmFd, TIOCPKT, &PacketMode));

    //
    // Fetch the default control character array values.
    //

    LxtCheckResult(TerminalSettingsGetControlArray(PtmFd, ControlArray));
    DefaultStartChar = ControlArray[VSTART];
    DefaultStopChar = ControlArray[VSTOP];

    //
    // Modify the start control character.
    //

    ControlArray[VSTART] = _POSIX_VDISABLE;
    LxtCheckResult(TerminalSettingsSetControlArray(PtmFd, ControlArray));

    //
    // Flush subordinate read and write queues.
    //

    LxtCheckErrno(tcflush(PtsFd, TCIOFLUSH));

    //
    // Send the STOP control character.
    //

    LxtCheckErrno(BytesReadWrite = write(PtmFd, &ControlArray[VSTOP], 1));
    LxtCheckFnResults("write", BytesReadWrite, 1);
    LxtCheckErrno(tcdrain(PtmFd));

    //
    // Ubuntu16 asynchronous pty processing needs some time to process stop.
    //

    sleep(1);

    //
    // Check for available data.
    //

    LxtLogInfo("Verifying master has data to read...");
    memset(&Timeout, 0, sizeof(Timeout));
    FD_ZERO(&ReadFds);
    FD_SET(PtmFd, &ReadFds);
    LxtCheckErrno(select((PtmFd + 1), &ReadFds, NULL, NULL, &Timeout));
    LxtCheckEqual(Result, 1, "%d");

    //
    // Turn off packet-mode.
    //

    PacketMode = 0;
    LxtCheckErrno(ioctl(PtmFd, TIOCPKT, &PacketMode));

    //
    // Check for available data.
    //

    LxtLogInfo("Verifying master has data to read...");
    FD_SET(PtmFd, &ReadFds);
    LxtCheckErrno(select((PtmFd + 1), &ReadFds, NULL, NULL, &Timeout));
    LxtCheckEqual(Result, 0, "%d");

    //
    // Turn on packet-mode.
    //

    PacketMode = 1;
    LxtCheckErrno(ioctl(PtmFd, TIOCPKT, &PacketMode));

    //
    // Check for available data.
    //

    LxtLogInfo("Verifying master has no data to read...");
    FD_SET(PtmFd, &ReadFds);
    LxtCheckErrno(select((PtmFd + 1), &ReadFds, NULL, NULL, &Timeout));
    LxtCheckEqual(Result, 0, "%d");

    //
    // Read control byte.
    //

    // LxtLogInfo("Reading from master");
    // LxtCheckErrno(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)));
    // LxtCheckFnResults("read", BytesReadWrite, 1);
    // LxtCheckEqual(ReadBuffer[0], TIOCPKT_NOSTOP, "%hhd");

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

int PtSessionBasicMaster(PLXT_ARGS Args)

/*++

Routine Description:

    This routine performs simple session controlling terminal tests.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    char Buffer[64];
    pid_t ChildPid;
    pid_t ForegroundId;
    pid_t SelfPid;
    int PtmFd;
    int PtsFd;
    int Result;
    int SerialNumber;
    pid_t SessionId;
    int Status;
    pid_t TerminalForegroundId;
    pid_t TerminalSessionId;
    int TtyFd;

    //
    // Initialize locals
    //

    ChildPid = -1;
    PtmFd = -1;
    PtsFd = -1;
    LXT_SYNCHRONIZATION_POINT_START();

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

        //
        // Current session should already have a controlling terminal, so
        // expect a failure trying to set a new one.
        //

        LxtCheckErrnoFailure(ioctl(PtmFd, TIOCSCTTY, (char*)NULL), EPERM);

        //
        // Move to a new session
        //

        LxtLogInfo("Creating new session and verifying state.");
        LxtCheckErrno(SessionId = setsid());
        LxtCheckResult(SelfPid = getpid());
        LxtCheckResult(SessionId = getsid(0));
        LxtCheckEqual(SelfPid, SessionId, "%d");
        LxtCheckResult(LxtSignalInitialize());
        LxtCheckResult(LxtSignalSetupHandler(SIGTTOU, SA_SIGINFO));
        LxtCheckResult(LxtSignalSetupHandler(SIGTTIN, SA_SIGINFO));
        LxtCheckErrnoFailure(tcgetsid(PtsFd), ENOTTY);
        LxtCheckErrnoFailure(tcgetsid(PtmFd), ENOTTY);
        LxtCheckErrnoFailure(tcgetpgrp(PtsFd), ENOTTY);
        LxtCheckErrno(tcgetpgrp(PtmFd));
        LxtCheckEqual(Result, 0, "%d");
        LxtCheckErrnoFailure((TtyFd = open("/dev/tty", O_RDONLY)), ENXIO);

        //
        // Set the master endpoint as the controlling terminal for the session.
        //

        LxtLogInfo("Setting master endpoint as the controlling terminal.");
        LxtCheckErrno(ioctl(PtmFd, TIOCSCTTY, (char*)NULL));
        LxtCheckErrno(ioctl(PtmFd, TIOCSCTTY, (char*)NULL));

        //
        // Check current state.
        //

        LxtCheckErrnoFailure(tcgetsid(PtsFd), ENOTTY);
        LxtCheckErrnoFailure(tcgetsid(PtmFd), ENOTTY);
        LxtCheckErrnoFailure(tcgetpgrp(PtsFd), ENOTTY);
        LxtCheckErrno(TerminalForegroundId = tcgetpgrp(PtmFd));
        LxtCheckEqual(TerminalForegroundId, 0, "%d");
        LxtCheckErrno(RawInit(PtsFd));
        LxtCheckErrno(SimpleReadWriteCheck(PtmFd, PtsFd));
        LxtCheckResult(LxtSignalCheckNoSignal());
        LxtCheckErrnoFailure((TtyFd = open("/dev/tty", O_RDONLY)), EIO);
        // LxtCheckErrno((TtyFd = open("/dev/tty", O_RDWR)));
        // LxtCheckErrno(ttyname_r(TtyFd, Buffer, sizeof(Buffer)));
        // LxtClose(TtyFd);
        // LxtLogInfo("Controlling terminal: %s", Buffer);

        //
        // Remove the master endpoint as the controlling terminal.
        //

        LxtLogInfo("Verifying locked controlling terminal.");
        LxtCheckErrnoFailure(ioctl(PtsFd, TIOCSCTTY, (char*)NULL), EPERM);
        LxtLogInfo("Removing master endpoint as controlling terminal.");
        LxtCheckResult(LxtSignalSetupHandler(SIGHUP, SA_SIGINFO));
        LxtCheckErrno(ioctl(PtmFd, TIOCNOTTY, (char*)NULL));
        LxtCheckResult(LxtSignalCheckReceived(SIGHUP));
        LxtSignalResetReceived();

        //
        // Now try to set the subordinate endpoint as the controlling terminal.
        //

        LxtLogInfo("Adding subordinate endpoint as controlling terminal and verifying state.");
        LxtCheckErrno(ioctl(PtsFd, TIOCSCTTY, (char*)NULL));
        LxtCheckErrno((TtyFd = open("/dev/tty", O_RDWR)));
        LxtCheckErrno(ttyname_r(TtyFd, Buffer, sizeof(Buffer)));
        LxtClose(TtyFd);
        LxtLogInfo("Controlling terminal: %s", Buffer);
        LxtLogInfo("Controlling terminal: %s", Buffer);
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

    LXT_SYNCHRONIZATION_POINT_END();
    return Result;
}

int PtSuspendOutput1(PLXT_ARGS Args)

/*++

Routine Description:

    This routine checks suspend output support on the subordinate endpoint.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int BytesReadWrite;
    int ExpectedResult;
    int FileFlags;
    const char* Greetings = "Hi there!!";
    int PtmFd;
    int PtsFd;
    char ReadBuffer[20];
    int Result;
    int SerialNumber;
    struct timeval Timeout;
    fd_set WriteFds;

    //
    // Initialize locals.
    //

    PtmFd = -1;
    PtsFd = -1;

    //
    // Open Master-Subordinate.
    //

    LxtCheckErrno(OpenMasterSubordinate(&PtmFd, &PtsFd, NULL, &SerialNumber));
    LxtLogInfo("Master opened at FD:%d", PtmFd);
    LxtLogInfo("Subordinate Serial Number: %d", SerialNumber);
    LxtLogInfo("Subordinate opened at FD:%d", PtsFd);

    //
    // subordinate endpoint should be ready for write.
    //

    memset(&Timeout, 0, sizeof(Timeout));
    FD_ZERO(&WriteFds);
    FD_SET(PtsFd, &WriteFds);
    LxtCheckErrno(select((PtsFd + 1), NULL, &WriteFds, NULL, &Timeout));
    LxtCheckEqual(Result, 1, "%d");

    //
    // Suspend output on subordinate.
    //

    LxtCheckErrno(tcflow(PtsFd, TCOOFF));

    //
    // subordinate endpoint should not be ready for write.
    //

    FD_SET(PtsFd, &WriteFds);
    LxtCheckErrno(select((PtsFd + 1), NULL, &WriteFds, NULL, &Timeout));
    LxtCheckEqual(Result, 0, "%d");

    //
    // Try to write to subordinate.
    //

    LxtLogInfo("Attempt to write to suspended subordinate...");
    ExpectedResult = strlen(Greetings);
    LxtCheckErrno(FileFlags = fcntl(PtsFd, F_GETFL, 0));
    LxtCheckErrno(fcntl(PtsFd, F_SETFL, (FileFlags | O_NONBLOCK)));
    LxtCheckErrnoFailure(BytesReadWrite = write(PtsFd, Greetings, ExpectedResult), EAGAIN);
    LxtCheckErrno(fcntl(PtsFd, F_SETFL, FileFlags));

    //
    // Verify flush and drain are not effected.
    //

    LxtLogInfo("Attempting drain...");
    LxtCheckErrno(tcdrain(PtsFd));
    LxtLogInfo("Attempting flush...");
    LxtCheckErrno(tcflush(PtsFd, TCIOFLUSH));

    //
    // Restart output on subordinate.
    //

    LxtCheckErrno(tcflow(PtsFd, TCOON));

    //
    // Try to write to subordinate.
    //

    LxtLogInfo("Write to subordinate...");
    ExpectedResult = strlen(Greetings);
    LxtCheckErrno(BytesReadWrite = write(PtsFd, Greetings, ExpectedResult));
    LxtCheckFnResults("write", BytesReadWrite, ExpectedResult);

    //
    // Check for message on master.
    //

    LxtLogInfo("Checking for message on master");
    memset(ReadBuffer, 0, sizeof(ReadBuffer));
    LxtCheckErrno(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)));
    LxtCheckFnResults("read", BytesReadWrite, ExpectedResult);
    ReadBuffer[BytesReadWrite] = '\0';
    LxtCheckStringEqual(ReadBuffer, Greetings);

    //
    // No output expected for subordinate.
    //

    LxtCheckErrno(FileFlags = fcntl(PtsFd, F_GETFL, 0));
    LxtCheckErrno(fcntl(PtsFd, F_SETFL, (FileFlags | O_NONBLOCK)));
    LxtCheckErrnoFailure(BytesReadWrite = read(PtsFd, ReadBuffer, sizeof(ReadBuffer)), EAGAIN);
    LxtCheckErrno(fcntl(PtsFd, F_SETFL, FileFlags));

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

int PtSuspendOutput2(PLXT_ARGS Args)

/*++

Routine Description:

    This routine checks suspend output support on the subordinate endpoint
    using the control characters.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int BytesReadWrite;
    cc_t ControlArray[NCCS];
    int ExpectedResult;
    int FileFlags;
    const char* Greetings = "Hi there!!";
    int PtmFd;
    int PtsFd;
    char ReadBuffer[20];
    int Result;
    int SerialNumber;
    struct timeval Timeout;
    fd_set WriteFds;

    //
    // Initialize locals.
    //

    PtmFd = -1;
    PtsFd = -1;

    //
    // Open Master-Subordinate.
    //

    LxtCheckErrno(OpenMasterSubordinate(&PtmFd, &PtsFd, NULL, &SerialNumber));
    LxtLogInfo("Master opened at FD:%d", PtmFd);
    LxtLogInfo("Subordinate Serial Number: %d", SerialNumber);
    LxtLogInfo("Subordinate opened at FD:%d", PtsFd);

    //
    // Get control characters.
    //

    LxtCheckResult(TerminalSettingsGetControlArray(PtmFd, ControlArray));

    //
    // subordinate endpoint should be ready for write.
    //

    memset(&Timeout, 0, sizeof(Timeout));
    FD_ZERO(&WriteFds);
    FD_SET(PtsFd, &WriteFds);
    LxtCheckErrno(select((PtsFd + 1), NULL, &WriteFds, NULL, &Timeout));
    LxtCheckEqual(Result, 1, "%d");

    //
    // Suspend output on subordinate.
    //

    LxtCheckErrno(BytesReadWrite = write(PtmFd, &ControlArray[VSTOP], 1));
    LxtCheckFnResults("write", BytesReadWrite, 1);
    LxtCheckErrno(tcdrain(PtmFd));

    //
    // Ubuntu16 asynchronous pty processing needs some time to process stop.
    //

    sleep(1);

    //
    // subordinate endpoint should not be ready for write.
    //

    FD_SET(PtsFd, &WriteFds);
    LxtCheckErrno(select((PtsFd + 1), NULL, &WriteFds, NULL, &Timeout));
    LxtCheckEqual(Result, 0, "%d");

    //
    // Try to write to subordinate.
    //

    LxtLogInfo("Attempt to write to suspended subordinate...");
    ExpectedResult = strlen(Greetings);
    LxtCheckErrno(FileFlags = fcntl(PtsFd, F_GETFL, 0));
    LxtCheckErrno(fcntl(PtsFd, F_SETFL, (FileFlags | O_NONBLOCK)));
    LxtCheckErrnoFailure(BytesReadWrite = write(PtsFd, Greetings, ExpectedResult), EAGAIN);
    LxtCheckErrno(fcntl(PtsFd, F_SETFL, FileFlags));

    //
    // Restart output on subordinate.
    //

    LxtCheckErrno(BytesReadWrite = write(PtmFd, &ControlArray[VSTART], 1));
    LxtCheckFnResults("write", BytesReadWrite, 1);
    LxtCheckErrno(tcdrain(PtmFd));

    //
    // Ubuntu16 asynchronous pty processing needs some time to process start.
    //

    sleep(1);

    //
    // subordinate endpoint should be ready for write.
    //

    FD_SET(PtsFd, &WriteFds);
    LxtCheckErrno(select((PtsFd + 1), NULL, &WriteFds, NULL, &Timeout));
    LxtCheckEqual(Result, 1, "%d");

    //
    // Try to write to subordinate.
    //

    LxtLogInfo("Write to subordinate...");
    ExpectedResult = strlen(Greetings);
    LxtCheckErrno(BytesReadWrite = write(PtsFd, Greetings, ExpectedResult));
    LxtCheckFnResults("write", BytesReadWrite, ExpectedResult);

    //
    // Check for message on master.
    //

    LxtLogInfo("Checking for message on master");
    memset(ReadBuffer, 0, sizeof(ReadBuffer));
    LxtCheckErrno(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)));
    LxtCheckFnResults("read", BytesReadWrite, ExpectedResult);
    ReadBuffer[BytesReadWrite] = '\0';
    LxtCheckStringEqual(ReadBuffer, Greetings);

    //
    // No output expected for subordinate.
    //

    LxtCheckErrno(FileFlags = fcntl(PtsFd, F_GETFL, 0));
    LxtCheckErrno(fcntl(PtsFd, F_SETFL, (FileFlags | O_NONBLOCK)));
    LxtCheckErrnoFailure(BytesReadWrite = read(PtsFd, ReadBuffer, sizeof(ReadBuffer)), EAGAIN);
    LxtCheckErrno(fcntl(PtsFd, F_SETFL, FileFlags));

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

int PtSuspendOutput3(PLXT_ARGS Args)

/*++

Routine Description:

    This routine checks suspend output support on the master endpoint.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int BytesReadWrite;
    int ExpectedResult;
    int FileFlags;
    const char* Greetings = "Hi there!!\n";
    const char* GreetingsEcho = "Hi there!!\r\n";
    int PtmFd;
    int PtsFd;
    char ReadBuffer[20];
    int Result;
    int SerialNumber;
    struct timeval Timeout;
    fd_set WriteFds;

    //
    // Initialize locals.
    //

    PtmFd = -1;
    PtsFd = -1;

    //
    // Open Master-Subordinate.
    //

    LxtCheckErrno(OpenMasterSubordinate(&PtmFd, &PtsFd, NULL, &SerialNumber));
    LxtLogInfo("Master opened at FD:%d", PtmFd);
    LxtLogInfo("Subordinate Serial Number: %d", SerialNumber);
    LxtLogInfo("Subordinate opened at FD:%d", PtsFd);

    //
    // Master endpoint should be ready for write.
    //

    memset(&Timeout, 0, sizeof(Timeout));
    FD_ZERO(&WriteFds);
    FD_SET(PtmFd, &WriteFds);
    LxtCheckErrno(select((PtmFd + 1), NULL, &WriteFds, NULL, &Timeout));
    LxtCheckEqual(Result, 1, "%d");

    //
    // Suspend output on master.
    //

    LxtCheckErrno(tcflow(PtmFd, TCOOFF));

    //
    // Master endpoint should not be ready for write.
    //

    FD_SET(PtmFd, &WriteFds);
    LxtCheckErrno(select((PtmFd + 1), NULL, &WriteFds, NULL, &Timeout));
    LxtCheckEqual(Result, 0, "%d");

    //
    // Try to write to master.
    //

    LxtLogInfo("Attempt to write to suspended master...");
    ExpectedResult = strlen(Greetings);
    LxtCheckErrno(FileFlags = fcntl(PtmFd, F_GETFL, 0));
    LxtCheckErrno(fcntl(PtmFd, F_SETFL, (FileFlags | O_NONBLOCK)));
    LxtCheckErrnoFailure(BytesReadWrite = write(PtmFd, Greetings, ExpectedResult), EAGAIN);
    LxtCheckErrno(fcntl(PtmFd, F_SETFL, FileFlags));

    //
    // Verify flush and drain are not effected.
    //

    LxtLogInfo("Attempting drain...");
    LxtCheckErrno(tcdrain(PtmFd));
    LxtLogInfo("Attempting flush...");
    LxtCheckErrno(tcflush(PtmFd, TCIOFLUSH));

    //
    // Restart output on master.
    //

    LxtCheckErrno(tcflow(PtmFd, TCOON));

    //
    // Master endpoint should be ready for write.
    //

    FD_SET(PtmFd, &WriteFds);
    LxtCheckErrno(select((PtmFd + 1), NULL, &WriteFds, NULL, &Timeout));
    LxtCheckEqual(Result, 1, "%d");

    //
    // Try to write to master.
    //

    LxtLogInfo("Write to master...");
    ExpectedResult = strlen(Greetings);
    LxtCheckErrno(BytesReadWrite = write(PtmFd, Greetings, ExpectedResult));
    LxtCheckFnResults("write", BytesReadWrite, ExpectedResult);

    //
    // Check for message on subordinate.
    //

    LxtLogInfo("Checking for message on subordinate");
    memset(ReadBuffer, 0, sizeof(ReadBuffer));
    LxtCheckErrno(BytesReadWrite = read(PtsFd, ReadBuffer, sizeof(ReadBuffer)));
    LxtCheckFnResults("read", BytesReadWrite, ExpectedResult);
    ReadBuffer[BytesReadWrite] = '\0';
    LxtCheckStringEqual(ReadBuffer, Greetings);

    //
    // Check echo output.
    //

    ExpectedResult = strlen(GreetingsEcho);
    LxtCheckErrno(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)));
    LxtCheckFnResults("read", BytesReadWrite, ExpectedResult);
    ReadBuffer[BytesReadWrite] = '\0';
    LxtCheckStringEqual(ReadBuffer, GreetingsEcho);

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

int PtSuspendOutput4(PLXT_ARGS Args)

/*++

Routine Description:

    This routine checks suspend output support on the subordinate endpoint via
    TCI(OFF/ON) from the master.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int BytesReadWrite;
    int ExpectedResult;
    int FileFlags;
    const char* Greetings = "Hi there!!\n";
    const char* GreetingsOut = "Hi there!!\r\n";
    int PtmFd;
    int PtsFd;
    char ReadBuffer[20];
    int Result;
    int SerialNumber;
    struct timeval Timeout;
    fd_set WriteFds;

    //
    // Initialize locals.
    //

    PtmFd = -1;
    PtsFd = -1;

    //
    // Open Master-Subordinate.
    //

    LxtCheckErrno(OpenMasterSubordinate(&PtmFd, &PtsFd, NULL, &SerialNumber));
    LxtLogInfo("Master opened at FD:%d", PtmFd);
    LxtLogInfo("Subordinate Serial Number: %d", SerialNumber);
    LxtLogInfo("Subordinate opened at FD:%d", PtsFd);

    //
    // Subordinate endpoint should be ready for write.
    //

    memset(&Timeout, 0, sizeof(Timeout));
    FD_ZERO(&WriteFds);
    FD_SET(PtsFd, &WriteFds);
    LxtCheckErrno(select((PtsFd + 1), NULL, &WriteFds, NULL, &Timeout));
    LxtCheckEqual(Result, 1, "%d");

    //
    // Suspend output on subordinate.
    //

    LxtCheckErrno(tcflow(PtmFd, TCIOFF));

    //
    // Ubuntu16 asynchronous pty processing needs some time to process stop.
    //

    sleep(1);

    //
    // Subordinate endpoint should not be ready for write.
    //

    FD_SET(PtsFd, &WriteFds);
    LxtCheckErrno(select((PtsFd + 1), NULL, &WriteFds, NULL, &Timeout));
    LxtCheckEqual(Result, 0, "%d");

    //
    // Try to write to subordinate.
    //

    LxtLogInfo("Attempt to write to suspended subordinate...");
    ExpectedResult = strlen(Greetings);
    LxtCheckErrno(FileFlags = fcntl(PtsFd, F_GETFL, 0));
    LxtCheckErrno(fcntl(PtsFd, F_SETFL, (FileFlags | O_NONBLOCK)));
    LxtCheckErrnoFailure(BytesReadWrite = write(PtsFd, Greetings, ExpectedResult), EAGAIN);
    LxtCheckErrno(fcntl(PtsFd, F_SETFL, FileFlags));

    //
    // Restart output on subordinate.
    //

    LxtCheckErrno(tcflow(PtmFd, TCION));

    //
    // Ubuntu16 asynchronous pty processing needs some time to process start.
    //

    sleep(1);

    //
    // Subordinate endpoint should be ready for write.
    //

    FD_SET(PtsFd, &WriteFds);
    LxtCheckErrno(select((PtsFd + 1), NULL, &WriteFds, NULL, &Timeout));
    LxtCheckEqual(Result, 1, "%d");

    //
    // Try to write to subordinate.
    //

    LxtLogInfo("Write to subordinate...");
    ExpectedResult = strlen(Greetings);
    LxtCheckErrno(BytesReadWrite = write(PtsFd, Greetings, ExpectedResult));
    LxtCheckFnResults("write", BytesReadWrite, ExpectedResult);

    //
    // Check for message on master.
    //

    LxtLogInfo("Checking for message on master");
    ExpectedResult = strlen(GreetingsOut);
    memset(ReadBuffer, 0, sizeof(ReadBuffer));
    LxtCheckErrno(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)));
    LxtCheckFnResults("read", BytesReadWrite, ExpectedResult);
    ReadBuffer[BytesReadWrite] = '\0';
    LxtCheckStringEqual(ReadBuffer, GreetingsOut);

    //
    // No output expected for subordinate.
    //

    LxtCheckErrno(FileFlags = fcntl(PtsFd, F_GETFL, 0));
    LxtCheckErrno(fcntl(PtsFd, F_SETFL, (FileFlags | O_NONBLOCK)));
    LxtCheckErrnoFailure(BytesReadWrite = read(PtsFd, ReadBuffer, sizeof(ReadBuffer)), EAGAIN);
    LxtCheckErrno(fcntl(PtsFd, F_SETFL, FileFlags));

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

int PtSuspendOutput5(PLXT_ARGS Args)

/*++

Routine Description:

    This routine checks the control characters transmitted via TCI(ON/OFF).

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int BytesReadWrite;
    cc_t ControlArray[NCCS];
    int ExpectedResult;
    int FileFlags;
    const char* Greetings = "Hi there!!";
    tcflag_t InputFlags;
    int PtmFd;
    int PtsFd;
    char ReadBuffer[20];
    int Result;
    int SerialNumber;
    struct timeval Timeout;
    fd_set WriteFds;

    //
    // Initialize locals.
    //

    PtmFd = -1;
    PtsFd = -1;

    //
    // Open Master-Subordinate.
    //

    LxtCheckErrno(OpenMasterSubordinate(&PtmFd, &PtsFd, NULL, &SerialNumber));
    LxtLogInfo("Master opened at FD:%d", PtmFd);
    LxtLogInfo("Subordinate Serial Number: %d", SerialNumber);
    LxtLogInfo("Subordinate opened at FD:%d", PtsFd);

    //
    // Get default control characters.
    //

    LxtCheckResult(TerminalSettingsGetControlArray(PtmFd, ControlArray));

    //
    // Turn off IXON to disable START/STOP characters.
    //

    LxtCheckResult(TerminalSettingsGetInputFlags(PtsFd, &InputFlags));
    LxtCheckResult(TerminalSettingsSetInputFlags(PtsFd, InputFlags & ~IXON));

    //
    // Suspend output on subordinate.
    //

    LxtCheckErrno(tcflow(PtmFd, TCIOFF));

    //
    // Ubuntu16 asynchronous pty processing needs some time to process stop.
    //

    sleep(1);

    //
    // Subordinate endpoint should be ready for write.
    //

    memset(&Timeout, 0, sizeof(Timeout));
    FD_ZERO(&WriteFds);
    FD_SET(PtsFd, &WriteFds);
    LxtCheckErrno(select((PtsFd + 1), NULL, &WriteFds, NULL, &Timeout));
    LxtCheckEqual(Result, 1, "%d");

    //
    // Check for echo to master.
    //

    LxtLogInfo("Checking for echo to master");
    memset(ReadBuffer, 0, sizeof(ReadBuffer));
    LxtCheckErrno(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)));
    LxtCheckFnResults("read", BytesReadWrite, 2);
    ReadBuffer[BytesReadWrite] = '\0';
    LxtCheckStringEqual(ReadBuffer, PTS_START_CONTROL_CHAR);

    //
    // Try to write to subordinate.
    //

    LxtLogInfo("Write to subordinate...");
    ExpectedResult = strlen(Greetings);
    LxtCheckErrno(BytesReadWrite = write(PtsFd, Greetings, ExpectedResult));
    LxtCheckFnResults("write", BytesReadWrite, ExpectedResult);

    //
    // Check for message on master.
    //

    LxtLogInfo("Checking for message on subordinate");
    memset(ReadBuffer, 0, sizeof(ReadBuffer));
    LxtCheckErrno(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)));
    LxtCheckFnResults("read", BytesReadWrite, ExpectedResult);
    ReadBuffer[BytesReadWrite] = '\0';
    LxtCheckStringEqual(ReadBuffer, Greetings);

    //
    // Restart output on subordinate.
    //

    LxtCheckErrno(tcflow(PtmFd, TCION));

    //
    // Ubuntu16 asynchronous pty processing needs some time to process start.
    //

    sleep(1);

    //
    // Subordinate endpoint should be ready for write.
    //

    FD_SET(PtsFd, &WriteFds);
    LxtCheckErrno(select((PtsFd + 1), NULL, &WriteFds, NULL, &Timeout));
    LxtCheckEqual(Result, 1, "%d");

    //
    // Check for echo to master.
    //

    LxtLogInfo("Checking for message on subordinate");
    memset(ReadBuffer, 0, sizeof(ReadBuffer));
    LxtCheckErrno(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)));
    LxtCheckFnResults("read", BytesReadWrite, 2);
    ReadBuffer[BytesReadWrite] = '\0';
    LxtCheckStringEqual(ReadBuffer, PTS_STOP_CONTROL_CHAR);

    //
    // No output expected for subordinate.
    //

    LxtCheckErrno(FileFlags = fcntl(PtsFd, F_GETFL, 0));
    LxtCheckErrno(fcntl(PtsFd, F_SETFL, (FileFlags | O_NONBLOCK)));
    LxtCheckErrnoFailure(BytesReadWrite = read(PtsFd, ReadBuffer, sizeof(ReadBuffer)), EAGAIN);
    LxtCheckErrno(fcntl(PtsFd, F_SETFL, FileFlags));

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

int PtSuspendOutput6(PLXT_ARGS Args)

/*++

Routine Description:

    This routine checks that alternate control characters do not effect
    TCI(ON/OFF), rather they keep it from working properly.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int BytesReadWrite;
    cc_t ControlArray[NCCS];
    int ExpectedResult;
    int FileFlags;
    const char* Greetings = "Hi there!!";
    int PtmFd;
    int PtsFd;
    char ReadBuffer[20];
    int Result;
    int SerialNumber;
    struct timeval Timeout;
    fd_set WriteFds;

    //
    // Initialize locals.
    //

    PtmFd = -1;
    PtsFd = -1;

    //
    // Open Master-Subordinate.
    //

    LxtCheckErrno(OpenMasterSubordinate(&PtmFd, &PtsFd, NULL, &SerialNumber));
    LxtLogInfo("Master opened at FD:%d", PtmFd);
    LxtLogInfo("Subordinate Serial Number: %d", SerialNumber);
    LxtLogInfo("Subordinate opened at FD:%d", PtsFd);

    //
    // Change START/STOP control characters.
    //

    LxtCheckResult(TerminalSettingsGetControlArray(PtsFd, ControlArray));
    ControlArray[VSTART] = 1;
    ControlArray[VSTOP] = 2;
    LxtCheckResult(TerminalSettingsSetControlArray(PtsFd, ControlArray));

    //
    // Suspend output on subordinate.
    //

    LxtCheckErrno(tcflow(PtmFd, TCIOFF));

    //
    // Ubuntu16 asynchronous pty processing needs some time to process stop.
    //

    sleep(1);

    //
    // Subordinate endpoint should be ready for write.
    //

    memset(&Timeout, 0, sizeof(Timeout));
    FD_ZERO(&WriteFds);
    FD_SET(PtsFd, &WriteFds);
    LxtCheckErrno(select((PtsFd + 1), NULL, &WriteFds, NULL, &Timeout));
    LxtCheckEqual(Result, 1, "%d");

    //
    // Check for message on master.
    //

    LxtLogInfo("Checking for message on master");
    memset(ReadBuffer, 0, sizeof(ReadBuffer));
    LxtCheckErrno(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)));
    LxtCheckFnResults("read", BytesReadWrite, 2);
    ReadBuffer[BytesReadWrite] = '\0';
    LxtCheckStringEqual(ReadBuffer, PTS_START_CONTROL_CHAR);

    //
    // Try to write to subordinate.
    //

    LxtLogInfo("Write to subordinate...");
    ExpectedResult = strlen(Greetings);
    LxtCheckErrno(BytesReadWrite = write(PtsFd, Greetings, ExpectedResult));
    LxtCheckFnResults("write", BytesReadWrite, ExpectedResult);

    //
    // Check for message on master.
    //

    LxtLogInfo("Checking for message on master");
    memset(ReadBuffer, 0, sizeof(ReadBuffer));
    LxtCheckErrno(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)));
    LxtCheckFnResults("read", BytesReadWrite, ExpectedResult);
    ReadBuffer[BytesReadWrite] = '\0';
    LxtCheckStringEqual(ReadBuffer, Greetings);

    //
    // Restart output on subordinate.
    //

    LxtCheckErrno(tcflow(PtmFd, TCION));

    //
    // Ubuntu16 asynchronous pty processing needs some time to process start.
    //

    sleep(1);

    //
    // Subordinate endpoint should be ready for write.
    //

    FD_SET(PtsFd, &WriteFds);
    LxtCheckErrno(select((PtsFd + 1), NULL, &WriteFds, NULL, &Timeout));
    LxtCheckEqual(Result, 1, "%d");

    //
    // Check for message on master.
    //

    LxtLogInfo("Checking for message on master");
    memset(ReadBuffer, 0, sizeof(ReadBuffer));
    LxtCheckErrno(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)));
    LxtCheckFnResults("read", BytesReadWrite, 2);
    ReadBuffer[BytesReadWrite] = '\0';
    LxtCheckStringEqual(ReadBuffer, PTS_STOP_CONTROL_CHAR);

    //
    // No output expected for subordinate.
    //

    LxtCheckErrno(FileFlags = fcntl(PtsFd, F_GETFL, 0));
    LxtCheckErrno(fcntl(PtsFd, F_SETFL, (FileFlags | O_NONBLOCK)));
    LxtCheckErrnoFailure(BytesReadWrite = read(PtsFd, ReadBuffer, sizeof(ReadBuffer)), EAGAIN);
    LxtCheckErrno(fcntl(PtsFd, F_SETFL, FileFlags));

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

int PtSuspendOutput7(PLXT_ARGS Args)

/*++

Routine Description:

    This routine checks that TCI(OFF/ON) from the subordinate have no effect on
    the master endpoint.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int BytesReadWrite;
    cc_t ControlArray[NCCS];
    int ExpectedResult;
    int FileFlags;
    const char* Greetings = "Hi there!!\n";
    const char* GreetingsOut = "Hi there!!\r\n";
    int PtmFd;
    int PtsFd;
    char ReadBuffer[20];
    int Result;
    int SerialNumber;
    struct timeval Timeout;
    fd_set WriteFds;

    //
    // Initialize locals.
    //

    PtmFd = -1;
    PtsFd = -1;

    //
    // Open Master-Subordinate.
    //

    LxtCheckErrno(OpenMasterSubordinate(&PtmFd, &PtsFd, NULL, &SerialNumber));
    LxtLogInfo("Master opened at FD:%d", PtmFd);
    LxtLogInfo("Subordinate Serial Number: %d", SerialNumber);
    LxtLogInfo("Subordinate opened at FD:%d", PtsFd);

    //
    // Get default control characters.
    //

    LxtCheckResult(TerminalSettingsGetControlArray(PtsFd, ControlArray));

    //
    // Suspend output on master.
    //

    LxtCheckErrno(tcflow(PtsFd, TCIOFF));

    //
    // Ubuntu16 asynchronous pty processing needs some time to process stop.
    //

    sleep(1);

    //
    // Master endpoint should be ready for write.
    //

    memset(&Timeout, 0, sizeof(Timeout));
    FD_ZERO(&WriteFds);
    FD_SET(PtmFd, &WriteFds);
    LxtCheckErrno(select((PtmFd + 1), NULL, &WriteFds, NULL, &Timeout));
    LxtCheckEqual(Result, 1, "%d");

    //
    // Check for "suspend" character on master.
    //

    LxtLogInfo("Checking for message on master");
    LxtCheckErrno(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)));
    LxtCheckFnResults("read", BytesReadWrite, 1);
    LxtCheckEqual(ReadBuffer[0], ControlArray[VSTOP], "%hhd");

    //
    // Try to write to master.
    //

    LxtLogInfo("Write to master...");
    ExpectedResult = strlen(Greetings);
    LxtCheckErrno(BytesReadWrite = write(PtmFd, Greetings, ExpectedResult));
    LxtCheckFnResults("write", BytesReadWrite, ExpectedResult);

    //
    // Check for message on subordinate.
    //

    LxtLogInfo("Checking for message on subordinate");
    memset(ReadBuffer, 0, sizeof(ReadBuffer));
    LxtCheckErrno(BytesReadWrite = read(PtsFd, ReadBuffer, sizeof(ReadBuffer)));
    LxtCheckFnResults("read", BytesReadWrite, ExpectedResult);
    ReadBuffer[BytesReadWrite] = '\0';
    LxtCheckStringEqual(ReadBuffer, Greetings);

    //
    // Check for echo to master.
    //

    LxtLogInfo("Checking for message on subordinate");
    ExpectedResult = strlen(GreetingsOut);
    memset(ReadBuffer, 0, sizeof(ReadBuffer));
    LxtCheckErrno(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)));
    LxtCheckFnResults("read", BytesReadWrite, ExpectedResult);
    ReadBuffer[BytesReadWrite] = '\0';
    LxtCheckStringEqual(ReadBuffer, GreetingsOut);

    //
    // Restart output on master.
    //

    LxtCheckErrno(tcflow(PtsFd, TCION));

    //
    // Ubuntu16 asynchronous pty processing needs some time to process start.
    //

    sleep(1);

    //
    // Master endpoint should be ready for write.
    //

    FD_SET(PtmFd, &WriteFds);
    LxtCheckErrno(select((PtmFd + 1), NULL, &WriteFds, NULL, &Timeout));
    LxtCheckEqual(Result, 1, "%d");

    //
    // Check for "resume" character on master.
    //

    LxtLogInfo("Checking for message on master");
    LxtCheckErrno(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)));
    LxtCheckFnResults("read", BytesReadWrite, 1);
    LxtCheckEqual(ReadBuffer[0], ControlArray[VSTART], "%hhd");

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

int PtSuspendOutput8(PLXT_ARGS Args)

/*++

Routine Description:

    This routine checks TCOFF/ON when IXON is disabled.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int BytesReadWrite;
    int ExpectedResult;
    int FileFlags;
    const char* Greetings = "Hi there!!";
    tcflag_t InputFlags;
    int PtmFd;
    int PtsFd;
    char ReadBuffer[20];
    int Result;
    int SerialNumber;
    struct timeval Timeout;
    fd_set WriteFds;

    //
    // Initialize locals.
    //

    PtmFd = -1;
    PtsFd = -1;

    //
    // Open Master-Subordinate.
    //

    LxtCheckErrno(OpenMasterSubordinate(&PtmFd, &PtsFd, NULL, &SerialNumber));
    LxtLogInfo("Master opened at FD:%d", PtmFd);
    LxtLogInfo("Subordinate Serial Number: %d", SerialNumber);
    LxtLogInfo("Subordinate opened at FD:%d", PtsFd);

    //
    // Turn off IXON to disable START/STOP characters.
    //

    LxtCheckResult(TerminalSettingsGetInputFlags(PtsFd, &InputFlags));
    LxtCheckResult(TerminalSettingsSetInputFlags(PtsFd, InputFlags & ~IXON));

    //
    // subordinate endpoint should be ready for write.
    //

    memset(&Timeout, 0, sizeof(Timeout));
    FD_ZERO(&WriteFds);
    FD_SET(PtsFd, &WriteFds);
    LxtCheckErrno(select((PtsFd + 1), NULL, &WriteFds, NULL, &Timeout));
    LxtCheckEqual(Result, 1, "%d");

    //
    // Suspend output on subordinate.
    //

    LxtCheckErrno(tcflow(PtsFd, TCOOFF));

    //
    // Subordinate endpoint should not be ready for write.
    //

    FD_SET(PtsFd, &WriteFds);
    LxtCheckErrno(select((PtsFd + 1), NULL, &WriteFds, NULL, &Timeout));
    LxtCheckEqual(Result, 0, "%d");

    //
    // Try to write to subordinate.
    //

    LxtLogInfo("Attempt to write to suspended subordinate...");
    ExpectedResult = strlen(Greetings);
    LxtCheckErrno(FileFlags = fcntl(PtsFd, F_GETFL, 0));
    LxtCheckErrno(fcntl(PtsFd, F_SETFL, (FileFlags | O_NONBLOCK)));
    LxtCheckErrnoFailure(BytesReadWrite = write(PtsFd, Greetings, ExpectedResult), EAGAIN);
    LxtCheckErrno(fcntl(PtsFd, F_SETFL, FileFlags));

    //
    // Restart output on subordinate.
    //

    LxtCheckErrno(tcflow(PtsFd, TCOON));

    //
    // Subordinate endpoint should be ready for write.
    //

    FD_SET(PtsFd, &WriteFds);
    LxtCheckErrno(select((PtsFd + 1), NULL, &WriteFds, NULL, &Timeout));
    LxtCheckEqual(Result, 1, "%d");

    //
    // Try to write to subordinate.
    //

    LxtLogInfo("Write to subordinate...");
    ExpectedResult = strlen(Greetings);
    LxtCheckErrno(BytesReadWrite = write(PtsFd, Greetings, ExpectedResult));
    LxtCheckFnResults("write", BytesReadWrite, ExpectedResult);

    //
    // Check for message on master.
    //

    LxtLogInfo("Checking for message on master");
    memset(ReadBuffer, 0, sizeof(ReadBuffer));
    LxtCheckErrno(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)));
    LxtCheckFnResults("read", BytesReadWrite, ExpectedResult);
    ReadBuffer[BytesReadWrite] = '\0';
    LxtCheckStringEqual(ReadBuffer, Greetings);

    //
    // No output expected for subordinate.
    //

    LxtCheckErrno(FileFlags = fcntl(PtsFd, F_GETFL, 0));
    LxtCheckErrno(fcntl(PtsFd, F_SETFL, (FileFlags | O_NONBLOCK)));
    LxtCheckErrnoFailure(BytesReadWrite = read(PtsFd, ReadBuffer, sizeof(ReadBuffer)), EAGAIN);
    LxtCheckErrno(fcntl(PtsFd, F_SETFL, FileFlags));

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

int PtSuspendOutput9(PLXT_ARGS Args)

/*++

Routine Description:

    This routine checks echo support while output is suspended on the
    subordinate endpoint.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int BytesReadWrite;
    int ExpectedResult;
    int FileFlags;
    const char* Greetings = "Hi there!!\n";
    const char* GreetingsEcho = "Hi there!!\r\n";
    int PtmFd;
    int PtsFd;
    char ReadBuffer[20];
    int Result;
    int SerialNumber;

    //
    // Initialize locals.
    //

    PtmFd = -1;
    PtsFd = -1;

    //
    // Open Master-Subordinate.
    //

    LxtCheckErrno(OpenMasterSubordinate(&PtmFd, &PtsFd, NULL, &SerialNumber));
    LxtLogInfo("Master opened at FD:%d", PtmFd);
    LxtLogInfo("Subordinate Serial Number: %d", SerialNumber);
    LxtLogInfo("Subordinate opened at FD:%d", PtsFd);

    //
    // Suspend output on subordinate.
    //

    LxtCheckErrno(tcflow(PtsFd, TCOOFF));

    //
    // Write to master.
    //

    LxtLogInfo("Write to master...");
    ExpectedResult = strlen(Greetings);
    LxtCheckErrno(BytesReadWrite = write(PtmFd, Greetings, ExpectedResult));
    LxtCheckFnResults("write", BytesReadWrite, ExpectedResult);
    LxtCheckErrno(tcdrain(PtmFd));

    //
    // Ubuntu16 asynchronous pty processing needs some time to process message.
    //

    sleep(1);

    //
    // No echo expected to master.
    //

    LxtCheckErrno(FileFlags = fcntl(PtmFd, F_GETFL, 0));
    LxtCheckErrno(fcntl(PtmFd, F_SETFL, (FileFlags | O_NONBLOCK)));
    LxtCheckErrnoFailure(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)), EAGAIN);
    LxtCheckErrno(fcntl(PtmFd, F_SETFL, FileFlags));

    //
    // Resume output on subordinate.
    //

    LxtCheckErrno(tcflow(PtsFd, TCOON));

    //
    // Ubuntu16 asynchronous pty processing needs some time to process state
    // change.
    //

    sleep(1);

    //
    // Echo characters expected to have been discarded.
    //

    LxtCheckErrno(FileFlags = fcntl(PtmFd, F_GETFL, 0));
    LxtCheckErrno(fcntl(PtmFd, F_SETFL, (FileFlags | O_NONBLOCK)));
    LxtCheckErrnoFailure(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)), EAGAIN);
    LxtCheckErrno(fcntl(PtmFd, F_SETFL, FileFlags));

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

int PtSuspendOutput10(PLXT_ARGS Args)

/*++

Routine Description:

    This routine checks echo support while output is suspended on the
    master endpoint.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int BytesReadWrite;
    int ExpectedResult;
    int FileFlags;
    const char* Greetings = "Hi there!!\n";
    const char* GreetingsEcho = "Hi there!!\r\n";
    int PtmFd;
    int PtsFd;
    char ReadBuffer[20];
    int Result;
    int SerialNumber;

    //
    // Initialize locals.
    //

    PtmFd = -1;
    PtsFd = -1;

    //
    // Open Master-Subordinate.
    //

    LxtCheckErrno(OpenMasterSubordinate(&PtmFd, &PtsFd, NULL, &SerialNumber));
    LxtLogInfo("Master opened at FD:%d", PtmFd);
    LxtLogInfo("Subordinate Serial Number: %d", SerialNumber);
    LxtLogInfo("Subordinate opened at FD:%d", PtsFd);

    //
    // Write to master.
    //

    LxtLogInfo("Write to master...");
    ExpectedResult = strlen(Greetings);
    LxtCheckErrno(BytesReadWrite = write(PtmFd, Greetings, ExpectedResult));
    LxtCheckFnResults("write", BytesReadWrite, ExpectedResult);
    LxtCheckErrno(tcdrain(PtmFd));

    //
    // Suspend output on master.
    //

    LxtCheckErrno(tcflow(PtmFd, TCOOFF));

    //
    // Ubuntu16 asynchronous pty processing needs some time to process message.
    //

    sleep(1);

    //
    // Check for echo on master.
    //

    LxtLogInfo("Checking for echo on master");
    ExpectedResult = strlen(GreetingsEcho);
    memset(ReadBuffer, 0, sizeof(ReadBuffer));
    LxtCheckErrno(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)));
    LxtCheckFnResults("read", BytesReadWrite, ExpectedResult);
    ReadBuffer[BytesReadWrite] = '\0';
    LxtCheckStringEqual(ReadBuffer, GreetingsEcho);

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

int PtSuspendOutput11(PLXT_ARGS Args)

/*++

Routine Description:

    This routine checks suspend output support on the subordinate endpoint via
    TCI(OFF/ON) from the master when the master has its own output suspended.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int BytesReadWrite;
    int ExpectedResult;
    int FileFlags;
    const char* Greetings = "Hi there!!\n";
    const char* GreetingsOut = "Hi there!!\r\n";
    int PtmFd;
    int PtsFd;
    char ReadBuffer[20];
    int Result;
    int SerialNumber;
    struct timeval Timeout;
    fd_set WriteFds;

    //
    // Initialize locals.
    //

    PtmFd = -1;
    PtsFd = -1;

    //
    // Open Master-Subordinate.
    //

    LxtCheckErrno(OpenMasterSubordinate(&PtmFd, &PtsFd, NULL, &SerialNumber));
    LxtLogInfo("Master opened at FD:%d", PtmFd);
    LxtLogInfo("Subordinate Serial Number: %d", SerialNumber);
    LxtLogInfo("Subordinate opened at FD:%d", PtsFd);

    //
    // Suspend output on master.
    //

    LxtCheckErrno(tcflow(PtmFd, TCOOFF));

    //
    // Ubuntu16 asynchronous pty processing needs some time to process stop.
    //

    sleep(1);

    //
    // Try to suspend output on subordinate.
    //

    LxtCheckErrno(tcflow(PtmFd, TCIOFF));

    //
    // Ubuntu16 asynchronous pty processing needs some time to process stop.
    //

    sleep(1);

    //
    // No echo expected to master as control character shouldn't have been
    // transmitted.
    //

    LxtCheckErrno(FileFlags = fcntl(PtmFd, F_GETFL, 0));
    LxtCheckErrno(fcntl(PtmFd, F_SETFL, (FileFlags | O_NONBLOCK)));
    LxtCheckErrnoFailure(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)), EAGAIN);
    LxtCheckErrno(fcntl(PtmFd, F_SETFL, FileFlags));

    //
    // Subordinate endpoint should be ready for write.
    //

    memset(&Timeout, 0, sizeof(Timeout));
    FD_ZERO(&WriteFds);
    FD_SET(PtsFd, &WriteFds);
    LxtCheckErrno(select((PtsFd + 1), NULL, &WriteFds, NULL, &Timeout));
    LxtCheckEqual(Result, 1, "%d");

    //
    // Try to write to subordinate.
    //

    LxtLogInfo("Write to subordinate...");
    ExpectedResult = strlen(Greetings);
    LxtCheckErrno(BytesReadWrite = write(PtsFd, Greetings, ExpectedResult));
    LxtCheckFnResults("write", BytesReadWrite, ExpectedResult);

    //
    // Check for message on master.
    //

    LxtLogInfo("Checking for message on master");
    ExpectedResult = strlen(GreetingsOut);
    memset(ReadBuffer, 0, sizeof(ReadBuffer));
    LxtCheckErrno(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)));
    LxtCheckFnResults("read", BytesReadWrite, ExpectedResult);
    ReadBuffer[BytesReadWrite] = '\0';
    LxtCheckStringEqual(ReadBuffer, GreetingsOut);

    //
    // Restart output on master.
    //

    LxtCheckErrno(tcflow(PtmFd, TCOON));

    //
    // Ubuntu16 asynchronous pty processing needs some time to process start.
    //

    sleep(1);

    //
    // No echo expected to master as control character should have been
    // discarded.
    //

    LxtCheckErrno(FileFlags = fcntl(PtmFd, F_GETFL, 0));
    LxtCheckErrno(fcntl(PtmFd, F_SETFL, (FileFlags | O_NONBLOCK)));
    LxtCheckErrnoFailure(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)), EAGAIN);
    LxtCheckErrno(fcntl(PtmFd, F_SETFL, FileFlags));

    //
    // Subordinate endpoint should be ready for write.
    //

    FD_SET(PtsFd, &WriteFds);
    LxtCheckErrno(select((PtsFd + 1), NULL, &WriteFds, NULL, &Timeout));
    LxtCheckEqual(Result, 1, "%d");

    //
    // Try to write to subordinate.
    //

    LxtLogInfo("Write to subordinate...");
    ExpectedResult = strlen(Greetings);
    LxtCheckErrno(BytesReadWrite = write(PtsFd, Greetings, ExpectedResult));
    LxtCheckFnResults("write", BytesReadWrite, ExpectedResult);

    //
    // Check for message on master.
    //

    LxtLogInfo("Checking for message on master");
    ExpectedResult = strlen(GreetingsOut);
    memset(ReadBuffer, 0, sizeof(ReadBuffer));
    LxtCheckErrno(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)));
    LxtCheckFnResults("read", BytesReadWrite, ExpectedResult);
    ReadBuffer[BytesReadWrite] = '\0';
    LxtCheckStringEqual(ReadBuffer, GreetingsOut);

    //
    // No output expected for subordinate.
    //

    LxtCheckErrno(FileFlags = fcntl(PtsFd, F_GETFL, 0));
    LxtCheckErrno(fcntl(PtsFd, F_SETFL, (FileFlags | O_NONBLOCK)));
    LxtCheckErrnoFailure(BytesReadWrite = read(PtsFd, ReadBuffer, sizeof(ReadBuffer)), EAGAIN);
    LxtCheckErrno(fcntl(PtsFd, F_SETFL, FileFlags));

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

int PtSuspendOutput12(PLXT_ARGS Args)

/*++

Routine Description:

    This routine checks suspend output support on the subordinate with IXANY
    using the control character (^S).

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int BytesReadWrite;
    int ExpectedResult;
    int FileFlags;
    const char* Greetings = "Hi there!!\n";
    const char* GreetingsOut = "Hi there!!\r\n";
    tcflag_t InputFlags;
    int PtmFd;
    int PtsFd;
    char ReadBuffer[20];
    int Result;
    int SerialNumber;
    struct timeval Timeout;
    fd_set WriteFds;

    //
    // Initialize locals.
    //

    PtmFd = -1;
    PtsFd = -1;

    //
    // Open Master-Subordinate.
    //

    LxtCheckErrno(OpenMasterSubordinate(&PtmFd, &PtsFd, NULL, &SerialNumber));
    LxtLogInfo("Master opened at FD:%d", PtmFd);
    LxtLogInfo("Subordinate Serial Number: %d", SerialNumber);
    LxtLogInfo("Subordinate opened at FD:%d", PtsFd);

    //
    // Enable IXANY flag.
    //

    LxtCheckResult(TerminalSettingsGetInputFlags(PtsFd, &InputFlags));
    LxtCheckResult(TerminalSettingsSetInputFlags(PtsFd, InputFlags | IXANY));

    //
    // Suspend output on subordinate with control character.
    //

    LxtCheckErrno(tcflow(PtmFd, TCIOFF));

    //
    // Ubuntu16 asynchronous pty processing needs some time to process stop.
    //

    sleep(1);

    //
    // Subordinate endpoint should not be ready for write.
    //

    memset(&Timeout, 0, sizeof(Timeout));
    FD_ZERO(&WriteFds);
    FD_SET(PtsFd, &WriteFds);
    LxtCheckErrno(select((PtsFd + 1), NULL, &WriteFds, NULL, &Timeout));
    LxtCheckEqual(Result, 0, "%d");

    //
    // Try to write to subordinate.
    //

    LxtLogInfo("Attempt to write to suspended subordinate...");
    ExpectedResult = strlen(Greetings);
    LxtCheckErrno(FileFlags = fcntl(PtsFd, F_GETFL, 0));
    LxtCheckErrno(fcntl(PtsFd, F_SETFL, (FileFlags | O_NONBLOCK)));
    LxtCheckErrnoFailure(BytesReadWrite = write(PtsFd, Greetings, ExpectedResult), EAGAIN);
    LxtCheckErrno(fcntl(PtsFd, F_SETFL, FileFlags));

    //
    // Write to master.
    //

    LxtLogInfo("Write to master...");
    ExpectedResult = strlen(Greetings);
    LxtCheckErrno(BytesReadWrite = write(PtmFd, Greetings, ExpectedResult));
    LxtCheckFnResults("write", BytesReadWrite, ExpectedResult);
    LxtCheckErrno(tcdrain(PtmFd));

    //
    // Check for message on subordinate.
    //

    LxtLogInfo("Checking for message on subordinate.");
    memset(ReadBuffer, 0, sizeof(ReadBuffer));
    LxtCheckErrno(BytesReadWrite = read(PtsFd, ReadBuffer, sizeof(ReadBuffer)));
    LxtCheckFnResults("read", BytesReadWrite, ExpectedResult);
    ReadBuffer[BytesReadWrite] = '\0';
    LxtCheckStringEqual(ReadBuffer, Greetings);

    //
    // Check for echo on master.
    //

    LxtLogInfo("Checking for message on master.");
    ExpectedResult = strlen(GreetingsOut);
    memset(ReadBuffer, 0, sizeof(ReadBuffer));
    LxtCheckErrno(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)));
    LxtCheckFnResults("read", BytesReadWrite, ExpectedResult);
    ReadBuffer[BytesReadWrite] = '\0';
    LxtCheckStringEqual(ReadBuffer, GreetingsOut);

    //
    // Subordinate endpoint should have resumed output.
    //

    memset(&Timeout, 0, sizeof(Timeout));
    FD_ZERO(&WriteFds);
    FD_SET(PtsFd, &WriteFds);
    LxtCheckErrno(select((PtsFd + 1), NULL, &WriteFds, NULL, &Timeout));
    LxtCheckEqual(Result, 1, "%d");

    //
    // Write to subordinate.
    //

    LxtLogInfo("Write to subordinate...");
    ExpectedResult = strlen(Greetings);
    LxtCheckErrno(BytesReadWrite = write(PtsFd, Greetings, ExpectedResult));
    LxtCheckFnResults("write", BytesReadWrite, ExpectedResult);
    LxtCheckErrno(tcdrain(PtsFd));

    //
    // Check for message on master.
    //

    LxtLogInfo("Checking for message on master.");
    ExpectedResult = strlen(GreetingsOut);
    memset(ReadBuffer, 0, sizeof(ReadBuffer));
    LxtCheckErrno(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)));
    LxtCheckFnResults("read", BytesReadWrite, ExpectedResult);
    ReadBuffer[BytesReadWrite] = '\0';
    LxtCheckStringEqual(ReadBuffer, GreetingsOut);

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

int PtSuspendOutput13(PLXT_ARGS Args)

/*++

Routine Description:

    This routine checks suspend output support on the subordinate with IXANY
    using TCOOFF which should not be effected.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int BytesReadWrite;
    int ExpectedResult;
    int FileFlags;
    const char* Greetings = "Hi there!!\n";
    const char* GreetingsOut = "Hi there!!\r\n";
    tcflag_t InputFlags;
    int PtmFd;
    int PtsFd;
    char ReadBuffer[20];
    int Result;
    int SerialNumber;
    struct timeval Timeout;
    fd_set WriteFds;

    //
    // Initialize locals.
    //

    PtmFd = -1;
    PtsFd = -1;

    //
    // Open Master-Subordinate.
    //

    LxtCheckErrno(OpenMasterSubordinate(&PtmFd, &PtsFd, NULL, &SerialNumber));
    LxtLogInfo("Master opened at FD:%d", PtmFd);
    LxtLogInfo("Subordinate Serial Number: %d", SerialNumber);
    LxtLogInfo("Subordinate opened at FD:%d", PtsFd);

    //
    // Enable IXANY flag.
    //

    LxtCheckResult(TerminalSettingsGetInputFlags(PtmFd, &InputFlags));
    LxtCheckResult(TerminalSettingsSetInputFlags(PtmFd, InputFlags | IXANY));

    //
    // Suspend output on subordinate.
    //

    LxtCheckErrno(tcflow(PtsFd, TCOOFF));

    //
    // Ubuntu16 asynchronous pty processing needs some time to process stop.
    //

    sleep(1);

    //
    // Subordinate endpoint should not be ready for write.
    //

    memset(&Timeout, 0, sizeof(Timeout));
    FD_ZERO(&WriteFds);
    FD_SET(PtsFd, &WriteFds);
    LxtCheckErrno(select((PtsFd + 1), NULL, &WriteFds, NULL, &Timeout));
    LxtCheckEqual(Result, 0, "%d");

    //
    // Try to write to subordinate.
    //

    LxtLogInfo("Attempt to write to suspended subordinate...");
    ExpectedResult = strlen(Greetings);
    LxtCheckErrno(FileFlags = fcntl(PtsFd, F_GETFL, 0));
    LxtCheckErrno(fcntl(PtsFd, F_SETFL, (FileFlags | O_NONBLOCK)));
    LxtCheckErrnoFailure(BytesReadWrite = write(PtsFd, Greetings, ExpectedResult), EAGAIN);
    LxtCheckErrno(fcntl(PtsFd, F_SETFL, FileFlags));

    //
    // Write to master.
    //

    LxtLogInfo("Write to master...");
    ExpectedResult = strlen(Greetings);
    LxtCheckErrno(BytesReadWrite = write(PtmFd, Greetings, ExpectedResult));
    LxtCheckFnResults("write", BytesReadWrite, ExpectedResult);

    //
    // Check for message on subordinate.
    //

    LxtLogInfo("Checking for message on subordinate");
    memset(ReadBuffer, 0, sizeof(ReadBuffer));
    LxtCheckErrno(BytesReadWrite = read(PtsFd, ReadBuffer, sizeof(ReadBuffer)));
    LxtCheckFnResults("read", BytesReadWrite, ExpectedResult);
    ReadBuffer[BytesReadWrite] = '\0';
    LxtCheckStringEqual(ReadBuffer, Greetings);

    //
    // Subordinate endpoint should still not be ready for write.
    //

    memset(&Timeout, 0, sizeof(Timeout));
    FD_ZERO(&WriteFds);
    FD_SET(PtsFd, &WriteFds);
    LxtCheckErrno(select((PtsFd + 1), NULL, &WriteFds, NULL, &Timeout));
    LxtCheckEqual(Result, 0, "%d");

    //
    // Try to write to subordinate.
    //

    LxtLogInfo("Attempt to write to suspended subordinate...");
    ExpectedResult = strlen(Greetings);
    LxtCheckErrno(FileFlags = fcntl(PtsFd, F_GETFL, 0));
    LxtCheckErrno(fcntl(PtsFd, F_SETFL, (FileFlags | O_NONBLOCK)));
    LxtCheckErrnoFailure(BytesReadWrite = write(PtsFd, Greetings, ExpectedResult), EAGAIN);
    LxtCheckErrno(fcntl(PtsFd, F_SETFL, FileFlags));

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

int PtSuspendOutput14(PLXT_ARGS Args)

/*++

Routine Description:

    This routine checks suspend output on the master when the subordinate
    disconnects.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int BytesReadWrite;
    int ExpectedResult;
    int FileFlags;
    const char* Greetings = "Hi there!!\n";
    const char* GreetingsOut = "Hi there!!\r\n";
    tcflag_t InputFlags;
    int PtmFd;
    int PtsFd;
    char PtsDevName[PTS_DEV_NAME_BUFFER_SIZE];
    char ReadBuffer[20];
    int Result;
    int SerialNumber;
    struct timeval Timeout;
    fd_set WriteFds;

    //
    // Initialize locals.
    //

    PtmFd = -1;
    PtsFd = -1;

    //
    // Open Master-Subordinate.
    //

    LxtCheckErrno(OpenMasterSubordinate(&PtmFd, &PtsFd, PtsDevName, &SerialNumber));
    LxtLogInfo("Master opened at FD:%d", PtmFd);
    LxtLogInfo("Subordinate Serial Number: %d", SerialNumber);
    LxtLogInfo("Subordinate opened at FD:%d", PtsFd);

    //
    // Suspend output on master.
    //

    LxtCheckErrno(tcflow(PtmFd, TCOOFF));

    //
    // Ubuntu16 asynchronous pty processing needs some time to process stop.
    //

    sleep(1);

    //
    // Master endpoint should not be ready for write.
    //

    memset(&Timeout, 0, sizeof(Timeout));
    FD_ZERO(&WriteFds);
    FD_SET(PtmFd, &WriteFds);
    LxtCheckErrno(select((PtmFd + 1), NULL, &WriteFds, NULL, &Timeout));
    LxtCheckEqual(Result, 0, "%d");

    //
    // Try to write to master.
    //

    LxtLogInfo("Attempt to write to suspended master...");
    ExpectedResult = strlen(Greetings);
    LxtCheckErrno(FileFlags = fcntl(PtmFd, F_GETFL, 0));
    LxtCheckErrno(fcntl(PtmFd, F_SETFL, (FileFlags | O_NONBLOCK)));
    LxtCheckErrnoFailure(BytesReadWrite = write(PtmFd, Greetings, ExpectedResult), EAGAIN);
    LxtCheckErrno(fcntl(PtmFd, F_SETFL, FileFlags));

    //
    // Close the subordinate.
    //

    LxtClose(PtsFd);

    //
    // Master endpoint should not be ready for write.
    //

    memset(&Timeout, 0, sizeof(Timeout));
    FD_ZERO(&WriteFds);
    FD_SET(PtmFd, &WriteFds);
    LxtCheckErrno(select((PtmFd + 1), NULL, &WriteFds, NULL, &Timeout));
    LxtCheckEqual(Result, 0, "%d");

    //
    // Try to write to master.
    //

    LxtLogInfo("Attempt to write to suspended master...");
    ExpectedResult = strlen(Greetings);
    LxtCheckErrno(FileFlags = fcntl(PtmFd, F_GETFL, 0));
    LxtCheckErrno(fcntl(PtmFd, F_SETFL, (FileFlags | O_NONBLOCK)));
    LxtCheckErrnoFailure(BytesReadWrite = write(PtmFd, Greetings, ExpectedResult), EAGAIN);
    LxtCheckErrno(fcntl(PtmFd, F_SETFL, FileFlags));

    //
    // Reopen the subordinate.
    //

    LxtCheckErrno(PtsFd = open(PtsDevName, O_RDWR));
    LxtLogInfo("Subordinate opened at FD:%d", PtsFd);

    //
    // Master endpoint should not be ready for write.
    //

    memset(&Timeout, 0, sizeof(Timeout));
    FD_ZERO(&WriteFds);
    FD_SET(PtmFd, &WriteFds);
    LxtCheckErrno(select((PtmFd + 1), NULL, &WriteFds, NULL, &Timeout));
    LxtCheckEqual(Result, 0, "%d");

    //
    // Try to write to master.
    //

    LxtLogInfo("Attempt to write to suspended master...");
    ExpectedResult = strlen(Greetings);
    LxtCheckErrno(FileFlags = fcntl(PtmFd, F_GETFL, 0));
    LxtCheckErrno(fcntl(PtmFd, F_SETFL, (FileFlags | O_NONBLOCK)));
    LxtCheckErrnoFailure(BytesReadWrite = write(PtmFd, Greetings, ExpectedResult), EAGAIN);
    LxtCheckErrno(fcntl(PtmFd, F_SETFL, FileFlags));

    //
    // Write to subordinate.
    //

    LxtLogInfo("Write to subordinate...");
    ExpectedResult = strlen(Greetings);
    LxtCheckErrno(BytesReadWrite = write(PtsFd, Greetings, ExpectedResult));
    LxtCheckFnResults("write", BytesReadWrite, ExpectedResult);

    //
    // Check for message on master.
    //

    LxtLogInfo("Checking for message on master");
    ExpectedResult = strlen(GreetingsOut);
    memset(ReadBuffer, 0, sizeof(ReadBuffer));
    LxtCheckErrno(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)));
    LxtCheckFnResults("read", BytesReadWrite, ExpectedResult);
    ReadBuffer[BytesReadWrite] = '\0';
    LxtCheckStringEqual(ReadBuffer, GreetingsOut);

    //
    // Master endpoint should not be ready for write.
    //

    memset(&Timeout, 0, sizeof(Timeout));
    FD_ZERO(&WriteFds);
    FD_SET(PtmFd, &WriteFds);
    LxtCheckErrno(select((PtmFd + 1), NULL, &WriteFds, NULL, &Timeout));
    LxtCheckEqual(Result, 0, "%d");

    //
    // Try to write to master.
    //

    LxtLogInfo("Attempt to write to suspended master...");
    ExpectedResult = strlen(Greetings);
    LxtCheckErrno(FileFlags = fcntl(PtmFd, F_GETFL, 0));
    LxtCheckErrno(fcntl(PtmFd, F_SETFL, (FileFlags | O_NONBLOCK)));
    LxtCheckErrnoFailure(BytesReadWrite = write(PtmFd, Greetings, ExpectedResult), EAGAIN);
    LxtCheckErrno(fcntl(PtmFd, F_SETFL, FileFlags));

    //
    // Resume output on master.
    //

    LxtCheckErrno(tcflow(PtmFd, TCOON));

    //
    // Write to master.
    //

    LxtLogInfo("Write to master...");
    ExpectedResult = strlen(Greetings);
    LxtCheckErrno(BytesReadWrite = write(PtmFd, Greetings, ExpectedResult));
    LxtCheckFnResults("write", BytesReadWrite, ExpectedResult);

    //
    // Check for message on subordinate.
    //

    LxtLogInfo("Checking for message on master");
    memset(ReadBuffer, 0, sizeof(ReadBuffer));
    LxtCheckErrno(BytesReadWrite = read(PtsFd, ReadBuffer, sizeof(ReadBuffer)));
    LxtCheckFnResults("read", BytesReadWrite, ExpectedResult);
    ReadBuffer[BytesReadWrite] = '\0';
    LxtCheckStringEqual(ReadBuffer, Greetings);

    //
    // Check for echo on master.
    //

    LxtLogInfo("Checking for echo on master");
    ExpectedResult = strlen(GreetingsOut);
    memset(ReadBuffer, 0, sizeof(ReadBuffer));
    LxtCheckErrno(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)));
    LxtCheckFnResults("read", BytesReadWrite, ExpectedResult);
    ReadBuffer[BytesReadWrite] = '\0';
    LxtCheckStringEqual(ReadBuffer, GreetingsOut);

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

int PtSuspendOutput15(PLXT_ARGS Args)

/*++

Routine Description:

    This routine checks suspend output support using combinations of
    control-characters and TCIOFF/ON.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int BytesReadWrite;
    cc_t ControlArray[NCCS];
    int ExpectedResult;
    int FileFlags;
    const char* Greetings = "Hi there!!";
    int PtmFd;
    int PtsFd;
    char ReadBuffer[20];
    int Result;
    int SerialNumber;
    struct timeval Timeout;
    fd_set WriteFds;

    //
    // Initialize locals.
    //

    PtmFd = -1;
    PtsFd = -1;

    //
    // Open Master-Subordinate.
    //

    LxtCheckErrno(OpenMasterSubordinate(&PtmFd, &PtsFd, NULL, &SerialNumber));
    LxtLogInfo("Master opened at FD:%d", PtmFd);
    LxtLogInfo("Subordinate Serial Number: %d", SerialNumber);
    LxtLogInfo("Subordinate opened at FD:%d", PtsFd);

    //
    // Get control characters.
    //

    LxtCheckResult(TerminalSettingsGetControlArray(PtmFd, ControlArray));

    //
    // subordinate endpoint should be ready for write.
    //

    memset(&Timeout, 0, sizeof(Timeout));
    FD_ZERO(&WriteFds);
    FD_SET(PtsFd, &WriteFds);
    LxtCheckErrno(select((PtsFd + 1), NULL, &WriteFds, NULL, &Timeout));
    LxtCheckEqual(Result, 1, "%d");

    //
    // Suspend output on subordinate.
    //

    LxtCheckErrno(BytesReadWrite = write(PtmFd, &ControlArray[VSTOP], 1));
    LxtCheckFnResults("write", BytesReadWrite, 1);
    LxtCheckErrno(tcdrain(PtmFd));

    //
    // Ubuntu16 asynchronous pty processing needs some time to process stop.
    //

    sleep(1);

    //
    // subordinate endpoint should not be ready for write.
    //

    FD_SET(PtsFd, &WriteFds);
    LxtCheckErrno(select((PtsFd + 1), NULL, &WriteFds, NULL, &Timeout));
    LxtCheckEqual(Result, 0, "%d");

    //
    // Try to write to subordinate.
    //

    LxtLogInfo("Attempt to write to suspended subordinate...");
    ExpectedResult = strlen(Greetings);
    LxtCheckErrno(FileFlags = fcntl(PtsFd, F_GETFL, 0));
    LxtCheckErrno(fcntl(PtsFd, F_SETFL, (FileFlags | O_NONBLOCK)));
    LxtCheckErrnoFailure(BytesReadWrite = write(PtsFd, Greetings, ExpectedResult), EAGAIN);
    LxtCheckErrno(fcntl(PtsFd, F_SETFL, FileFlags));

    //
    // Use TCION to try to resume output.
    //

    LxtCheckErrno(tcflow(PtsFd, TCOON));

    //
    // Ubuntu16 asynchronous pty processing needs some time to process stop.
    //

    sleep(1);

    //
    // subordinate endpoint should not be ready for write.
    //

    FD_SET(PtsFd, &WriteFds);
    LxtCheckErrno(select((PtsFd + 1), NULL, &WriteFds, NULL, &Timeout));
    LxtCheckEqual(Result, 0, "%d");

    //
    // Try to write to subordinate.
    //

    LxtLogInfo("Attempt to write to suspended subordinate...");
    ExpectedResult = strlen(Greetings);
    LxtCheckErrno(FileFlags = fcntl(PtsFd, F_GETFL, 0));
    LxtCheckErrno(fcntl(PtsFd, F_SETFL, (FileFlags | O_NONBLOCK)));
    LxtCheckErrnoFailure(BytesReadWrite = write(PtsFd, Greetings, ExpectedResult), EAGAIN);
    LxtCheckErrno(fcntl(PtsFd, F_SETFL, FileFlags));

    //
    // Use TCIOFF to suspend output on already CTRL-S suspended terminal.
    //

    LxtCheckErrno(tcflow(PtsFd, TCOOFF));

    //
    // Ubuntu16 asynchronous pty processing needs some time to process stop.
    //

    sleep(1);

    //
    // subordinate endpoint should not be ready for write.
    //

    FD_SET(PtsFd, &WriteFds);
    LxtCheckErrno(select((PtsFd + 1), NULL, &WriteFds, NULL, &Timeout));
    LxtCheckEqual(Result, 0, "%d");

    //
    // Try to write to subordinate.
    //

    LxtLogInfo("Attempt to write to suspended subordinate...");
    ExpectedResult = strlen(Greetings);
    LxtCheckErrno(FileFlags = fcntl(PtsFd, F_GETFL, 0));
    LxtCheckErrno(fcntl(PtsFd, F_SETFL, (FileFlags | O_NONBLOCK)));
    LxtCheckErrnoFailure(BytesReadWrite = write(PtsFd, Greetings, ExpectedResult), EAGAIN);
    LxtCheckErrno(fcntl(PtsFd, F_SETFL, FileFlags));

    //
    // Restart output on subordinate with control character.
    //

    LxtCheckErrno(BytesReadWrite = write(PtmFd, &ControlArray[VSTART], 1));
    LxtCheckFnResults("write", BytesReadWrite, 1);
    LxtCheckErrno(tcdrain(PtmFd));

    //
    // Ubuntu16 asynchronous pty processing needs some time to process start.
    //

    sleep(1);

    //
    // subordinate endpoint should not be ready for write.
    //

    FD_SET(PtsFd, &WriteFds);
    LxtCheckErrno(select((PtsFd + 1), NULL, &WriteFds, NULL, &Timeout));
    LxtCheckEqual(Result, 0, "%d");

    //
    // Try to write to subordinate.
    //

    LxtLogInfo("Attempt to write to suspended subordinate...");
    ExpectedResult = strlen(Greetings);
    LxtCheckErrno(FileFlags = fcntl(PtsFd, F_GETFL, 0));
    LxtCheckErrno(fcntl(PtsFd, F_SETFL, (FileFlags | O_NONBLOCK)));
    LxtCheckErrnoFailure(BytesReadWrite = write(PtsFd, Greetings, ExpectedResult), EAGAIN);
    LxtCheckErrno(fcntl(PtsFd, F_SETFL, FileFlags));

    //
    // Use TCOON to resume output.
    //

    LxtCheckErrno(tcflow(PtsFd, TCOON));

    //
    // Ubuntu16 asynchronous pty processing needs some time to process stop.
    //

    sleep(1);

    //
    // subordinate endpoint should be ready for write.
    //

    FD_SET(PtsFd, &WriteFds);
    LxtCheckErrno(select((PtsFd + 1), NULL, &WriteFds, NULL, &Timeout));
    LxtCheckEqual(Result, 1, "%d");

    //
    // Try to write to subordinate.
    //

    LxtLogInfo("Write to subordinate...");
    ExpectedResult = strlen(Greetings);
    LxtCheckErrno(BytesReadWrite = write(PtsFd, Greetings, ExpectedResult));
    LxtCheckFnResults("write", BytesReadWrite, ExpectedResult);

    //
    // Check for message on master.
    //

    LxtLogInfo("Checking for message on master");
    memset(ReadBuffer, 0, sizeof(ReadBuffer));
    LxtCheckErrno(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)));
    LxtCheckFnResults("read", BytesReadWrite, ExpectedResult);
    ReadBuffer[BytesReadWrite] = '\0';
    LxtCheckStringEqual(ReadBuffer, Greetings);

    //
    // No output expected for subordinate.
    //

    LxtCheckErrno(FileFlags = fcntl(PtsFd, F_GETFL, 0));
    LxtCheckErrno(fcntl(PtsFd, F_SETFL, (FileFlags | O_NONBLOCK)));
    LxtCheckErrnoFailure(BytesReadWrite = read(PtsFd, ReadBuffer, sizeof(ReadBuffer)), EAGAIN);
    LxtCheckErrno(fcntl(PtsFd, F_SETFL, FileFlags));

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

int PtSuspendOutput16(PLXT_ARGS Args)

/*++

Routine Description:

    This routine tries to write output to a suspended endpoint.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int BytesReadWrite;
    int ChildPid;
    int ExpectedResult;
    const char* Greetings = "Hi there!!\n";
    int PtmFd;
    int PtmFlags;
    int PtsFd;
    char ReadBuffer[20];
    fd_set ReadFds;
    int Result;
    int SerialNumber;
    int Status;
    struct timeval Timeout;
    fd_set WriteFds;

    //
    // Initialize locals.
    //

    ExpectedResult = strlen(Greetings);
    PtmFd = -1;
    PtsFd = -1;
    LXT_SYNCHRONIZATION_POINT_START();

    //
    // Open Master-Subordinate.
    //

    LxtCheckErrno(OpenMasterSubordinate(&PtmFd, &PtsFd, NULL, &SerialNumber));
    LxtLogInfo("Master opened at FD:%d", PtmFd);
    LxtLogInfo("Subordinate Serial Number: %d", SerialNumber);
    LxtLogInfo("Subordinate opened at FD:%d", PtsFd);

    //
    // Suspend output on subordinate.
    //

    LxtCheckErrno(tcflow(PtsFd, TCOOFF));

    //
    // Flush subordinate read and write queues.
    //

    LxtCheckErrno(tcflush(PtsFd, TCIOFLUSH));

    //
    // Fork a thread that should block on suspended output.
    //

    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {

        //
        // subordinate endpoint should not be ready for write.
        //

        memset(&Timeout, 0, sizeof(Timeout));
        FD_ZERO(&WriteFds);
        FD_SET(PtsFd, &WriteFds);
        LxtCheckErrno(select((PtsFd + 1), NULL, &WriteFds, NULL, &Timeout));
        LxtCheckEqual(Result, 0, "%d");

        //
        // Try to write to subordinate.
        //

        LxtLogInfo("Attempt to write to suspended subordinate...");
        LxtCheckErrno(BytesReadWrite = write(PtsFd, Greetings, ExpectedResult));
        LxtCheckFnResults("write", BytesReadWrite, ExpectedResult);

        //
        // Make sure subordinate output is resumed.
        //

        LxtCheckErrno(tcflow(PtsFd, TCOON));
    }
    else
    {

        //
        // Wait a bit to give the child thread time to attempt the write.
        //

        memset(&Timeout, 0, sizeof(Timeout));
        Timeout.tv_sec = 1;
        FD_ZERO(&ReadFds);
        FD_SET(PtmFd, &ReadFds);
        LxtLogInfo("Waiting one second for data...");
        LxtCheckErrno(select((PtmFd + 1), &ReadFds, NULL, NULL, &Timeout));

        //
        // There should be no data available.
        //

        LxtLogInfo("Checking for available data...");
        LxtCheckEqual(Result, 0, "%d");
        LxtCheckErrno(PtmFlags = fcntl(PtmFd, F_GETFL, 0));
        LxtCheckErrno(fcntl(PtmFd, F_SETFL, (PtmFlags | O_NONBLOCK)));
        LxtCheckErrnoFailure(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)), EAGAIN);

        //
        // Resume subordinate output.
        //

        LxtLogInfo("Resuming suspended subordinate...");
        LxtCheckErrno(tcflow(PtsFd, TCOON));

        //
        // Data should become available.
        //

        LxtLogInfo("Checking again for available data...");
        memset(&Timeout, 0, sizeof(Timeout));
        Timeout.tv_sec = 1;
        FD_SET(PtmFd, &ReadFds);
        LxtCheckErrno(select((PtmFd + 1), &ReadFds, NULL, NULL, &Timeout));
        LxtCheckEqual(Result, 1, "%d");
        LxtLogInfo("Reading available data...");
        LxtCheckErrno(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)));
        LxtCheckFnResults("read", BytesReadWrite, ExpectedResult + 1);
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

    LXT_SYNCHRONIZATION_POINT_END();
    return Result;
}
