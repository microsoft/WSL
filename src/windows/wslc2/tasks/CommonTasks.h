// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once
#include "ExecutionContext.h"

using wsl::windows::wslc::execution::CLIExecutionContext;

namespace wsl::windows::wslc::task
{
    // Ensures that the process is running as admin.
    // Required Args: None
    // Inputs: None
    // Outputs: None
    void EnsureRunningAsAdmin(CLIExecutionContext& context);

    // Checks for SessionId and stores it in the context.
    // Required Args: SessionId
    // Inputs: None
    // Outputs: None
    void StoreSessionId(CLIExecutionContext& context);
}
