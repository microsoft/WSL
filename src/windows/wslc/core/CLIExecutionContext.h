/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    CLIExecutionContext.h

Abstract:

    Declaration of CLI execution context.

--*/
#pragma once
#include "ArgMap.h"
#include "ExecutionContextData.h"
#include "Terminal.h"
#include "WSLCEvent.h"
#include "WslTelemetry.h"
#include <optional>

namespace wsl::windows::wslc::execution {

struct CLIExecutionContext : public wsl::windows::common::ExecutionContext
{
    CLIExecutionContext() : wsl::windows::common::ExecutionContext(wsl::windows::common::Context::WslC)
    {
    }
    CLIExecutionContext(FILE* outFile, bool outVtEnabled, FILE* errFile, bool errVtEnabled) :
        wsl::windows::common::ExecutionContext(wsl::windows::common::Context::WslC), Terminal(outFile, outVtEnabled, errFile, errVtEnabled)
    {
    }
    ~CLIExecutionContext() override = default;

    NON_COPYABLE(CLIExecutionContext);
    NON_MOVABLE(CLIExecutionContext);

    // Arguments accumulated from the selected command path.
    argument::ArgMap Args;

    // Map of data stored in the context.
    DataMap Data;

    // Central output terminal for all user-facing status messages.
    Terminal Terminal;

    // Process exit code set by tasks like Run/Exec.
    std::optional<int> ExitCode;

    // Event signaled when the user presses Ctrl-C.
    wil::unique_event CancelEvent;

    HANDLE CreateCancelEvent();

    // Applies terminal configuration from parsed arguments and freezes those values for the invocation.
    void ApplyTerminalOptions();

    // Prints a caught error to stderr.
    void ReportError(HRESULT result);

    // Drops the collected error so a later failure in the same invocation reports its own message.
    void ClearError();
};

} // namespace wsl::windows::wslc::execution

// CLI events are always available through TraceLogging and are mirrored to stderr
// when debug output is enabled. Message arguments are evaluated once when either sink is enabled.
#define WSLC_CLI_EVENT(Context, Name, Format, ...) \
    do \
    { \
        auto&& _wslcDebugContext = (Context); \
        const bool _wslcDebugTraceEnabled = \
            g_hTraceLoggingProvider != nullptr && TraceLoggingProviderEnabled(g_hTraceLoggingProvider, WINEVENT_LEVEL_VERBOSE, 0); \
        const bool _wslcDebugOutputEnabled = _wslcDebugContext.Terminal.IsDebugEnabled(); \
        ::wsl::windows::wslc::events::Dispatch( \
            _wslcDebugTraceEnabled, \
            _wslcDebugOutputEnabled, \
            [&]() { return ::wsl::windows::wslc::events::FormatMessage((Format), __VA_ARGS__); }, \
            [&](const std::wstring& _wslcDebugMessage) { \
                WSL_LOG( \
                    Name, TraceLoggingLevel(WINEVENT_LEVEL_VERBOSE), TraceLoggingWideString(_wslcDebugMessage.c_str(), "Message")); \
            }, \
            [&](const std::wstring& _wslcDebugMessage) { _wslcDebugContext.Terminal.Debug(_wslcDebugMessage); }); \
    } while (false)
