/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    lxtlog.c

Abstract:

    This file contains lx test logging routines.

--*/

#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include "lxtutil.h"
#include "lxtlog.h"

#define LXT_LOG_TIMESTAMP_BUFFER_SIZE 64

static LxtLogType g_LxtLogTypeMask = LXT_LOG_TYPE_DEFAULT_MASK;
static const char* g_LxtTestName = "";
static char g_LXTestLogFileName[64] = "/data/test/log/";
static FILE* g_LxtFile = NULL;

void LxtLog(LxtLogLevel LogLevel, const char* Message, ...)

/*++
--*/

{

    static volatile int Active = 0;
    va_list Args;
    char TimeFormat[LXT_LOG_TIMESTAMP_BUFFER_SIZE];
    struct tm* TimeInfo;
    static struct timespec TimeSpec;
    char TimeStamp[LXT_LOG_TIMESTAMP_BUFFER_SIZE];

    if ((LogLevel == LxtLogLevelResourceError) && ((g_LxtLogTypeMask & LxtLogTypeStress) != 0))
    {

        goto Exit;
    }

    //
    // Create a timestamp string which will precede the provided log message.
    // The format of the string is: [Hours:Minutes:Seconds.Milliseconds]
    //
    // N.B. The returns of strftime and snprintf are not checked because the
    //      provided buffers are large enough for the format strings.
    //

    LxtClockGetTime(CLOCK_REALTIME, &TimeSpec);

    //
    // N.B. The signal handling code may use this function and not all of the
    //      functions used here are reentrant safe, resulting in possible
    //      deadlocks or crashes. To avoid these keep an active count and skip
    //      functions known to be problematic in the signal case. The functions
    //      identified at this point are:
    //          - fflush
    //          - localtime
    //          - strftime
    //

    if (__sync_add_and_fetch(&Active, 1) == 1)
    {
        TimeInfo = localtime(&TimeSpec.tv_sec);
        strftime(TimeFormat, sizeof(TimeFormat), "[%H:%M:%S", TimeInfo);
    }

    snprintf(TimeStamp, sizeof(TimeStamp), "%s.%03u] ", TimeFormat, (unsigned int)TimeSpec.tv_nsec / (1000 * 1000));

    //
    // Print the timestamp string followed by the provided message.
    //

    if ((g_LxtLogTypeMask & LxtLogTypePrintf) != 0)
    {
        va_start(Args, Message);
        printf("%s", TimeStamp);
        vprintf(Message, Args);
        va_end(Args);
    }

    if (((g_LxtLogTypeMask & LxtLogTypeFile) != 0) && (g_LxtFile != NULL))
    {
        va_start(Args, Message);
        fprintf(g_LxtFile, "%s", TimeStamp);
        vfprintf(g_LxtFile, Message, Args);
        va_end(Args);
    }

    if (__sync_sub_and_fetch(&Active, 1) == 0)
    {
        sigset_t PreviousSignals;
        sigset_t SignalMask;
        sigfillset(&SignalMask);

        int MaskResult = pthread_sigmask(SIG_BLOCK, &SignalMask, &PreviousSignals);
        if (((g_LxtLogTypeMask & LxtLogTypeFile) != 0) && (g_LxtFile != NULL))
        {
            fflush(g_LxtFile);
        }

        if ((g_LxtLogTypeMask & LxtLogTypePrintf) != 0)
        {
            fflush(stdout);
        }

        if (MaskResult == 0)
        {
            pthread_sigmask(SIG_SETMASK, &PreviousSignals, NULL);
        }
    }

Exit:
    return;
}

int LxtLogInitialize(const char* TestName, LxtLogType LogTypeMask, bool LogAppend)

/*++
--*/

{

    char* Mode;
    int Result;

    g_LxtTestName = TestName;
    g_LxtLogTypeMask = LogTypeMask;
    strcat(g_LXTestLogFileName, TestName);
    if ((g_LxtLogTypeMask & LxtLogTypeFile) != 0)
    {
        Mode = "w";
        if (LogAppend != false)
        {
            Mode = "a";
        }

        g_LxtFile = fopen(g_LXTestLogFileName, Mode);
        if (g_LxtFile == NULL)
        {
            Result = LXT_RESULT_FAILURE;
            g_LxtLogTypeMask &= ~LxtLogTypeFile;
            LxtLogError("Failed to open %s: %s", g_LXTestLogFileName, strerror(errno));

            goto ErrorExit;
        }
    }

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    return Result;
}

void LxtLogUninitialize(void)

/*++
--*/

{

    //
    // Close and flush the log file. This ensures that the file contents are
    // able to be read from Windows.
    //

    if (g_LxtFile != NULL)
    {
        fflush(g_LxtFile);
        fsync(fileno(g_LxtFile));
        fclose(g_LxtFile);
        g_LxtFile = NULL;
    }

    return;
}
