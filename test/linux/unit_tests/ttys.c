/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    Template.c

Abstract:

    This file is a ttys test.

--*/

#include "lxtcommon.h"
#include "unittests.h"
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <fcntl.h>
#include <termios.h>
#include <stdarg.h>
#include <stdio.h>
#include <poll.h>

#define LXT_NAME "Ttys"

#define LXT_TTYS_LARGE_BUFFER_SIZE (1 * 1024)
#define LXT_TTYS_DEFAULT "/dev/ttyS1"
#define LXT_TTYS_DEFAULT_MINOR (LXT_TTYS_DEV_OFFSET + 1)
#define LXT_TTYS_DEFAULT2 "/dev/ttyS2"
#define LXT_TTYS_DEFAULT2_MINOR (LXT_TTYS_DEV_OFFSET + 2)
#define LXT_TTYS_FORMAT "/dev/ttyS%d"
#define LXT_TTYS_MAX 192
#define LXT_TTYS_DEV_MODE (S_IFCHR | 0660)
#define LXT_TTYS_DEV_MAJOR 4
#define LXT_TTYS_DEV_OFFSET 64

LXT_VARIATION_HANDLER TtysBasicOps;
LXT_VARIATION_HANDLER TtysTermiosBaudParity;
LXT_VARIATION_HANDLER TtysWrite;
LXT_VARIATION_HANDLER TtysWindowSize;
LXT_VARIATION_HANDLER TtysTermiosFlowControl;
LXT_VARIATION_HANDLER TtysWriteRead;
LXT_VARIATION_HANDLER TtysModemIoctls;

//
// Global constants
//
// TtysWriteRead requires two connected serial ports for testing.
//

static const LXT_VARIATION g_LxtVariations[] = {
    {"Ttys basic operations", TtysBasicOps},
    {"Ttys termios - baud rate and parity", TtysTermiosBaudParity},
    {"Ttys write", TtysWrite},
    {"Ttys window size", TtysWindowSize},
    /* {"Ttys write read", TtysWriteRead}, */
    {"Ttys termios - flow control", TtysTermiosFlowControl},
    {"Ttys modem ioctls", TtysModemIoctls}};

int TtysTestEntry(int Argc, char* Argv[])

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

int TtysBasicOps(PLXT_ARGS Args)

/*++
--*/

{

    char Buffer[64];
    dev_t Device;
    int Fd;
    char Path[32];
    int Index;
    int Result;

    for (Index = 0; Index < LXT_TTYS_MAX; ++Index)
    {
        snprintf(Path, sizeof(Path), LXT_TTYS_FORMAT, Index);
        unlink(Path);
        Device = makedev(LXT_TTYS_DEV_MAJOR, Index + LXT_TTYS_DEV_OFFSET);
        LxtCheckErrno(mknod(Path, LXT_TTYS_DEV_MODE, Device));
        Fd = open(Path, O_RDWR | O_NONBLOCK, 0);
        if (Fd != -1)
        {
            read(Fd, Buffer, sizeof(Buffer));
            write(Fd, Buffer, sizeof(Buffer));
            fsync(Fd);
            tcflush(Fd, TCIOFLUSH);
            LxtClose(Fd);
        }
    }

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    return Result;
}

int TtysTermiosBaudParity(PLXT_ARGS Args)

/*++
--*/

