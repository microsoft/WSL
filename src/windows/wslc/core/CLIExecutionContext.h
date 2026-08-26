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

    // Central output terminal for all user-facing status messages.
    Terminal Terminal;

    // Process exit code set by tasks like Run/Exec.
    std::optional<int> ExitCode;

    // Event signaled when the user presses Ctrl-C.
    wil::unique_event CancelEvent;

    HANDLE CreateCancelEvent();

    // Returns true when a destructive prune may proceed, either because --force was passed or
    // because the user accepted the confirmation prompt. End of input declines, so a prune with no
    // interactive input aborts rather than blocking.
    bool ConfirmPrune(std::wstring_view warning);

    // Applies and freezes environment-only global options before command-line parsing reports errors.
    void ApplyGlobalEnvironmentOptions();
};

} // namespace wsl::windows::wslc::execution
