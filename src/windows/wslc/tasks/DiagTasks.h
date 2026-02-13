/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    DiagTasks.h

Abstract:

    Declaration of diag command execution tasks.

--*/
#pragma once
#include "CLIExecutionContext.h"

using wsl::windows::wslc::execution::CLIExecutionContext;

namespace wsl::windows::wslc::task {
void ListContainers(CLIExecutionContext& context);
void RunShellCommand(CLIExecutionContext& context);
void PullCommand(CLIExecutionContext& context);
void BuildCommand(CLIExecutionContext& context);
void LogsCommand(CLIExecutionContext& context);
void AttachCommand(CLIExecutionContext& context);
void RunCommand(CLIExecutionContext& context);
} // namespace wsl::windows::wslc::task