{

    int BaudRate;
    int BaudRates[] = {B1200, B9600, B9600};
    int Cflag;
    dev_t Device;
    int Iflag;
    int Index;
    int Fd;
    int Result;
    struct termios Termios = {0};
    struct termios TermiosNew = {0};

    Fd = -1;
    Result = LXT_RESULT_FAILURE;

    //
    // Check the default termios values.
    //
    // N.B. Ignore termios fields that may differ.
    //

    Device = makedev(LXT_TTYS_DEV_MAJOR, LXT_TTYS_DEFAULT_MINOR);
    mknod(LXT_TTYS_DEFAULT, LXT_TTYS_DEV_MODE, Device);
    Fd = open(LXT_TTYS_DEFAULT, O_RDWR, 0);
    if (Fd == -1)
    {
        if (errno == EIO)
        {
            LxtLogInfo("Skipping test %d", errno);
            Result = LXT_RESULT_SUCCESS;
        }
        else
        {
            LxtLogError("Unexpected error %d", errno);
        }

        goto ErrorExit;
    }

    LxtCheckErrno(tcgetattr(Fd, &Termios));
    LxtCheckEqual(Termios.c_oflag, 05, "%d");
    LxtCheckEqual(Termios.c_lflag, 0105063, "%d");
    LxtCheckEqual(Termios.c_line, 0, "%d");
    LxtCheckEqual(Termios.c_cc[0], 3, "%d");
    LxtCheckEqual(Termios.c_cc[1], 28, "%d");
    LxtCheckEqual(Termios.c_cc[2], 127, "%d");
    LxtCheckEqual(Termios.c_cc[3], 21, "%d");
    LxtCheckEqual(Termios.c_cc[5], 0, "%d");
    LxtCheckEqual(Termios.c_cc[6], 1, "%d");
    LxtCheckEqual(Termios.c_cc[7], 0, "%d");
    LxtCheckEqual(Termios.c_cc[10], 26, "%d");
    LxtCheckEqual(Termios.c_cc[11], 0, "%d");
    LxtCheckEqual(Termios.c_cc[12], 18, "%d");
    LxtCheckEqual(Termios.c_cc[13], 15, "%d");
    LxtCheckEqual(Termios.c_cc[14], 23, "%d");
    LxtCheckEqual(Termios.c_cc[15], 22, "%d");
    LxtCheckEqual(Termios.c_cc[16], 0, "%d");

    //
    // Set and check the baud rate.
    //

    BaudRate = cfgetispeed(&Termios);
    for (Index = 0; Index < LXT_COUNT_OF(BaudRates); ++Index)
    {
        cfsetspeed(&Termios, BaudRates[Index]);
        LxtCheckErrno(tcsetattr(Fd, TCSANOW, &Termios));
        LxtCheckErrno(tcgetattr(Fd, &TermiosNew));
        LxtCheckMemoryEqual(&Termios, &TermiosNew, sizeof(Termios));
        LxtClose(Fd);
        Fd = -1;
        Fd = open(LXT_TTYS_DEFAULT, O_RDWR, 0);
        LxtCheckErrno(tcgetattr(Fd, &TermiosNew));
        LxtCheckMemoryEqual(&Termios, &TermiosNew, sizeof(Termios));
    }

    //
    // Reset to the original.
    //

    cfsetspeed(&Termios, BaudRate);
    LxtCheckErrno(tcsetattr(Fd, TCSANOW, &Termios));
    LxtCheckErrno(tcgetattr(Fd, &TermiosNew));
    LxtCheckMemoryEqual(&Termios, &TermiosNew, sizeof(Termios));
    LxtClose(Fd);
    Fd = -1;
    Fd = open(LXT_TTYS_DEFAULT, O_RDWR, 0);
    LxtCheckErrno(tcgetattr(Fd, &TermiosNew));
    LxtCheckMemoryEqual(&Termios, &TermiosNew, sizeof(Termios));

    //
    // Set and check the parity enable and type bits
    //
    // N.B. NT does not support setting input and output parity independently,
    //      but Linux does.
    //

    Cflag = Termios.c_cflag;
    Iflag = Termios.c_iflag;

    //
    // No parity, 8 bits
    //

    Termios.c_iflag &= ~INPCK;
    Termios.c_cflag &= ~PARENB;
    Termios.c_cflag &= ~CSTOPB;
    Termios.c_cflag &= ~CMSPAR;
    Termios.c_cflag &= ~CSIZE;
    Termios.c_cflag |= CS8;
    LxtCheckErrno(tcsetattr(Fd, TCSANOW, &Termios));
    LxtCheckErrno(tcgetattr(Fd, &TermiosNew));
    LxtCheckMemoryEqual(&Termios, &TermiosNew, sizeof(Termios));
    LxtClose(Fd);
    Fd = -1;
    Fd = open(LXT_TTYS_DEFAULT, O_RDWR, 0);
    LxtCheckErrno(tcgetattr(Fd, &TermiosNew));
    LxtCheckMemoryEqual(&Termios, &TermiosNew, sizeof(Termios));

    //
    // Even parity, 7 bits
    //

    Termios.c_iflag |= INPCK;
    Termios.c_cflag |= PARENB;
    Termios.c_cflag &= ~PARODD;
    Termios.c_cflag &= ~CMSPAR;
    Termios.c_cflag &= ~CSTOPB;
    Termios.c_cflag &= ~CSIZE;
    Termios.c_cflag |= CS7;
    LxtCheckErrno(tcsetattr(Fd, TCSANOW, &Termios));
    LxtCheckErrno(tcgetattr(Fd, &TermiosNew));
    LxtCheckMemoryEqual(&Termios, &TermiosNew, sizeof(Termios));
    LxtClose(Fd);
    Fd = -1;
    Fd = open(LXT_TTYS_DEFAULT, O_RDWR, 0);
    LxtCheckErrno(tcgetattr(Fd, &TermiosNew));
    LxtCheckMemoryEqual(&Termios, &TermiosNew, sizeof(Termios));

    //
    // Odd parity, 7 bits
    //

    Termios.c_iflag |= INPCK;
    Termios.c_cflag |= PARENB;
    Termios.c_cflag |= PARODD;
    Termios.c_cflag &= ~CMSPAR;
    Termios.c_cflag &= ~CSTOPB;
    Termios.c_cflag &= ~CSIZE;
    Termios.c_cflag |= CS7;
    LxtCheckErrno(tcsetattr(Fd, TCSANOW, &Termios));
    LxtCheckErrno(tcgetattr(Fd, &TermiosNew));
    LxtCheckMemoryEqual(&Termios, &TermiosNew, sizeof(Termios));
    LxtClose(Fd);
    Fd = -1;
    Fd = open(LXT_TTYS_DEFAULT, O_RDWR, 0);
    LxtCheckErrno(tcgetattr(Fd, &TermiosNew));
    LxtCheckMemoryEqual(&Termios, &TermiosNew, sizeof(Termios));

    //
    // Even parity, 5 bits
    //

    Termios.c_iflag |= INPCK;
    Termios.c_cflag |= PARENB;
    Termios.c_cflag &= ~PARODD;
    Termios.c_cflag &= ~CMSPAR;
    Termios.c_cflag &= ~CSTOPB;
    Termios.c_cflag &= ~CSIZE;
    Termios.c_cflag |= CS5;
    LxtCheckErrno(tcsetattr(Fd, TCSANOW, &Termios));
    LxtCheckErrno(tcgetattr(Fd, &TermiosNew));
    LxtCheckMemoryEqual(&Termios, &TermiosNew, sizeof(Termios));
    LxtClose(Fd);
    Fd = -1;
    Fd = open(LXT_TTYS_DEFAULT, O_RDWR, 0);
    LxtCheckErrno(tcgetattr(Fd, &TermiosNew));
    LxtCheckMemoryEqual(&Termios, &TermiosNew, sizeof(Termios));

    //
    // Odd parity, 5 bits, with stop bit
    //

    Termios.c_iflag |= INPCK;
    Termios.c_cflag |= PARENB;
    Termios.c_cflag |= PARODD;
    Termios.c_cflag &= ~CMSPAR;
    Termios.c_cflag |= CSTOPB;
    Termios.c_cflag &= ~CSIZE;
    Termios.c_cflag |= CS5;
    LxtCheckErrno(tcsetattr(Fd, TCSANOW, &Termios));
    LxtCheckErrno(tcgetattr(Fd, &TermiosNew));
    LxtCheckMemoryEqual(&Termios, &TermiosNew, sizeof(Termios));
    LxtClose(Fd);
    Fd = -1;
    Fd = open(LXT_TTYS_DEFAULT, O_RDWR, 0);
    LxtCheckErrno(tcgetattr(Fd, &TermiosNew));
    LxtCheckMemoryEqual(&Termios, &TermiosNew, sizeof(Termios));

    //
    // Space parity, 7 bits
    //

    Termios.c_iflag |= INPCK;
    Termios.c_cflag |= PARENB;
    Termios.c_cflag &= ~PARODD;
    Termios.c_cflag |= CMSPAR;
    Termios.c_cflag &= ~CSTOPB;
    Termios.c_cflag &= ~CSIZE;
    Termios.c_cflag |= CS7;
    LxtCheckErrno(tcsetattr(Fd, TCSANOW, &Termios));
    LxtCheckErrno(tcgetattr(Fd, &TermiosNew));
    LxtCheckMemoryEqual(&Termios, &TermiosNew, sizeof(Termios));
    LxtClose(Fd);
    Fd = -1;
    Fd = open(LXT_TTYS_DEFAULT, O_RDWR, 0);
    LxtCheckErrno(tcgetattr(Fd, &TermiosNew));
    LxtCheckMemoryEqual(&Termios, &TermiosNew, sizeof(Termios));

    //
    // Mark parity, 7 bits
    //

    Termios.c_iflag |= INPCK;
    Termios.c_cflag |= PARENB;
    Termios.c_cflag |= PARODD;
    Termios.c_cflag |= CMSPAR;
    Termios.c_cflag &= ~CSTOPB;
    Termios.c_cflag &= ~CSIZE;
    Termios.c_cflag |= CS7;
    LxtCheckErrno(tcsetattr(Fd, TCSANOW, &Termios));
    LxtCheckErrno(tcgetattr(Fd, &TermiosNew));
    LxtCheckMemoryEqual(&Termios, &TermiosNew, sizeof(Termios));
    LxtClose(Fd);
    Fd = -1;
    Fd = open(LXT_TTYS_DEFAULT, O_RDWR, 0);
    LxtCheckErrno(tcgetattr(Fd, &TermiosNew));
    LxtCheckMemoryEqual(&Termios, &TermiosNew, sizeof(Termios));

    //
    // Reset to the original
    //

    Termios.c_cflag = Cflag;
    Termios.c_iflag = Iflag;
    LxtCheckErrno(tcsetattr(Fd, TCSANOW, &Termios));
    LxtCheckErrno(tcgetattr(Fd, &TermiosNew));
    LxtCheckMemoryEqual(&Termios, &TermiosNew, sizeof(Termios));
    LxtClose(Fd);
    Fd = -1;
    Fd = open(LXT_TTYS_DEFAULT, O_RDWR, 0);
    LxtCheckErrno(tcgetattr(Fd, &TermiosNew));
    LxtCheckMemoryEqual(&Termios, &TermiosNew, sizeof(Termios));

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    if (Fd != -1)
    {
        LxtClose(Fd);
    }

    return Result;
}

