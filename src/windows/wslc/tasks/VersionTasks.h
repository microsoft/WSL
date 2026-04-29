/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    VersionTasks.h

Abstract:

    Declaration of version command execution tasks.
--*/
#pragma once
#include "CLIExecutionContext.h"
#include "Task.h"

namespace wsl::windows::wslc::task {
using wsl::windows::wslc::execution::CLIExecutionContext;
void GetVersionInfo(CLIExecutionContext& context);
void ListVersionInfo(CLIExecutionContext& context);
} // namespace wsl::windows::wslc::task
