/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    dev_pt_common.c

Abstract:

    This file is a test for the Pseudo Terminals: /dev/ptmx, /dev/pts/<n>
    devices.

--*/

#include "dev_pt_common.h"

pid_t ForkPtyCommon(int* PtmFdOut, int* PtsFdOut, bool UseMasterEndpoint);

void DumpBuffer(const char Data[], size_t DataSize)

/*++

Routine Description:

    This routine will log the Data.

Arguments:

    Data - Supplies the buffer to be filled.

    DataSize - Supplies the size of the data buffer.

Return Value:

    None

--*/

{

    size_t Index;
    for (Index = 0; Index < DataSize; Index++)
    {
        printf("%d:(", Data[Index]);
        if (Data[Index] == '\n')
        {
            printf("\\n");
        }
        else if (Data[Index] == '\r')
        {
            printf("\\r");
        }
        else if (Data[Index] == '\t')
        {
            printf("\\t");
        }
        else
        {
            printf("%c", Data[Index]);
        }

        printf(") ");
    }

    return;
}

int GetPtSerialNumFromDeviceString(const char PtsNameString[])

/*++

Routine Description:

    This routine will parse the PTS (Pseudo Terminal Slave) device name and
    retrieve the Serial Number from the string.

Arguments:

    PtsNameString - Supplies the device name of the PTS. The name should be
        of the format "/dev/pts/<n>" where 'n' is the Serial Number. The string
        should also be NULL terminated.

Return Value:

    Returns the Serial Number, which is >=0 on success, -1 on failure.

--*/

{

    int NumberOfItemsScanned;
    int Result;
    int SerialNumber;

    SerialNumber = 0;

    NumberOfItemsScanned = sscanf(PtsNameString, "/dev/pts/%d", &SerialNumber);
    if (NumberOfItemsScanned != 1)
    {
        Result = -1;
        goto ErrorExit;
    }

    Result = SerialNumber;

ErrorExit:
    return Result;
}

int GetRandomMessage(char Message[], size_t MessageSize, bool CompleteMessage)
/*++

Routine Description:

    This routine will fill the message buffer with random bytes for the
    specified size. If a complete message is requested, then it will set the
    last byte in the message with the completion character.

Arguments:

    Message - Supplies the buffer to be filled with data.

    MessageSize - Supplies the size to be filled. Size should be >=1.

    CompleteMessage - Supplies the flag which indicates whether the message
        should be completed with a terminating character or not.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{
    size_t Itr;
    size_t NumBytesToFill;

    //
    // If the message has to be completed, last byte is reserved for the
    // terminating character.
    //

    if (CompleteMessage != FALSE)
    {
        NumBytesToFill = MessageSize - 1;
        Message[NumBytesToFill] = '\n';
    }
    else
    {
        NumBytesToFill = MessageSize;
    }

    for (Itr = 0; Itr < NumBytesToFill; Itr += 1)
    {

        //
        // TODO_LX_PTYT: Randomize the data.
        //

        Message[Itr] = 'A' + (Itr % 26);
    }

    return 0;
}

int OpenMasterSubordinate(int* PtmFd, int* PtsFd, char* PtsDevName, int* SerialNumber)

/*++

Routine Description:

    This routine will open a master and subordinate pseudo terminal.
    It will also extract the serial number of the subordinate and
    return it in 'SerialNumber'.

Arguments:

    PtmFd - Supplies the pointer which will be set to the FD of the
        master, on success. This parameter is not optional.

    PtsFd - Supplies the pointer which will be set to the FD of the
        subordinate, on success. This parameter is not optional.

    PtsDevName - Supplies the pointer to the buffer that will hold
        the subordinate device name, on success. This parameter is
        optional.

    SerialNumber - Supplies the pointer which will be set to the
        serial number of the subordinate pseudo terminal. This
        parameter is optional.

Return Value:

    0 on success, error code on failure.
--*/

