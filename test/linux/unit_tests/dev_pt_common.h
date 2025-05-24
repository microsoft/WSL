/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    dev_pt_common.h

Abstract:

    This file is a test for the Pseudo Terminals: /dev/ptmx, /dev/pts/<n>
    devices.

--*/

#ifndef _DEV_PT_COMMON
#define _DEV_PT_COMMON

#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/cdefs.h>
#include <linux/limits.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <termios.h>
#include <pthread.h>
#include <stdbool.h>
#include <unistd.h>
#include <pty.h>
#include <sys/ioctl.h>
#include <sys/uio.h>
#include <termios.h>
#include "lxtcommon.h"
#include "unittests.h"

//
// min, max macros
//

#define min(a, b) (((a) < (b)) ? (a) : (b))
#define max(a, b) (((a) > (b)) ? (a) : (b))

//
// TRUE, FALSE macros
//

#define FALSE 0
#define TRUE 1

#define PTS_DEV_NAME_BUFFER_SIZE 50

#define IS_CONTROL_CHAR_ECHO_STRING(s, c) (((s)[0] == '^') && (((s)[1] > 0x40)) && (((s)[1] - 0x40) == (c)))

#define LxtCheckFnResults(fn, result, expectedresult) \
    { \
        if ((result) != (expectedresult)) \
        { \
            LxtLogError("Unexpected results. %s returned with result:%d, expected:%d.", #fn, result, expectedresult); \
            Result = LXT_RESULT_FAILURE; \
            goto ErrorExit; \
        } \
\
        Result = LXT_RESULT_SUCCESS; \
    }

#define LxtClose(_fd) \
    { \
        if ((_fd) >= 0) \
        { \
            LxtCheckErrno(close(_fd)); \
            (_fd) = -1; \
        } \
    }

#ifndef STDIN
#define STDIN 0
#endif

#ifndef STDOUT
#define STDOUT 1
#endif

typedef enum _SIMPLE_READ_WRITE_MODE
{
    SimpleReadWriteForeground,
    SimpleReadWriteBackgroundNoSignal,
    SimpleReadWriteBackgroundSignal,
    SimpleReadWriteBackgroundSignalNoStop
} SIMPLE_READ_WRITE_MODE;

//
// Functions.
//

void DumpBuffer(const char Data[], size_t DataSize);

int GetPtSerialNumFromDeviceString(const char PtsNameString[]);

int GetRandomMessage(char Message[], size_t MessageSize, bool CompleteMessage);

int OpenMasterSubordinate(int* PtmFd, int* PtsFd, char* PtsDevName, int* SerialNumber);

pid_t ForkPty(int* PtmFdOut, int* PtsFdOut);

pid_t ForkPtyBackground(int* PtmFdOut, int* PtsFdOut, pid_t* ForegroundIdOut);

pid_t ForkPtyMaster(int* PtmFdOut, int* PtsFdOut);

void PtSignalHandler(int signal, siginfo_t* signalInfo, void* context);

int RawInit(int Fd);

int SimpleReadWriteCheck(int PtmFd, int PtsFd);

int SimpleReadWriteCheckEx(int PtmFd, int PtsFd, SIMPLE_READ_WRITE_MODE Mode);

int TerminalSettingsGet(int Fd, cc_t* ControlArrayOut, tcflag_t* ControlFlagsOut, tcflag_t* InputFlagsOut, tcflag_t* LocalFlagsOut, tcflag_t* OutputFlagsOut);

int TerminalSettingsGetControlArray(int Fd, cc_t* ControlArrayOut);

int TerminalSettingsGetControlFlags(int Fd, tcflag_t* ControlFlagsOut);

int TerminalSettingsGetInputFlags(int Fd, tcflag_t* InputFlagsOut);

int TerminalSettingsGetLocalFlags(int Fd, tcflag_t* LocalFlagsOut);

int TerminalSettingsGetOutputFlags(int Fd, tcflag_t* OutputFlagsOut);

int TerminalSettingsSet(int Fd, cc_t* ControlArray, tcflag_t ControlFlags, tcflag_t InputFlags, tcflag_t LocalFlags, tcflag_t OutputFlags);

int TerminalSettingsSetControlArray(int Fd, cc_t* ControlArray);

int TerminalSettingsSetControlFlags(int Fd, tcflag_t ControlFlags);

int TerminalSettingsSetInputFlags(int Fd, tcflag_t InputFlags);

int TerminalSettingsSetLocalFlags(int Fd, tcflag_t LocalFlags);

int TerminalSettingsSetOutputFlags(int Fd, tcflag_t OutputFlags);

int WriteReadFdCommon(int WriteFd, size_t WriteSizes[], size_t NumWriteSizes, int ReadFd, size_t ReadSizes[], size_t NumReadSizes);

#endif // _DEV_PT_COMMON
