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
#include <optional>

namespace wsl::windows::wslc::execution {
// The context within which all commands execute.
// Contains arguments via Args.
struct CLIExecutionContext : public wsl::windows::common::ExecutionContext
{
    CLIExecutionContext() : wsl::windows::common::ExecutionContext(wsl::windows::common::Context::WslC)
    {
    }
    ~CLIExecutionContext() override = default;

    NON_COPYABLE(CLIExecutionContext);
    CLIExecutionContext(CLIExecutionContext&&) = default;
    CLIExecutionContext& operator=(CLIExecutionContext&&) = default;

    argument::ArgMap Args;

    // Map of data stored in the context.
    DataMap Data;

    // Process exit code set by tasks like Run/Exec. When set, CoreMain returns this
    // instead of the HRESULT, enabling `wslc run ... && echo success` patterns.
    std::optional<int> ExitCode;

    // Event signaled when the user presses Ctrl-C. Starts null; long-running operations
    // that support cancellation create it via CreateCancelEvent() before passing it to
    // COM APIs that accept a CancelEvent handle.
    wil::unique_event CancelEvent;

    HANDLE CreateCancelEvent()
    {
        WI_ASSERT(!CancelEvent);
        CancelEvent.create(wil::EventOptions::ManualReset);
        return CancelEvent.get();
    }
};
} // namespace wsl::windows::wslc::execution