{

    int Fdm;
    int Fds;
    char LocalBuffer[PTS_DEV_NAME_BUFFER_SIZE];
    int Result;
    int SubordinateSerialNumber;

    //
    // Initialize locals
    //

    Result = -1;
    Fdm = -1;
    Fds = -1;

    LxtCheckErrno((Fdm = open("/dev/ptmx", O_RDWR)));
    LxtCheckErrno(grantpt(Fdm));
    LxtCheckErrno(unlockpt(Fdm));
    LxtCheckErrno(ptsname_r(Fdm, LocalBuffer, PTS_DEV_NAME_BUFFER_SIZE));
    LxtCheckErrno(Fds = open(LocalBuffer, O_RDWR));
    if (PtsDevName != NULL)
    {
        strcpy(PtsDevName, LocalBuffer);
    }

    if (SerialNumber != NULL)
    {
        LxtCheckErrno(SubordinateSerialNumber = GetPtSerialNumFromDeviceString(LocalBuffer));
        *SerialNumber = SubordinateSerialNumber;
    }

    *PtmFd = Fdm;
    *PtsFd = Fds;
    Fdm = -1;
    Fds = -1;
    Result = 0;

ErrorExit:
    if (Fdm != -1)
    {
        close(Fdm);
    }

    if (Fds != -1)
    {
        close(Fds);
    }

    return Result;
}

pid_t ForkPty(int* PtmFdOut, int* PtsFdOut)

/*++

Routine Description:

    This routine sets up a new process as a foreground process using PtsFd as
    its controlling terminal.

Arguments:

    PtmFdOut - Supplies a pointer to receive the master file descriptor.

    PtsFdOut - Supplies a pointer to receive the subordinate file descriptor.

Return Value:

    Returns pid of newly forked process to the parent, zero to the child and
    <0 on error.

--*/

{

    return ForkPtyCommon(PtmFdOut, PtsFdOut, false);
}

int ForkPtyBackground(int* PtmFdOut, int* PtsFdOut, pid_t* ForegroundIdOut)

/*++

Routine Description:

    This routine sets up a new process as a background process using PtsFd as
    its controlling terminal.

Arguments:

    PtmFdOut - Supplies a pointer to receive the master file descriptor.

    PtsFdOut - Supplies a pointer to receive the subordinate file descriptor.

    ForegroundId - Supplies a pointer to receive the ID of the foreground
        process.

Return Value:

    Returns pid of newly forked process to the parent, zero to the child and
    <0 on error.

--*/

{

    int ChildPid;
    int GrandChildPid;
    int GrandChildStatus;
    int PtmFd;
    int PtsFd;
    int Result;

    ChildPid = -1;
    GrandChildPid = -1;
    PtmFd = -1;
    PtsFd = -1;

    LxtCheckErrno(ChildPid = ForkPty(&PtmFd, &PtsFd));
    if (ChildPid == 0)
    {
        *ForegroundIdOut = getpid();
        LxtCheckErrno(GrandChildPid = fork());
        if (GrandChildPid == 0)
        {
            LxtCheckErrno(setpgid(0, 0));
        }
        else
        {
            LxtCheckErrno(TEMP_FAILURE_RETRY(Result = waitpid(GrandChildPid, &GrandChildStatus, 0)));
            LxtCheckResult(WIFEXITED(GrandChildStatus) ? 0 : -1);
            LxtCheckResult((int)(char)WEXITSTATUS(GrandChildStatus));
        }
    }
    else
    {
        *ForegroundIdOut = ChildPid;
    }

    *PtmFdOut = PtmFd;
    PtmFd = -1;
    *PtsFdOut = PtsFd;
    PtsFd = -1;

ErrorExit:
    if (PtmFd != -1)
    {
        close(PtmFd);
    }

    if (PtsFd != -1)
    {
        close(PtsFd);
    }

    if ((ChildPid == 0) && (GrandChildPid > 0))
    {
        exit(Result);
    }

    return ChildPid;
}

pid_t ForkPtyCommon(int* PtmFdOut, int* PtsFdOut, bool UseMasterEndpoint)

/*++

Routine Description:

    This routine sets up a new process as a foreground process using PtsFd as
    its controlling terminal.

Arguments:

    PtmFdOut - Supplies a pointer to receive the master file descriptor.

    PtsFdOut - Supplies a pointer to receive the subordinate file descriptor.

    UseMasterEndpoint - Supplies a flag indicating whether the master or
        subordinate endpoint should be set as the controlling terminal.

Return Value:

    Returns pid of newly forked process to the parent, zero to the child and
    <0 on error.

--*/

