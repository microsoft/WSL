/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    CommonTasks.h

Abstract:

    Declaration of execution tasks shared by multiple commands.

--*/
#pragma once
#include "CLIExecutionContext.h"

using wsl::windows::wslc::execution::CLIExecutionContext;

namespace wsl::windows::wslc::task {
void ConfirmAction(CLIExecutionContext& context);

// Writes the output built by a preceding format task: Data::Json one line per entry, or the
// rendered Data::Table.
void PrintFormattedOutput(CLIExecutionContext& context);
} // namespace wsl::windows::wslc::task