int TtysWrite(PLXT_ARGS Args)

/*++
--*/

{

    char Buffer[64];
    int BytesWritten;
    dev_t Device;
    int Fd;
    char Path[32];
    struct pollfd PollFd;
    int Index;
    int Result;

    //
    // Test non-blocking write paths.
    //
    // N.B. Blocking can hang if there is no reader.
    //

    Fd = -1;
    Device = makedev(LXT_TTYS_DEV_MAJOR, LXT_TTYS_DEFAULT_MINOR);
    mknod(LXT_TTYS_DEFAULT, LXT_TTYS_DEV_MODE, Device);
    Fd = open(LXT_TTYS_DEFAULT, O_RDWR | O_NONBLOCK, 0);
    if (Fd == -1)
    {
        if (errno == EIO)
        {
            LxtLogInfo("Skipping test %d", errno);
            Result = LXT_RESULT_SUCCESS;
        }
        else
        {
            LxtLogError("Unexpected error %d", errno);
        }

        goto ErrorExit;
    }

    memset(&PollFd, 0, sizeof(PollFd));
    PollFd.fd = Fd;
    PollFd.events = POLLIN | POLLOUT | POLLHUP;
    LxtCheckErrno(poll(&PollFd, 1, 0));
    LxtCheckEqual(PollFd.revents & POLLOUT, POLLOUT, "%d");

    errno = 0;
    BytesWritten = write(Fd, Buffer, sizeof(Buffer));
    if ((BytesWritten != sizeof(Buffer)) && (errno != EAGAIN))
    {
        LxtLogError("Unexpected BytesWritten %d, %d", BytesWritten, errno);
        Result = LXT_RESULT_FAILURE;
        goto ErrorExit;
    }

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    if (Fd != -1)
    {
        LxtClose(Fd);
    }

    return Result;
}