{

    pid_t ChildPid;
    int PtmFd;
    int PtsFd;
    int Result;
    int SerialNumber;
    pid_t SessionId;

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

        //
        // Move to a new session
        //

        LxtCheckErrno(SessionId = setsid());

        //
        // Set the fd as the controlling terminal for the session, calling
        // again should not fail.
        //

        LxtCheckErrno(ioctl((UseMasterEndpoint) ? PtmFd : PtsFd, TIOCSCTTY, (char*)NULL));
        LxtCheckErrno(ioctl((UseMasterEndpoint) ? PtmFd : PtsFd, TIOCSCTTY, (char*)NULL));
    }

    *PtmFdOut = PtmFd;
    PtmFd = -1;
    *PtsFdOut = PtsFd;
    PtsFd = -1;

ErrorExit:
    if (PtmFd != -1)
    {
        close(PtmFd);
    }

    if (PtsFd != -1)
    {
        close(PtsFd);
    }

    return ChildPid;
}

pid_t ForkPtyMaster(int* PtmFdOut, int* PtsFdOut)

/*++

Routine Description:

    This routine sets up a new process as a foreground process using PtmFd as
    its controlling terminal.

Arguments:

    PtmFdOut - Supplies a pointer to receive the master file descriptor.

    PtsFdOut - Supplies a pointer to receive the subordinate file descriptor.

Return Value:

    Returns pid of newly forked process to the parent, zero to the child and
    <0 on error.

--*/

{

    return ForkPtyCommon(PtmFdOut, PtsFdOut, true);
}

int RawInit(int Fd)

/*++

Routine Description:

    This routine will use termios to set the FD for raw input/output.

Arguments:

    Fd - Supplies the FD.

Return Value:

    0 on success, error code on failure.

--*/

{

    cc_t ControlArray[NCCS];
    int Result;

    //
    // After the switch to RAW mode want no timeout and a minimum of 1 char.
    //

    Result = TerminalSettingsGetControlArray(Fd, ControlArray);
    if (Result < 0)
    {
        goto ErrorExit;
    }

    ControlArray[VTIME] = 0;
    ControlArray[VMIN] = 1;
    Result = TerminalSettingsSetControlArray(Fd, ControlArray);
    if (Result < 0)
    {
        goto ErrorExit;
    }

    //
    // Disable echo, cannon and other flags. Set TOSTOP so signals are
    // generated by default.
    //

    Result = TerminalSettingsSetLocalFlags(Fd, TOSTOP);

ErrorExit:
    return Result;
}

int SimpleReadWriteCheck(int PtmFd, int PtsFd)

/*++

Routine Description:

    This routine performs a simple read/write check on the master-subordinate
    pseudo terminal pair. The check is as follows:
    - Write to the master.
    - Read from the subordinate. Read data should match the data written
          by master.
    - Write to the subordinate.
    - Read data from the master. Data read from the mater should match
          what was written by the subordinate.

Arguments:

    PtmFd - Supplies the FD for the master.

    PtsFd - Supplies the FD for the subordinate.

Return Value:

    0 on success, error code on failure.

--*/

{

    return SimpleReadWriteCheckEx(PtmFd, PtsFd, SimpleReadWriteForeground);
}

int SimpleReadWriteCheckEx(int PtmFd, int PtsFd, SIMPLE_READ_WRITE_MODE Mode)

/*++

Routine Description:

    This routine performs a simple read/write check on the master-subordinate
    pseudo terminal pair. The check is as follows:
    - Write to the master.
    - Read from the subordinate. Read data should match the data written
          by master.
    - Write to the subordinate.
    - Read data from the master. Data read from the mater should match
          what was written by the subordinate.

Arguments:

    PtmFd - Supplies the FD for the master.

    PtsFd - Supplies the FD for the subordinate.

    Mode - Supplies a value indicating whether this access is from foreground
        or background, and whether signals are enabled.

Return Value:

    0 on success, error code on failure.

--*/

