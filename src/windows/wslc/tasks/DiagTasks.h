// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once
#include "CLIExecutionContext.h"

using wsl::windows::wslc::execution::CLIExecutionContext;

namespace wsl::windows::wslc::task
{
    // Lists containers in the current session.
    // Required Args: None
    // Inputs: Verbose optional flag
    // Outputs: None
    void ListContainers(CLIExecutionContext& context);
}