int TtysWindowSize(PLXT_ARGS Args)

/*++
--*/

{

    dev_t Device;
    int Fd;
    char Path[32];
    int Result;
    struct winsize WindowSize;
    struct winsize WindowSizeNew;

    //
    // Test setting and getting the window size of a serial device.
    //

    Fd = -1;
    Device = makedev(LXT_TTYS_DEV_MAJOR, LXT_TTYS_DEFAULT_MINOR);
    mknod(LXT_TTYS_DEFAULT, LXT_TTYS_DEV_MODE, Device);
    Fd = open(LXT_TTYS_DEFAULT, O_RDWR, 0);
    if (Fd == -1)
    {
        if (errno == EIO)
        {
            LxtLogInfo("Skipping test %d", errno);
            Result = LXT_RESULT_SUCCESS;
        }
        else
        {
            LxtLogError("Unexpected error %d", errno);
        }

        goto ErrorExit;
    }

    LxtCheckErrno(ioctl(Fd, TIOCGWINSZ, &WindowSize));
    LxtLogInfo("%d, %d", WindowSize.ws_row, WindowSize.ws_col);
    WindowSizeNew = WindowSize;
    WindowSizeNew.ws_row += 1;
    WindowSizeNew.ws_col += 1;
    LxtCheckErrno(ioctl(Fd, TIOCSWINSZ, &WindowSizeNew));
    LxtCheckErrno(ioctl(Fd, TIOCGWINSZ, &WindowSize));
    LxtCheckMemoryEqual(&WindowSize, &WindowSizeNew, sizeof(WindowSize));
    memset(&WindowSizeNew, 0, sizeof(WindowSizeNew));
    LxtCheckErrno(ioctl(Fd, TIOCSWINSZ, &WindowSizeNew));
    LxtCheckErrno(ioctl(Fd, TIOCGWINSZ, &WindowSize));
    LxtCheckMemoryEqual(&WindowSize, &WindowSizeNew, sizeof(WindowSize));

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    if (Fd != -1)
    {
        LxtClose(Fd);
    }

    return Result;
}

int TtysTermiosFlowControl(PLXT_ARGS Args)

/*++
--*/