{

    int BytesReadWrite;
    int ExpectedResult;
    const char* Greetings = "Hello there!!\n";
    size_t GreetingsLength;
    int PacketMode;
    int PtmFlags;
    char ReadBuffer[1024];
    char* ReadBufferMaster;
    const char* Reply = "Hi, how are you?\r";
    size_t ReplyLength;
    int Result;
    struct termios TiosMaster;
    struct termios TiosSubordinate;

    LxtCheckErrno(PtmFlags = fcntl(PtmFd, F_GETFL, 0));

    memset(&TiosMaster, 0, sizeof(TiosMaster));
    LxtCheckErrno(tcgetattr(PtmFd, &TiosMaster));
    memset(&TiosSubordinate, 0, sizeof(TiosSubordinate));
    LxtCheckErrno(tcgetattr(PtsFd, &TiosSubordinate));
    LxtCheckMemoryEqual(&TiosMaster, &TiosSubordinate, sizeof(TiosMaster));
    GreetingsLength = strlen(Greetings);
    ReplyLength = strlen(Reply);
    if (TiosSubordinate.c_lflag & ICANON)
    {
        LxtLogInfo("Canonical mode.");
    }
    else
    {
        LxtLogInfo("Raw mode.");
        --GreetingsLength;
        --ReplyLength;
    }

    LxtCheckErrno(ioctl(PtmFd, TIOCGPKT, &PacketMode));
    if (PacketMode != 0)
    {
        LxtLogInfo("Packet mode enabled.");
        ReadBufferMaster = &ReadBuffer[1];
    }
    else
    {
        ReadBufferMaster = &ReadBuffer[0];
    }

    //
    // Write the greetings message to the master.
    //

    LxtLogInfo("Writing to master");
    ExpectedResult = GreetingsLength;
    LxtCheckErrno(BytesReadWrite = write(PtmFd, Greetings, ExpectedResult));
    LxtCheckFnResults("write", BytesReadWrite, ExpectedResult);
    LxtLogInfo("Master(FD:%d) --> subordinate(FD:%d):%*s", PtmFd, PtsFd, GreetingsLength, Greetings);

    //
    // Canonical mode should echo the input back to the master with a
    // carriage-return and newline.
    //

    if (TiosSubordinate.c_lflag & ICANON)
    {
        LxtCheckErrno(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)));
        if (PacketMode != 0)
        {
            LxtCheckEqual(ReadBuffer[0], 0, "%hhd");
            if (BytesReadWrite > 0)
            {
                BytesReadWrite -= 1;
            }
        }

        ReadBufferMaster[BytesReadWrite] = '\0';
        LxtLogInfo("Echo received by master(FD:%d):%s", PtmFd, ReadBufferMaster);
        LxtLogInfo("Last character = %d [\\n = %d, \\r = %d]", ReadBufferMaster[BytesReadWrite - 1], '\n', '\r');
        LxtCheckFnResults("read", BytesReadWrite, (ExpectedResult + 1));
        if ((ReadBufferMaster[BytesReadWrite - 1] != '\n') || (ReadBufferMaster[BytesReadWrite - 2] != '\r'))
        {
            LxtLogError("Echo to master(FD:%d) does not end with \r\n.", PtmFd);

            Result = -1;
            goto ErrorExit;
        }

        ReadBufferMaster[BytesReadWrite - 2] = '\n';
        if (memcmp(ReadBufferMaster, Greetings, ExpectedResult) != 0)
        {
            LxtLogError(
                "Echo to master(FD:%d) does not match what was "
                "written.",
                PtmFd);

            Result = -1;
            goto ErrorExit;
        }
    }

    //
    // Read from subordinate.
    //

    memset(ReadBuffer, 0, sizeof(ReadBuffer));
    LxtLogInfo("Reading from subordinate");
    if (Mode == SimpleReadWriteForeground)
    {
        LxtCheckErrno(BytesReadWrite = read(PtsFd, ReadBuffer, sizeof(ReadBuffer)));
        LxtLogInfo("Message received by subordinate(FD:%d):%s", PtsFd, ReadBuffer);
        LxtLogInfo("Last character = %d [\\n = %d, \\r = %d]", ReadBuffer[BytesReadWrite - 1], '\n', '\r');
        LxtCheckFnResults("read", BytesReadWrite, ExpectedResult);

        //
        // Compare the messages.
        //

        if (memcmp(ReadBuffer, Greetings, BytesReadWrite) != 0)
        {
            LxtLogError(
                "Data read from subordinate(FD:%d) does not match what was "
                "written by master(FD:%d).",
                PtsFd,
                PtmFd);
            Result = -1;
            goto ErrorExit;
        }
    }
    else if ((Mode == SimpleReadWriteBackgroundSignal) || (Mode == SimpleReadWriteBackgroundSignalNoStop))
    {

        LxtCheckErrnoFailure(BytesReadWrite = read(PtsFd, ReadBuffer, sizeof(ReadBuffer)), EINTR);
    }
    else
    {
        LxtCheckErrnoFailure(BytesReadWrite = read(PtsFd, ReadBuffer, sizeof(ReadBuffer)), EIO);
    }

    //
    // So far so good, now write a response from the subordinate.
    //

    LxtLogInfo("Subordinate(FD:%d) --> master(FD:%d):%*s", PtsFd, PtmFd, ReplyLength, Reply);

    ExpectedResult = ReplyLength;
    if (Mode != SimpleReadWriteBackgroundSignal)
    {
        LxtCheckErrno(BytesReadWrite = write(PtsFd, Reply, ExpectedResult));
        LxtCheckFnResults("write", BytesReadWrite, ExpectedResult);
    }
    else
    {
        LxtCheckErrnoFailure(BytesReadWrite = write(PtsFd, Reply, ExpectedResult), EINTR);
        BytesReadWrite = ExpectedResult;
        ExpectedResult = 0;
    }

    //
    // Read from master.
    //

    LxtLogInfo("Reading from master");
    memset(ReadBuffer, 0, sizeof(ReadBuffer));
    if (Mode != SimpleReadWriteBackgroundSignal)
    {
        LxtCheckErrno(BytesReadWrite = read(PtmFd, ReadBuffer, sizeof(ReadBuffer)));
        if (PacketMode != 0)
        {
            LxtCheckEqual(ReadBuffer[0], 0, "%hhd");
            if (BytesReadWrite > 0)
            {
                BytesReadWrite -= 1;
            }
        }

        ReadBufferMaster[BytesReadWrite] = '\0';
        LxtLogInfo("Reply received by master(FD:%d):%s", PtmFd, ReadBufferMaster);
        LxtLogInfo("Last character = %d [\\n = %d, \\r = %d]", ReadBufferMaster[BytesReadWrite - 1], '\n', '\r');
    }
    else
    {
        LxtCheckErrno(fcntl(PtmFd, F_SETFL, (PtmFlags | O_NONBLOCK)));
        LxtCheckErrnoFailure(BytesReadWrite = read(PtmFd, ReadBuffer, BytesReadWrite), EAGAIN);
        BytesReadWrite = 0;
    }

    LxtCheckFnResults("read", BytesReadWrite, ExpectedResult);

    //
    // Compare the messages.
    //

    if (memcmp(ReadBufferMaster, Reply, BytesReadWrite) != 0)
    {
        LxtLogError(
            "Data read from master(FD:%d) does not match what was "
            "written by subordinate(FD:%d).",
            PtmFd,
            PtsFd);

        Result = -1;
        goto ErrorExit;
    }

