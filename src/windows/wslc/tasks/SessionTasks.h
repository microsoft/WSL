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
void AttachToSession(CLIExecutionContext& context);
void CreateSession(CLIExecutionContext& context);
void ListSessions(CLIExecutionContext& context);
void TerminateSession(CLIExecutionContext& context);
} // namespace wsl::windows::wslc::task
