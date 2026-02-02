// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once
#include "context.h"

using namespace wsl::windows::wslc::execution;

namespace wsl::windows::wslc::workflow
{
    // Prints the NinjaCat
    // Required Args: TestArg
    // Inputs: None
    // Outputs: None
    void OutputNinjaCat(CLIExecutionContext& context);
}