ErrorExit:
    fcntl(PtmFd, F_SETFL, PtmFlags);
    return Result;
}

int TerminalSettingsGet(int Fd, cc_t* ControlArrayOut, tcflag_t* ControlFlagsOut, tcflag_t* InputFlagsOut, tcflag_t* LocalFlagsOut, tcflag_t* OutputFlagsOut)

/*++

Routine Description:

    This routine will use termios to get the settings for the FD.
    All of the *Out variables are optional.

Arguments:

    Fd - Supplies the FD.

    ControlArrayOut - Supplies an optional pointer to receive the NCCS element
        control array.

    ControlFlagsOut - Supplies an optional pointer to receive the terminal
        control flags.

    InputFlagsOut - Supplies an optional pointer to receive the terminal input
        flags.

    LocalFlagsOut - Supplies an optional pointer to receive the terminal local
        flags.

    OutputFlagsOut - Supplies an optional pointer to receive the terminal
        output flags.

Return Value:

    0 on success, error code on failure.

--*/

{

    int Result;
    struct termios Tios;

    Result = tcgetattr(Fd, &Tios);
    if (Result < 0)
    {
        goto ErrorExit;
    }

    if (ControlArrayOut != NULL)
    {
        memcpy(ControlArrayOut, Tios.c_cc, NCCS * sizeof(cc_t));
    }

    if (ControlFlagsOut != NULL)
    {
        *ControlFlagsOut = Tios.c_cflag;
    }

    if (InputFlagsOut != NULL)
    {
        *InputFlagsOut = Tios.c_iflag;
    }

    if (LocalFlagsOut != NULL)
    {
        *LocalFlagsOut = Tios.c_lflag;
    }

    if (OutputFlagsOut != NULL)
    {
        *OutputFlagsOut = Tios.c_oflag;
    }

ErrorExit:
    return Result;
}

