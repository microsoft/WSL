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

// Renders the table left in Data::Table by a preceding task. Does nothing when no table was
// produced, which is the case for the quiet and JSON output formats.
void PrintTable(CLIExecutionContext& context);
} // namespace wsl::windows::wslc::task
