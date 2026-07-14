/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    CLIExecutionContext.h

Abstract:

    Declaration of CLI execution context.

--*/
#pragma once
#include "ArgumentTypes.h"
#include "ExecutionContextData.h"
#include "Reporter.h"
#include <optional>

namespace wsl::windows::wslc::execution {

struct CLIExecutionContext : public wsl::windows::common::ExecutionContext
{
    CLIExecutionContext() : wsl::windows::common::ExecutionContext(wsl::windows::common::Context::WslC)
    {
    }
    ~CLIExecutionContext() override = default;

    NON_COPYABLE(CLIExecutionContext);
    NON_MOVABLE(CLIExecutionContext);

    // Per-subcommand arguments parsed by the resolved leaf Command.
    argument::ArgMap Args;

    // Global options parsed from tokens that appear before any subcommand
    // (e.g. `wslc <global-option> image list`). Populated early in CoreMain.
    argument::ArgMap GlobalArgs;

    // Map of data stored in the context.
    DataMap Data;

    // Central output reporter for all user-facing status messages.
    Reporter Reporter;

    // Process exit code set by tasks like Run/Exec.
    std::optional<int> ExitCode;

    // Event signaled when the user presses Ctrl-C.
    wil::unique_event CancelEvent;

    HANDLE CreateCancelEvent();

    // Single chokepoint that turns parsed GlobalArgs into process-wide effects
    // (debug logging, VT color, ...). Idempotent.
    void ApplyGlobalOptions();
};

} // namespace wsl::windows::wslc::execution
