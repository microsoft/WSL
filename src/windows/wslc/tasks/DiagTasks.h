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
void ListSessions(CLIExecutionContext& context);
} // namespace wsl::windows::wslc::task
