// Copyright (C) Microsoft Corporation. All rights reserved.
#include "precomp.h"
#include "p9defs.h"
#include "p9tracelogging.h"
#include "p9tracelogginghelper.h"

namespace {

const char* c_levelLabels[] = {": CRITICAL: ", ": ERROR:    ", ": WARNING:  ", ": INFO:     ", ": VERBOSE:  "};

constexpr int c_levelLabelLength = 12;

} // namespace

namespace p9fs {

int Plan9TraceLoggingProvider::m_level{TRACE_LEVEL_ERROR};
int Plan9TraceLoggingProvider::m_log{-1};

constexpr int c_numberBufferSize = 64;

// Helper to convert unsigned numbers without the use of printf or iostream.
std::string_view ConvertNumber(char* buffer, int bufferSize, UINT64 value, int base = 10, int minWidth = 0)
{
    if (value == 0)
    {
        return "0";
    }

    auto* characters = "0123456789abcdef";
    int index;
    for (index = bufferSize - 1; index > 0 && value > 0; --index, value /= base)
    {
        buffer[index] = characters[value % base];
    }

    for (; index > 0 && c_numberBufferSize - (index + 1) < minWidth; --index)
    {
        buffer[index] = '0';
    }

    if (base == 16 && index > 1)
    {
        buffer[index--] = 'x';
        buffer[index--] = '0';
    }

    index += 1;
    return {&buffer[index], c_numberBufferSize - index};
}

// Sets the file descriptor to log to.
void Plan9TraceLoggingProvider::SetLogFileDescriptor(int fd)
{
    m_log = fd;
}

// Checks whether logging is enabled for messages of the specified level.
bool Plan9TraceLoggingProvider::IsEnabled(int level)
{
    return m_log >= 0 && level <= m_level;
}

// Sets the current logging level.
void Plan9TraceLoggingProvider::SetLevel(int level)
{
    m_level = level;
}

// Logs a message at the specified level.
void Plan9TraceLoggingProvider::LogMessage(const char* message, int level)
{
    if (!IsEnabled(level))
    {
        return;
    }

    timespec timestamp;
    clock_gettime(CLOCK_MONOTONIC, &timestamp);
    char secondsBuffer[c_numberBufferSize];
    auto seconds = ConvertNumber(secondsBuffer, c_numberBufferSize, timestamp.tv_sec);
    char nsecondsBuffer[c_numberBufferSize];
    auto nseconds = ConvertNumber(nsecondsBuffer, c_numberBufferSize, timestamp.tv_nsec, 10, 9);
    if (level < 1)
    {
        level = 1;
    }
    else if (level > TRACE_LEVEL_VERBOSE)
    {
        level = TRACE_LEVEL_VERBOSE;
    }

    // Use writev to ensure the message is written atomically with all its parts.
    iovec buffers[6];
    buffers[0].iov_base = const_cast<char*>(seconds.data());
    buffers[0].iov_len = seconds.size();
    buffers[1].iov_base = const_cast<char*>(".");
    buffers[1].iov_len = 1;
    buffers[2].iov_base = const_cast<char*>(nseconds.data());
    buffers[2].iov_len = nseconds.size();
    buffers[3].iov_base = const_cast<char*>(c_levelLabels[level - 1]);
    buffers[3].iov_len = c_levelLabelLength;
    buffers[4].iov_base = const_cast<char*>(message);
    buffers[4].iov_len = strlen(message);
    buffers[5].iov_base = const_cast<char*>("\n");
    buffers[5].iov_len = 1;
    writev(m_log, buffers, std::extent<decltype(buffers)>::value);
}

// Logs a message at the specified level.
void Plan9TraceLoggingProvider::LogMessage(const std::string& message, int level)
{
    LogMessage(message.c_str(), level);
}

// Logs an exception with an optional additional message to the output.
void Plan9TraceLoggingProvider::LogException(const char* message, const char* exceptionDescription, int level)
{
    if (!IsEnabled(level))
    {
        return;
    }

    std::string logMessage;
    if (message != nullptr)
    {
        logMessage += message;
        if (exceptionDescription != nullptr)
        {
            logMessage += " ";
        }
    }

    if (exceptionDescription != nullptr)
    {
        logMessage += "Exception: ";
        logMessage += exceptionDescription;
    }

    LogMessage(logMessage.c_str(), level);
}

// Logs a message that the server has started.
void Plan9TraceLoggingProvider::ServerStart()
{
    LogMessage("Server started.", TRACE_LEVEL_INFORMATION);
}

// Logs a message that the server has stopped.
void Plan9TraceLoggingProvider::ServerStop()
{
    LogMessage("Server stopped.", TRACE_LEVEL_INFORMATION);
}

// Logs a message that the server has accepted a connection.
void Plan9TraceLoggingProvider::AcceptedConnection()
{
    LogMessage("Accepted connection.", TRACE_LEVEL_INFORMATION);
}

// Logs a message that a connection was disconnected.
void Plan9TraceLoggingProvider::ConnectionDisconnected()
{
    LogMessage("Connection disconnected.", TRACE_LEVEL_INFORMATION);
}

// Logs a message that the server has rejected a connection attempt because there are too many
// active connections.
void Plan9TraceLoggingProvider::TooManyConnections()
{
    LogMessage("Too many connections.", TRACE_LEVEL_ERROR);
}

// Logs a message indicating that the buffer provided by the virtio transport for the response is
// too small.
void Plan9TraceLoggingProvider::InvalidResponseBufferSize()
{
    LogMessage("Invalid response buffer size.", TRACE_LEVEL_ERROR);
}

// A socket has been accepted
void Plan9TraceLoggingProvider::PreAccept()
{
    LogMessage("PreAccept", TRACE_LEVEL_VERBOSE);
}

// A socket has been closed
void Plan9TraceLoggingProvider::PostAccept()
{
    LogMessage("PostAccept", TRACE_LEVEL_INFORMATION);
}

// An accept operation has been aborted
void Plan9TraceLoggingProvider::OperationAborted()
{
    LogMessage("OperationAborted", TRACE_LEVEL_VERBOSE);
}

// A client connected
void Plan9TraceLoggingProvider::ClientConnected(unsigned int connectionCount)
{
    LogMessage(std::format("ClientConnected, connectionCount={}", connectionCount), TRACE_LEVEL_VERBOSE);
}

// A client disconnected
void Plan9TraceLoggingProvider::ClientDisconnected(unsigned int connectionCount)
{
    LogMessage(std::format("ClientDisconnected, connectionCount={}", connectionCount), TRACE_LEVEL_VERBOSE);
}

// Adds the message name to the log message.
// N.B. This should be the first call on a new LogMessageBuilder.
void LogMessageBuilder::AddName(std::string_view name)
{
    m_message += name;
}

// Adds a string field to the message.
void LogMessageBuilder::AddField(std::string_view name, std::string_view value)
{
    AddFieldName(name);
    AddRawValue(value);
}

// Adds an unsigned integer field to the message.
void LogMessageBuilder::AddField(std::string_view name, UINT64 value, int base)
{
    AddFieldName(name);
    AddRawValue(value, base);
}

// Adds a qid field to the message.
void LogMessageBuilder::AddField(std::string_view name, const Qid& value)
{
    AddFieldName(name);
    AddRawValue(value);
}

// Adds a string value to the message.
void LogMessageBuilder::AddValue(std::string_view value)
{
    m_message += " ";
    AddRawValue(value);
}

// Adds a qid value to the message.
void LogMessageBuilder::AddValue(const Qid& value)
{
    m_message += " ";
    AddRawValue(value);
}

// Returns the message text as a string.
const char* LogMessageBuilder::String() const
{
    return m_message.c_str();
}

// Adds the name of the field, including separators.
void LogMessageBuilder::AddFieldName(std::string_view name)
{
    m_message += " ";
    m_message += name;
    m_message += "=";
}

// Adds an unsigned integer value without any separators or prefix.
void LogMessageBuilder::AddRawValue(UINT64 value, int base)
{
    char buffer[c_numberBufferSize]{};
    m_message += ConvertNumber(buffer, c_numberBufferSize, value, base);
}

// Adds a qid value without any separators or prefix.
void LogMessageBuilder::AddRawValue(const Qid& value)
{
    m_message += "{";
    AddRawValue(static_cast<UINT32>(value.Type), 16);
    m_message += ",";
    AddRawValue(value.Version);
    m_message += ",";
    AddRawValue(value.Path);
    m_message += "}";
}

// Adds a string value without any separators of prefix.
// N.B. This function does adds quotes surrounding the string.
void LogMessageBuilder::AddRawValue(std::string_view value)
{
    m_message += "\"";
    m_message.append(value.data(), value.size());
    m_message += "\"";
}

} // namespace p9fs
