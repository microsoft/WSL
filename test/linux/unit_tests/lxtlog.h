/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    lxtlog.h

Abstract:

    This file contains lx test logging routines.

--*/

#ifndef _LXT_LOG
#define _LXT_LOG

#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <stdbool.h>

typedef enum _LxtLogType
{
    LxtLogTypeFile = 0x1,
    LxtLogTypePrintf = 0x2,
    LxtLogTypeStress = 0x4,
    LxtLogTypeMax
} LxtLogType,
    *PLxtLogType;

typedef enum _LxtLogLevel
{
    LxtLogLevelInfo = 0,
    LxtLogLevelError,
    LxtLogLevelResourceError,
    LxtLogLevelPass,
    LxtLogLevelStart,
    LxtLogLevelMax
} LxtLogLevel,
    *PLxtLogLevel;

#define LXT_LOG_TYPE_DEFAULT_MASK (LxtLogTypeFile | LxtLogTypePrintf)

void LxtLog(LxtLogLevel LogLevel, const char* Message, ...);

int LxtLogInitialize(const char* TestName, LxtLogType LogTypeMask, bool LogAppend);

void LxtLogUninitialize(void);

#define LXT_RESULT_SUCCESS (0)
#define LXT_RESULT_FAILURE (-1)
#define LXT_SUCCESS(Result) ((Result) != LXT_RESULT_FAILURE)

