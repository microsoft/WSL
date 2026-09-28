// Copyright (C) Microsoft Corporation. All rights reserved.
#pragma once

#include <string>

// Trace-logging levels that match the levels used by Windows levels.
#define TRACE_LEVEL_NONE 0        // Tracing is not on
#define TRACE_LEVEL_CRITICAL 1    // Abnormal exit or termination
#define TRACE_LEVEL_ERROR 2       // Severe errors that need logging
#define TRACE_LEVEL_WARNING 3     // Warnings such as allocation failure
#define TRACE_LEVEL_INFORMATION 4 // Includes non-error cases(e.g.,Entry-Exit)
#define TRACE_LEVEL_VERBOSE 5     // Detailed traces from intermediate steps

#if defined(__cplusplus)

namespace p9fs {

// Tracelogging class that has similar methods as its Windows counterpart to simple log statements
// in the cross-platform code will work.
class Plan9TraceLoggingProvider
{
public:
    static void SetLogFileDescriptor(int fd);
    static bool IsEnabled(int level);
    static void SetLevel(int level);
    static void LogMessage(const char* message, int level = TRACE_LEVEL_VERBOSE);
    static void LogMessage(const std::string& message, int level = TRACE_LEVEL_VERBOSE);
    static void LogException(const char* message, const char* exceptionDescription, int level = TRACE_LEVEL_ERROR);
    static void ServerStart();
    static void ServerStop();
    static void AcceptedConnection();
    static void ConnectionDisconnected();
    static void TooManyConnections();
    static void InvalidResponseBufferSize();
    static void PreAccept();
    static void PostAccept();
    static void OperationAborted();
    static void ClientConnected(unsigned int connectionCount);
    static void ClientDisconnected(unsigned int connectionCount);

private:
    Plan9TraceLoggingProvider() = delete;

    static int m_level;
    static int m_log;
};

} // namespace p9fs

#endif
