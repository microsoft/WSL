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

namespace wsl::windows::wslc::execution {
// The context within which all commands execute.
// Contains arguments via Args.
struct CLIExecutionContext : public wsl::windows::common::ExecutionContext
{
    CLIExecutionContext() : wsl::windows::common::ExecutionContext(wsl::windows::common::Context::WslC)
    {
    }
    ~CLIExecutionContext() override = default;

    CLIExecutionContext(const CLIExecutionContext&) = default;
    CLIExecutionContext& operator=(const CLIExecutionContext&) = default;

    CLIExecutionContext(CLIExecutionContext&&) = default;
    CLIExecutionContext& operator=(CLIExecutionContext&&) = default;

    argument::ArgMap Args;

    // Map of data stored in the context.
    DataMap Data;
};
} // namespace wsl::windows::wslc::execution