#define LxtLogStart(str, ...) \
    { \
        LxtLog(LxtLogLevelStart, "START: %s:%u: " str "\n", __FUNCTION__, __LINE__, ##__VA_ARGS__); \
    }

#define LxtLogInfo(str, ...) \
    { \
        LxtLog(LxtLogLevelInfo, "INFO: %s:%u: " str "\n", __FUNCTION__, __LINE__, ##__VA_ARGS__); \
    }

#define LxtLogResourceError(str, ...) \
    { \
        LxtLog(LxtLogLevelResourceError, "RESOURCE_ERROR: %s:%u: " str "\n", __FUNCTION__, __LINE__, ##__VA_ARGS__); \
    }

#define LxtLogError(str, ...) \
    { \
        LxtLog(LxtLogLevelError, "ERROR: %s:%u: " str "\n", __FUNCTION__, __LINE__, ##__VA_ARGS__); \
    }

#define LxtLogPassed(str, ...) \
    { \
        LxtLog(LxtLogLevelPass, "PASS: %s:%u: " str "\n", __FUNCTION__, __LINE__, ##__VA_ARGS__); \
    }

#define LxtCheckErrno(fn) \
    { \
        Result = (fn); \
        if (LXT_SUCCESS(Result) == 0) \
        { \
            LxtLogError("%s failed: %d (%s)", #fn, errno, strerror(errno)); \
            goto ErrorExit; \
        } \
    }

#define LxtCheckMapErrno(fn) \
    { \
        MapResult = (fn); \
        Result = LXT_RESULT_SUCCESS; \
        if (MapResult == MAP_FAILED) \
        { \
            Result = LXT_RESULT_FAILURE; \
            LxtLogError("%s failed: %s", #fn, strerror(errno)); \
            goto ErrorExit; \
        } \
    }

#define LxtCheckMapErrnoFailure(_Fn, _ExpectedErrno) \
    { \
        MapResult = (_Fn); \
        Result = LXT_RESULT_SUCCESS; \
        if (MapResult != MAP_FAILED) \
        { \
            Result = LXT_RESULT_FAILURE; \
            LxtLogError("%s succeeded, expected errno %d", #_Fn, (_ExpectedErrno)); \
            goto ErrorExit; \
        } \
\
        if (errno != (_ExpectedErrno)) \
        { \
            LxtLogError("%s unexpected failure status: %d != %d (%s)", #_Fn, _ExpectedErrno, errno, strerror(errno)); \
\
            Result = LXT_RESULT_FAILURE; \
            goto ErrorExit; \
        } \
    }

#define LxtCheckNullErrno(_Fn) \
    { \
        PointerResult = (_Fn); \
        Result = LXT_RESULT_SUCCESS; \
        if (PointerResult == NULL) \
        { \
            Result = LXT_RESULT_FAILURE; \
            LxtLogError("%s failed: %s", #_Fn, strerror(errno)); \
            goto ErrorExit; \
        } \
    }

#define LxtCheckNullErrnoFailure(_Fn, _ExpectedErrno) \
    { \
        PointerResult = (_Fn); \
        Result = LXT_RESULT_SUCCESS; \
        if (PointerResult != NULL) \
        { \
            LxtLogError("%s succeeded, expected errno %d", #_Fn, (_ExpectedErrno)); \
            Result = LXT_RESULT_FAILURE; \
            goto ErrorExit; \
        } \
\
        if (errno != (_ExpectedErrno)) \
        { \
            LxtLogError("%s unexpected failure status: %d != %d (%s)", #_Fn, _ExpectedErrno, errno, strerror(errno)); \
\
            Result = LXT_RESULT_FAILURE; \
            goto ErrorExit; \
        } \
    }

//
// The following macro is for functions where 0 is the only valid success value
// (they don't return a pid or fd or anything), and where errno is set if an
// error occurs.
//

#define LxtCheckErrnoZeroSuccess(_Fn) \
    { \
        Result = (_Fn); \
        if (Result != LXT_RESULT_SUCCESS) \
        { \
            if (LXT_SUCCESS(Result) != 0) \
            { \
                LxtLogError("%s succeeded with %d, expected 0.", #_Fn, Result); \
            } \
            else \
            { \
                LxtLogError("%s failed: %d, errno %d (%s)", #_Fn, Result, errno, strerror(errno)); \
            } \
\
            Result = LXT_RESULT_FAILURE; \
            goto ErrorExit; \
        } \
    }

#define LxtCheckErrnoFailure(_Fn, _ExpectedErrno) \
    { \
        Result = (_Fn); \
        if ((LXT_SUCCESS(Result) != 0)) \
        { \
            LxtLogError("%s succeeded with %d, expected errno %d", #_Fn, Result, _ExpectedErrno); \
\
            Result = LXT_RESULT_FAILURE; \
            goto ErrorExit; \
        } \
\
        if ((_ExpectedErrno) != errno) \
        { \
            LxtLogError("%s unexpected failure status: %d, %d != %d (%s)", #_Fn, Result, _ExpectedErrno, errno, strerror(errno)); \
\
            Result = LXT_RESULT_FAILURE; \
            goto ErrorExit; \
        } \
\
        Result = LXT_RESULT_SUCCESS; \
    }

#define LxtCheckEqual(_Val1, _Val2, _Format) \
    { \
        if ((_Val1) != (_Val2)) \
        { \
            LxtLogError("{:%s (" _Format ") != %s (" _Format "):}", #_Val1, (_Val1), #_Val2, (_Val2)); \
            Result = LXT_RESULT_FAILURE; \
            goto ErrorExit; \
        } \
\
        Result = LXT_RESULT_SUCCESS; \
    }

#define LxtCheckGreaterOrEqual(_Val1, _Val2, _Format) \
    { \
        if ((_Val1) < (_Val2)) \
        { \
            LxtLogError("{:%s (" _Format ") < %s (" _Format "):}", #_Val1, (_Val1), #_Val2, (_Val2)); \
            Result = LXT_RESULT_FAILURE; \
            goto ErrorExit; \
        } \
\
        Result = LXT_RESULT_SUCCESS; \
    }

#define LxtCheckNotEqual(_Val1, _Val2, _Format) \
    { \
        if ((_Val1) == (_Val2)) \
        { \
            LxtLogError("{:%s (" _Format ") == %s (" _Format "):}", #_Val1, (_Val1), #_Val2, (_Val2)); \
            Result = LXT_RESULT_FAILURE; \
            goto ErrorExit; \
        } \
\
        Result = LXT_RESULT_SUCCESS; \
    }

#define LxtCheckStringEqual(_Val1, _Val2) \
    { \
        if ((((_Val1) == NULL) && ((_Val2) != NULL)) || (((_Val1) != NULL) && ((_Val2) == NULL)) || \
            (((_Val1) != (_Val2)) && (strcmp((_Val1), (_Val2)) != 0))) \
        { \
\
            LxtLogError("%s (\"%s\") != %s (\"%s\")", #_Val1, (_Val1), #_Val2, (_Val2)); \
\
            Result = LXT_RESULT_FAILURE; \
            goto ErrorExit; \
        } \
\
        Result = LXT_RESULT_SUCCESS; \
    }

#define LxtCheckStringNotEqual(_Val1, _Val2) \
    { \
        if (strcmp((_Val1), (_Val2)) == 0) \
        { \
            LxtLogError("%s (\"%s\") == %s (\"%s\")", #_Val1, (_Val1), #_Val2, (_Val2)); \
\
            Result = LXT_RESULT_FAILURE; \
            goto ErrorExit; \
        } \
\
        Result = LXT_RESULT_SUCCESS; \
    }

#define LxtCheckGreater(_Val1, _Val2, _Format) \
    { \
        if ((_Val1) <= (_Val2)) \
        { \
            LxtLogError("{:%s (" _Format ") <= %s (" _Format "):}", #_Val1, (_Val1), #_Val2, (_Val2)); \
            Result = LXT_RESULT_FAILURE; \
            goto ErrorExit; \
        } \
\
        Result = LXT_RESULT_SUCCESS; \
    }

#define LxtCheckMemoryEqual(_Ptr1, _Ptr2, _Size) \
    { \
        Result = LxtCompareMemory((_Ptr1), (_Ptr2), (_Size), #_Ptr1, #_Ptr2); \
        if (Result < 0) \
        { \
            LxtLogError("Memory contents were not equal"); \
            goto ErrorExit; \
        } \
    }

#define LxtCheckTrue(_Val) \
    { \
        if ((_Val) == FALSE) \
        { \
            LxtLogError("The expression (" #_Val ") does not equal true"); \
            Result = LXT_RESULT_FAILURE; \
            goto ErrorExit; \
        } \
\
        Result = LXT_RESULT_SUCCESS; \
    }

#define LxtCheckResult(fn) \
    { \
        Result = (fn); \
        if (LXT_SUCCESS(Result) == 0) \
        { \
            LxtLogError("%s failed", #fn); \
            goto ErrorExit; \
        } \
    }

//
// Macro for functions that return a positive error value on failure, like
// pthread functions.
//

#define LxtCheckResultError(_Fn) \
    { \
        Result = (_Fn); \
        if (Result != 0) \
        { \
            LxtLogError("%s failed: %d (%s)", #_Fn, Result, strerror(Result)); \
            Result = LXT_RESULT_FAILURE; \
            goto ErrorExit; \
        } \
    }

#define LxtCheckResultErrorFailure(_Fn, _ExpectedErrno) \
    { \
        Result = (_Fn); \
        if (Result == 0) \
        { \
            LxtLogError("%s succeeded, expected errno %d", #_Fn, _ExpectedErrno); \
\
            Result = LXT_RESULT_FAILURE; \
            goto ErrorExit; \
        } \
\
        if ((_ExpectedErrno) != Result) \
        { \
            LxtLogError("%s unexpected failure status: %d != %d (%s)", #_Fn, (_ExpectedErrno), Result, strerror(Result)); \
\
            Result = LXT_RESULT_FAILURE; \
            goto ErrorExit; \
        } \
\
        Result = LXT_RESULT_SUCCESS; \
    }

#endif // _LXT_LOG