{

    int Cflag;
    dev_t Device;
    int Iflag;
    int Index;
    int Fd;
    int Result;
    struct termios Termios = {0};
    struct termios TermiosNew = {0};
    char VStart;
    char VStop;

    Fd = -1;
    Result = LXT_RESULT_FAILURE;

    //
    // Check the default termios values.
    //
    // N.B. Ignore the termios settings that may differ..
    //

    Device = makedev(LXT_TTYS_DEV_MAJOR, LXT_TTYS_DEFAULT_MINOR);
    mknod(LXT_TTYS_DEFAULT, LXT_TTYS_DEV_MODE, Device);
    Fd = open(LXT_TTYS_DEFAULT, O_RDWR | O_NONBLOCK, 0);
    if (Fd == -1)
    {
        if (errno == EIO)
        {
            LxtLogInfo("Skipping test %d", errno);
            Result = LXT_RESULT_SUCCESS;
        }
        else
        {
            LxtLogError("Unexpected error %d", errno);
        }

        goto ErrorExit;
    }

    //
    // Set and check the parity enable and type bits
    //
    // N.B. NT does not support setting input and output parity independently,
    //      but Linux does.
    //

    LxtCheckErrno(tcgetattr(Fd, &Termios));
    Cflag = Termios.c_cflag;
    Iflag = Termios.c_iflag;
    VStart = Termios.c_cc[VSTART];
    VStop = Termios.c_cc[VSTOP];

    //
    // ixon and ixoff set
    //

    Termios.c_iflag |= IXON;
    Termios.c_iflag |= IXOFF;
    LxtCheckErrno(tcsetattr(Fd, TCSANOW, &Termios));
    LxtCheckErrno(tcgetattr(Fd, &TermiosNew));
    LxtCheckMemoryEqual(&Termios, &TermiosNew, sizeof(Termios));
    LxtClose(Fd);
    Fd = -1;
    Fd = open(LXT_TTYS_DEFAULT, O_RDWR | O_NONBLOCK, 0);
    LxtCheckErrno(tcgetattr(Fd, &TermiosNew));
    LxtCheckMemoryEqual(&Termios, &TermiosNew, sizeof(Termios));

    //
    // ixon and ixoff not set
    //

    Termios.c_iflag &= ~IXON;
    Termios.c_iflag &= ~IXOFF;
    LxtCheckErrno(tcsetattr(Fd, TCSANOW, &Termios));
    LxtCheckErrno(tcgetattr(Fd, &TermiosNew));
    LxtCheckMemoryEqual(&Termios, &TermiosNew, sizeof(Termios));
    LxtClose(Fd);
    Fd = -1;
    Fd = open(LXT_TTYS_DEFAULT, O_RDWR | O_NONBLOCK, 0);
    LxtCheckErrno(tcgetattr(Fd, &TermiosNew));
    LxtCheckMemoryEqual(&Termios, &TermiosNew, sizeof(Termios));

    //
    // crtscts set
    //

    Termios.c_cflag |= CRTSCTS;
    LxtCheckErrno(tcsetattr(Fd, TCSANOW, &Termios));
    LxtCheckErrno(tcgetattr(Fd, &TermiosNew));
    LxtCheckMemoryEqual(&Termios, &TermiosNew, sizeof(Termios));
    LxtClose(Fd);
    Fd = -1;
    Fd = open(LXT_TTYS_DEFAULT, O_RDWR | O_NONBLOCK, 0);
    LxtCheckErrno(tcgetattr(Fd, &TermiosNew));
    LxtCheckMemoryEqual(&Termios, &TermiosNew, sizeof(Termios));

    //
    // crtscts not set
    //

    Termios.c_cflag &= ~CRTSCTS;
    LxtCheckErrno(tcsetattr(Fd, TCSANOW, &Termios));
    LxtCheckErrno(tcgetattr(Fd, &TermiosNew));
    LxtCheckMemoryEqual(&Termios, &TermiosNew, sizeof(Termios));
    LxtClose(Fd);
    Fd = -1;
    Fd = open(LXT_TTYS_DEFAULT, O_RDWR | O_NONBLOCK, 0);
    LxtCheckErrno(tcgetattr(Fd, &TermiosNew));
    LxtCheckMemoryEqual(&Termios, &TermiosNew, sizeof(Termios));

    //
    // clocal not set
    //

    LxtLogInfo("Clearing clocal");
    Termios.c_cflag &= ~CLOCAL;
    LxtCheckErrno(tcsetattr(Fd, TCSANOW, &Termios));
    LxtCheckErrno(tcgetattr(Fd, &TermiosNew));
    LxtCheckMemoryEqual(&Termios, &TermiosNew, sizeof(Termios));
    LxtClose(Fd);
    Fd = -1;
    Fd = open(LXT_TTYS_DEFAULT, O_RDWR | O_NONBLOCK, 0);
    LxtCheckErrno(tcgetattr(Fd, &TermiosNew));
    LxtCheckMemoryEqual(&Termios, &TermiosNew, sizeof(Termios));

    //
    // clocal set
    //

    Termios.c_cflag |= CLOCAL;
    LxtCheckErrno(tcsetattr(Fd, TCSANOW, &Termios));
    LxtCheckErrno(tcgetattr(Fd, &TermiosNew));
    LxtCheckMemoryEqual(&Termios, &TermiosNew, sizeof(Termios));
    LxtClose(Fd);
    Fd = -1;
    Fd = open(LXT_TTYS_DEFAULT, O_RDWR | O_NONBLOCK, 0);
    LxtCheckErrno(tcgetattr(Fd, &TermiosNew));
    LxtCheckMemoryEqual(&Termios, &TermiosNew, sizeof(Termios));

    //
    // Flip vstart and vstop
    //

    LxtLogInfo("Updating vstart and vstop");
    Termios.c_cc[VSTART] = VStop;
    Termios.c_cc[VSTOP] = VStart;
    LxtCheckErrno(tcsetattr(Fd, TCSANOW, &Termios));
    LxtCheckErrno(tcgetattr(Fd, &TermiosNew));
    LxtCheckMemoryEqual(&Termios, &TermiosNew, sizeof(Termios));
    LxtClose(Fd);
    Fd = -1;
    Fd = open(LXT_TTYS_DEFAULT, O_RDWR | O_NONBLOCK, 0);
    LxtCheckErrno(tcgetattr(Fd, &TermiosNew));
    LxtCheckMemoryEqual(&Termios, &TermiosNew, sizeof(Termios));

    //
    // Reset to the original
    //

    LxtLogInfo("Resetting...");
    Termios.c_cflag = Cflag;
    Termios.c_iflag = Iflag;
    Termios.c_cc[VSTART] = VStart;
    Termios.c_cc[VSTOP] = VStop;
    LxtCheckErrno(tcsetattr(Fd, TCSANOW, &Termios));
    LxtCheckErrno(tcgetattr(Fd, &TermiosNew));
    LxtCheckMemoryEqual(&Termios, &TermiosNew, sizeof(Termios));
    LxtClose(Fd);
    Fd = -1;
    Fd = open(LXT_TTYS_DEFAULT, O_RDWR | O_NONBLOCK, 0);
    LxtCheckErrno(tcgetattr(Fd, &TermiosNew));
    LxtCheckMemoryEqual(&Termios, &TermiosNew, sizeof(Termios));

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    if (Fd != -1)
    {
        LxtClose(Fd);
    }

    return Result;
}

