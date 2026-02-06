// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once
#include "CLIExecutionContext.h"

using wsl::windows::wslc::execution::CLIExecutionContext;

namespace wsl::windows::wslc::task
{
    // Prints the NinjaCat
    // Required Args: TestArg
    // Inputs: None
    // Outputs: None
    void OutputNinjaCat(CLIExecutionContext& context);
}
