// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once
#include "context.h"

using wsl::windows::wslc::execution::CLIExecutionContext;

namespace wsl::windows::wslc::workflow
{
    // Ensures that the process is running as admin.
    // Required Args: None
    // Inputs: None
    // Outputs: None
    void EnsureRunningAsAdmin(CLIExecutionContext& context);
}