int TtysWriteReadTransfer(PLXT_ARGS Args, int FdRead, int FdWrite)

/*++
--*/

{

    int BytesRead;
    int BytesWritten;
    int BytesTotal;
    pid_t ChildPid;
    int Index;
    char* RecvBuffer;
    int Result;
    char* SendBuffer;

    ChildPid = -1;
    RecvBuffer = NULL;
    SendBuffer = NULL;

    //
    // Allocate a buffer with known data to transfer and a buffer to receive
    // the data.
    //

    SendBuffer = LxtAlloc(LXT_TTYS_LARGE_BUFFER_SIZE);
    if (SendBuffer == NULL)
    {
        Result = LXT_RESULT_FAILURE;
        goto ErrorExit;
    }

    for (Index = 0; Index < LXT_TTYS_LARGE_BUFFER_SIZE; ++Index)
    {
        SendBuffer[Index] = (char)Index;
    }

    RecvBuffer = LxtAlloc(LXT_TTYS_LARGE_BUFFER_SIZE);
    if (RecvBuffer == NULL)
    {
        Result = LXT_RESULT_FAILURE;
        goto ErrorExit;
    }

    memset(RecvBuffer, 0, LXT_TTYS_LARGE_BUFFER_SIZE);

    //
    // Send and recv the data.
    //

    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        for (BytesTotal = 0; BytesTotal < LXT_TTYS_LARGE_BUFFER_SIZE; BytesTotal += BytesWritten)
        {
            BytesWritten = write(FdWrite, &SendBuffer[BytesTotal], LXT_TTYS_LARGE_BUFFER_SIZE - BytesTotal);

            if (BytesWritten == -1)
            {
                if ((errno == EAGAIN) || (errno == EINTR))
                {
                    BytesWritten = 0;
                    continue;
                }

                Result = LXT_RESULT_FAILURE;
                LxtLogError("Write failed with %d", errno);
                goto ErrorExit;
            }
        }

        _exit(0);
    }

    for (BytesTotal = 0; BytesTotal < LXT_TTYS_LARGE_BUFFER_SIZE; BytesTotal += BytesRead)
    {
        BytesRead = read(FdRead, &RecvBuffer[BytesTotal], LXT_TTYS_LARGE_BUFFER_SIZE - BytesTotal);

        if (BytesRead == -1)
        {
            if ((errno == EAGAIN) || (errno == EINTR))
            {
                BytesRead = 0;
                continue;
            }

            Result = LXT_RESULT_FAILURE;
            LxtLogError("Read failed with %d", errno);
            goto ErrorExit;
        }
    }

    LxtWaitPidPoll(ChildPid, 0);
    for (Index = 0; Index < LXT_TTYS_LARGE_BUFFER_SIZE; ++Index)
    {
        if (RecvBuffer[Index] != (char)Index)
        {
            Result = LXT_RESULT_FAILURE;
            LxtLogError("Mismatch at index %d: %d != %d", Index, (int)(RecvBuffer[Index]), (int)((char)Index));

            goto ErrorExit;
        }
    }

    Result = 0;

