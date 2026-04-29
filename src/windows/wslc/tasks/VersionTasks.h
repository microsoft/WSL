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
void GetVersionInfo(wsl::windows::wslc::execution::CLIExecutionContext& context);
void ListVersionInfo(wsl::windows::wslc::execution::CLIExecutionContext& context);
} // namespace wsl::windows::wslc::task