int TerminalSettingsGetControlArray(int Fd, cc_t* ControlArrayOut)

/*++

Routine Description:

    This routine will use termios to get the NCCS element control array.

Arguments:

    Fd - Supplies the FD.

    ControlArrayOut - Supplies a pointer to receive the NCCS element control
        array.

Return Value:

    0 on success, error code on failure.

--*/

{

    return TerminalSettingsGet(Fd, ControlArrayOut, NULL, NULL, NULL, NULL);
}

int TerminalSettingsGetControlFlags(int Fd, tcflag_t* ControlFlagsOut)

/*++

Routine Description:

    This routine will use termios to get the local flags for the FD.

Arguments:

    Fd - Supplies the FD.

    ControlFlagsOut - Supplies a pointer to receive the terminal control flags.

Return Value:

    0 on success, error code on failure.

--*/

{

    return TerminalSettingsGet(Fd, NULL, ControlFlagsOut, NULL, NULL, NULL);
}

int TerminalSettingsGetInputFlags(int Fd, tcflag_t* InputFlagsOut)

/*++

Routine Description:

    This routine will use termios to get the input flags for the FD.

Arguments:

    Fd - Supplies the FD.

    InputFlagsOut - Supplies a pointer to receive the terminal input flags.

Return Value:

    0 on success, error code on failure.

--*/

{

    return TerminalSettingsGet(Fd, NULL, NULL, InputFlagsOut, NULL, NULL);
}

int TerminalSettingsGetLocalFlags(int Fd, tcflag_t* LocalFlagsOut)

/*++

Routine Description:

    This routine will use termios to get the local flags for the FD.

Arguments:

    Fd - Supplies the FD.

    LocalFlagsOut - Supplies a pointer to receive the terminal local flags.

Return Value:

    0 on success, error code on failure.

--*/

{

    return TerminalSettingsGet(Fd, NULL, NULL, NULL, LocalFlagsOut, NULL);
}

int TerminalSettingsGetOutputFlags(int Fd, tcflag_t* OutputFlagsOut)

/*++

Routine Description:

    This routine will use termios to get the output flags for the FD.

Arguments:

    Fd - Supplies the FD.

    OutputFlagsOut - Supplies a pointer to receive the terminal output flags.

Return Value:

    0 on success, error code on failure.

--*/

{

    return TerminalSettingsGet(Fd, NULL, NULL, NULL, NULL, OutputFlagsOut);
}

int TerminalSettingsSet(int Fd, cc_t* ControlArray, tcflag_t ControlFlags, tcflag_t InputFlags, tcflag_t LocalFlags, tcflag_t OutputFlags)

/*++

Routine Description:

    This routine will use termios to update the settings for the FD.

Arguments:

    Fd - Supplies the FD.

    ControlArray - Supplies a pointer to the new NCCS element control array.

    ControlFlags - Supplies the new terminal control flags.

    InputFlagsOut - Supplies the new terminal input flags.

    LocalFlagsOut - Supplies the new terminal local flags.

    OutputFlagsOut - Supplies the new terminal output flags.

Return Value:

    0 on success, error code on failure.

--*/

{

    int Result;
    struct termios Tios = {0};

    memcpy(Tios.c_cc, ControlArray, NCCS * sizeof(cc_t));
    Tios.c_cflag = ControlFlags;
    Tios.c_iflag = InputFlags;
    Tios.c_lflag = LocalFlags;
    Tios.c_oflag = OutputFlags;
    Result = tcsetattr(Fd, TCSANOW, &Tios);
    return Result;
}

int TerminalSettingsSetControlArray(int Fd, cc_t* ControlArray)

/*++

Routine Description:

    This routine will use termios to set the control array for the FD.

Arguments:

    Fd - Supplies the FD.

    ControlArray - Supplies the new control array.

Return Value:

    0 on success, error code on failure.

--*/

{

    int Result;
    struct termios Tios = {0};

    Result = TerminalSettingsGet(Fd, NULL, &Tios.c_cflag, &Tios.c_iflag, &Tios.c_lflag, &Tios.c_oflag);
    if (Result < 0)
    {
        goto ErrorExit;
    }

    Result = TerminalSettingsSet(Fd, ControlArray, Tios.c_cflag, Tios.c_iflag, Tios.c_lflag, Tios.c_oflag);

ErrorExit:
    return Result;
}