ErrorExit:
    if (ChildPid == 0)
    {
        _exit(Result);
    }

    if (RecvBuffer != NULL)
    {
        LxtFree(RecvBuffer);
    }

    if (SendBuffer != NULL)
    {
        LxtFree(SendBuffer);
    }

    return Result;
}

int TtysWriteReadTermios(PLXT_ARGS Args, int FdRead, int FdWrite)

/*++
--*/

{

    struct termios FdReadTermios;
    int Fds[2];
    struct termios FdWriteTermios;
    int Index;
    struct termios Termios;
    int Result;

    //
    // Set the termios structure for transferring raw data.
    //

    Fds[0] = FdRead;
    Fds[1] = FdWrite;
    for (Index = 0; Index < LXT_COUNT_OF(Fds); ++Index)
    {
        LxtCheckErrno(tcgetattr(Fds[Index], &Termios));
        Termios.c_iflag = 0;
        Termios.c_oflag = 0;
        Termios.c_cflag = B115200 | CREAD | CS8;
        Termios.c_lflag = 0;
        LxtCheckErrno(tcsetattr(Fds[Index], TCSANOW, &Termios));
    }

    memset(&FdReadTermios, 0, sizeof(FdReadTermios));
    LxtCheckErrno(tcgetattr(FdRead, &FdReadTermios));
    memset(&FdWriteTermios, 0, sizeof(FdWriteTermios));
    LxtCheckErrno(tcgetattr(FdWrite, &FdWriteTermios));
    LxtCheckMemoryEqual(&FdReadTermios, &FdWriteTermios, sizeof(FdWriteTermios));

    Result = 0;

ErrorExit:
    return Result;
}

int TtysWriteRead(PLXT_ARGS Args)

/*++
--*/

{

    int BytesWritten;
    dev_t Device;
    int FdRead;
    int FdWrite;
    char Path[32];
    int Result;

    FdRead = -1;
    FdWrite = -1;

    Device = makedev(LXT_TTYS_DEV_MAJOR, LXT_TTYS_DEFAULT_MINOR);
    mknod(LXT_TTYS_DEFAULT, LXT_TTYS_DEV_MODE, Device);
    LxtCheckErrno(FdRead = open(LXT_TTYS_DEFAULT, O_RDWR, 0));
    Device = makedev(LXT_TTYS_DEV_MAJOR, LXT_TTYS_DEFAULT2_MINOR);
    mknod(LXT_TTYS_DEFAULT2, LXT_TTYS_DEV_MODE, Device);
    LxtCheckErrno(FdWrite = open(LXT_TTYS_DEFAULT2, O_RDWR, 0));
    LxtCheckResult(TtysWriteReadTermios(Args, FdRead, FdWrite));

    //
    // Transfer data with blocking IO
    //

    LxtCheckResult(TtysWriteReadTransfer(Args, FdRead, FdWrite));

    //
    // Transfer again with non-blocking IO.
    //

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    if (FdRead != -1)
    {
        LxtClose(FdRead);
    }

    if (FdWrite != -1)
    {
        LxtClose(FdWrite);
    }

    return Result;
}

int TtysModemIoctls(PLXT_ARGS Args)

/*++
--*/

