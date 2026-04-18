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

using wsl::windows::wslc::execution::CLIExecutionContext;

namespace wsl::windows::wslc::task {
void GetVersionInfo(CLIExecutionContext& context);
void ListVersionInfo(CLIExecutionContext& context);
} // namespace wsl::windows::wslc::task