int TerminalSettingsSetControlFlags(int Fd, tcflag_t ControlFlags)

/*++

Routine Description:

    This routine will use termios to set the control flags for the FD.

Arguments:

    Fd - Supplies the FD.

    ControlFlags - Supplies the new control flags.

Return Value:

    0 on success, error code on failure.

--*/

{

    int Result;
    struct termios Tios = {0};

    Result = TerminalSettingsGet(Fd, Tios.c_cc, NULL, &Tios.c_iflag, &Tios.c_lflag, &Tios.c_oflag);
    if (Result < 0)
    {
        goto ErrorExit;
    }

    Result = TerminalSettingsSet(Fd, Tios.c_cc, ControlFlags, Tios.c_iflag, Tios.c_lflag, Tios.c_oflag);

ErrorExit:
    return Result;
}

int TerminalSettingsSetInputFlags(int Fd, tcflag_t InputFlags)

/*++

Routine Description:

    This routine will use termios to set the input flags for the FD.

Arguments:

    Fd - Supplies the FD.

    InputFlags - Supplies the new input flags.

Return Value:

    0 on success, error code on failure.

--*/

{

    int Result;
    struct termios Tios = {0};

    Result = TerminalSettingsGet(Fd, Tios.c_cc, &Tios.c_cflag, NULL, &Tios.c_lflag, &Tios.c_oflag);
    if (Result < 0)
    {
        goto ErrorExit;
    }

    Result = TerminalSettingsSet(Fd, Tios.c_cc, Tios.c_cflag, InputFlags, Tios.c_lflag, Tios.c_oflag);

ErrorExit:
    return Result;
}

int TerminalSettingsSetLocalFlags(int Fd, tcflag_t LocalFlags)

/*++

Routine Description:

    This routine will use termios to set the local flags for the FD.

Arguments:

    Fd - Supplies the FD.

    LocalFlags - Supplies the new local flags.

Return Value:

    0 on success, error code on failure.

--*/

{

    int Result;
    struct termios Tios = {0};

    Result = TerminalSettingsGet(Fd, Tios.c_cc, &Tios.c_cflag, &Tios.c_iflag, NULL, &Tios.c_oflag);
    if (Result < 0)
    {
        goto ErrorExit;
    }

    Result = TerminalSettingsSet(Fd, Tios.c_cc, Tios.c_cflag, Tios.c_iflag, LocalFlags, Tios.c_oflag);

ErrorExit:
    return Result;
}

int TerminalSettingsSetOutputFlags(int Fd, tcflag_t OutputFlags)

/*++

Routine Description:

    This routine will use termios to set the output flags for the FD.

Arguments:

    Fd - Supplies the FD.

    OutputFlags - Supplies the new output flags.

Return Value:

    0 on success, error code on failure.

--*/

{

    int Result;
    struct termios Tios = {0};

    Result = TerminalSettingsGet(Fd, Tios.c_cc, &Tios.c_cflag, &Tios.c_iflag, &Tios.c_lflag, NULL);
    if (Result < 0)
    {
        goto ErrorExit;
    }

    Result = TerminalSettingsSet(Fd, Tios.c_cc, Tios.c_cflag, Tios.c_iflag, Tios.c_lflag, OutputFlags);

ErrorExit:
    return Result;
}

int WriteReadFdCommon(int WriteFd, size_t WriteSizes[], size_t NumWriteSizes, int ReadFd, size_t ReadSizes[], size_t NumReadSizes)

/*++

Routine Description:

    This routine performs write operation of given sizes to the WriteFd and
    reads from the ReadFd for the specified sizes. Each write is expected to
    succeed for the given size and each read is expected to succeed for the
    given size. This routine will also validate that the data read aligns up
    with the data written. All the writes will be performed before any of the
    reads.

Arguments:

    WriteFd - Supplies the FD on which the write operation should be called.

    WriteSizes - Supplies the array of sizes for the write operation..

    NumWriteSizes - Supplies the number of elements in the write size array.

    ReadFd - Supplies the FD on which the read operation should be called.

    ReadSizes - Supplies the array of sizes for the read operation..

    NumReadSizes - Supplies the number of elements in the read size array.

Return Value:

    0 on success, error code on failure.
--*/