{

    dev_t Device;
    int Fd;
    int ModemSettings;
    int ModemSettingsOrig;
    int Result;
    struct termios Termios;
    struct termios TermiosOrig;

    Fd = -1;
    ModemSettingsOrig = -1;
    Result = LXT_RESULT_FAILURE;

    //
    // Check the default modem settings
    //

    Device = makedev(LXT_TTYS_DEV_MAJOR, LXT_TTYS_DEFAULT_MINOR);
    mknod(LXT_TTYS_DEFAULT, LXT_TTYS_DEV_MODE, Device);
    Fd = open(LXT_TTYS_DEFAULT, O_RDWR | O_NONBLOCK, 0);
    if (Fd == -1)
    {
        if (errno == EIO)
        {
            LxtLogInfo("Skipping test %d", errno);
            Result = LXT_RESULT_SUCCESS;
        }
        else
        {
            LxtLogError("Unexpected error %d", errno);
        }

        goto ErrorExit;
    }

    LxtCheckResult(ioctl(Fd, TIOCMGET, &ModemSettingsOrig));
    LxtLogInfo("ModemSettingsOrig: %d", ModemSettingsOrig);
    ModemSettings = ModemSettingsOrig;
    LxtCheckResult(ioctl(Fd, TIOCMSET, &ModemSettings));
    LxtCheckResult(ioctl(Fd, TIOCMGET, &ModemSettings));
    LxtCheckEqual(ModemSettings, ModemSettingsOrig, "%d");

    //
    // Check that invalid settings are ignored
    //

    ModemSettings = -1;
    LxtCheckResult(ioctl(Fd, TIOCMSET, &ModemSettings));
    LxtCheckResult(ioctl(Fd, TIOCMGET, &ModemSettings));
    LxtCheckNotEqual(ModemSettings, ModemSettingsOrig, "%d");
    ModemSettings = -1;
    LxtCheckResult(ioctl(Fd, TIOCMBIC, &ModemSettings));
    LxtCheckResult(ioctl(Fd, TIOCMGET, &ModemSettings));
    LxtCheckEqual(ModemSettings, 0, "%d");
    ModemSettings = -1;
    LxtCheckResult(ioctl(Fd, TIOCMBIS, &ModemSettings));
    LxtCheckResult(ioctl(Fd, TIOCMGET, &ModemSettings));
    LxtCheckNotEqual(ModemSettings, ModemSettingsOrig, "%d");

    //
    // Recheck the settings after closing and reopening.
    //

    LxtClose(Fd);
    Fd = -1;
    LxtCheckErrno(Fd = open(LXT_TTYS_DEFAULT, O_RDWR | O_NONBLOCK, 0));
    LxtCheckResult(ioctl(Fd, TIOCMGET, &ModemSettings));
    LxtCheckNotEqual(ModemSettings, ModemSettingsOrig, "%d");

    //
    // Check DTR
    //
    // N.B. Some serial drivers start up with DTR on native Linux.
    //

    ModemSettings = ModemSettingsOrig & ~TIOCM_DTR;
    LxtCheckResult(ioctl(Fd, TIOCMSET, &ModemSettings));
    LxtCheckResult(ioctl(Fd, TIOCMGET, &ModemSettings));
    LxtCheckEqual(ModemSettings, ModemSettingsOrig & ~TIOCM_DTR, "%d");
    LxtClose(Fd);
    Fd = -1;
    LxtCheckErrno(Fd = open(LXT_TTYS_DEFAULT, O_RDWR | O_NONBLOCK, 0));

    ModemSettings = TIOCM_DTR;
    LxtCheckResult(ioctl(Fd, TIOCMBIS, &ModemSettings));
    LxtCheckResult(ioctl(Fd, TIOCMGET, &ModemSettings));
    LxtCheckEqual(ModemSettings, ModemSettingsOrig | TIOCM_DTR, "%d");
    LxtClose(Fd);
    Fd = -1;
    LxtCheckErrno(Fd = open(LXT_TTYS_DEFAULT, O_RDWR | O_NONBLOCK, 0));
    LxtCheckResult(ioctl(Fd, TIOCMGET, &ModemSettings));
    LxtCheckEqual(ModemSettings, ModemSettingsOrig | TIOCM_DTR, "%d");

    //
    // Check that changing RTS doesn't impact termios
    //

    LxtCheckErrno(tcgetattr(Fd, &TermiosOrig));
    ModemSettings = TIOCM_RTS;
    LxtCheckResult(ioctl(Fd, TIOCMBIS, &ModemSettings));
    LxtCheckErrno(tcgetattr(Fd, &Termios));
    LxtCheckMemoryEqual(&TermiosOrig, &Termios, sizeof(Termios));
    ModemSettings = TIOCM_RTS;
    LxtCheckResult(ioctl(Fd, TIOCMBIC, &ModemSettings));
    LxtCheckErrno(tcgetattr(Fd, &Termios));
    LxtCheckMemoryEqual(&TermiosOrig, &Termios, sizeof(Termios));

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    if (Fd != -1)
    {
        if (ModemSettingsOrig != -1)
        {
            LxtCheckResult(ioctl(Fd, TIOCMSET, &ModemSettingsOrig));
        }

        LxtClose(Fd);
    }

    return Result;
}
