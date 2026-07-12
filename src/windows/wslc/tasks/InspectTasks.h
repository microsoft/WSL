/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    InspectTasks.h

Abstract:

    Declaration of inspection command execution tasks.

--*/
#pragma once
#include "CLIExecutionContext.h"
#include "Task.h"

using wsl::windows::wslc::execution::CLIExecutionContext;

namespace wsl::windows::wslc::task {
void Inspect(CLIExecutionContext& context);
} // namespace wsl::windows::wslc::task