{

    char* AccumulatedReadBuffer;
    char* AccumulatedWriteBuffer;
    ssize_t BytesReadWrite;
    size_t Itr;
    size_t Offset;
    char* ReadBuffer;
    int Result;
    size_t TotalReadSize;
    size_t TotalWriteSize;
    char* WriteBuffer;

    AccumulatedReadBuffer = NULL;
    AccumulatedWriteBuffer = NULL;
    ReadBuffer = NULL;
    WriteBuffer = NULL;
    TotalWriteSize = 0;
    TotalReadSize = 0;

    //
    // Calculate the size of total number of bytes to be written and read.
    //

    for (Itr = 0; Itr < NumWriteSizes; Itr += 1)
    {
        TotalWriteSize += WriteSizes[Itr];
    }

    for (Itr = 0; Itr < NumReadSizes; Itr += 1)
    {
        TotalReadSize += ReadSizes[Itr];
    }

    //
    // Allocate memory to hold the data for the accumulated writes and reads.
    //

    AccumulatedWriteBuffer = calloc(TotalWriteSize, 1);
    if (AccumulatedWriteBuffer == NULL)
    {
        LxtLogError("Failed to allocate memory for Write Buffer");
        Result = LXT_RESULT_FAILURE;
        goto ErrorExit;
    }

    AccumulatedReadBuffer = calloc(TotalReadSize, 1);
    if (AccumulatedReadBuffer == NULL)
    {
        LxtLogError("Failed to allocate memory for Read Buffer");
        Result = LXT_RESULT_FAILURE;
        goto ErrorExit;
    }

    //
    // First perform all the writes.
    //

    Offset = 0;
    for (Itr = 0; Itr < NumWriteSizes; Itr += 1)
    {
        WriteBuffer = calloc(WriteSizes[Itr], 1);
        if (WriteBuffer == NULL)
        {
            Result = ENOMEM;
            goto ErrorExit;
        }

        LxtCheckErrno(GetRandomMessage(WriteBuffer, WriteSizes[Itr], FALSE));
        LxtCheckErrno(BytesReadWrite = write(WriteFd, WriteBuffer, WriteSizes[Itr]));

        LxtCheckFnResults("write", BytesReadWrite, (ssize_t)WriteSizes[Itr]);
        memcpy(&AccumulatedWriteBuffer[Offset], WriteBuffer, WriteSizes[Itr]);

        Offset += WriteSizes[Itr];
        free(WriteBuffer);
        WriteBuffer = NULL;
    }

    //
    // On Ubuntu16 pty processing is asynchronous so pause a second to give the
    // writes time to be processed.
    //

    sleep(1);

    //
    // Now read the data previously written.
    //

    Offset = 0;
    for (Itr = 0; Itr < NumReadSizes; Itr += 1)
    {
        ReadBuffer = calloc(ReadSizes[Itr], 1);
        if (ReadBuffer == NULL)
        {
            Result = ENOMEM;
            goto ErrorExit;
        }

        LxtCheckErrno(BytesReadWrite = read(ReadFd, ReadBuffer, ReadSizes[Itr]));

        LxtCheckFnResults("read", BytesReadWrite, (ssize_t)ReadSizes[Itr]);
        memcpy(&AccumulatedReadBuffer[Offset], ReadBuffer, ReadSizes[Itr]);
        Offset += ReadSizes[Itr];
        free(ReadBuffer);
        ReadBuffer = NULL;
    }

    //
    // Data read should align up with the previously written data.
    //

    if (memcmp(AccumulatedWriteBuffer, AccumulatedReadBuffer, min(TotalWriteSize, TotalReadSize)) != 0)
    {

        LxtLogError(
            "Data read from FD:%d does not match what was "
            "written by FD:%d.",
            ReadFd,
            WriteFd);

        Result = -1;
        goto ErrorExit;
    }

    /*
    LxtLogInfo("Data Written: ");
    DumpBuffer(AccumulatedWriteBuffer, TotalWriteSize);
    LxtLogInfo("Data Read: ");
    DumpBuffer(AccumulatedReadBuffer, TotalReadSize);
    */

ErrorExit:

    if (AccumulatedReadBuffer != NULL)
    {
        free(AccumulatedReadBuffer);
    }

    if (AccumulatedWriteBuffer != NULL)
    {
        free(AccumulatedWriteBuffer);
    }

    if (ReadBuffer != NULL)
    {
        free(ReadBuffer);
    }

    if (WriteBuffer != NULL)
    {
        free(WriteBuffer);
    }

    return Result;
}
