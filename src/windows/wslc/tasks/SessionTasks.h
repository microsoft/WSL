/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    SessionTasks.h

Abstract:

    Declaration of session command execution tasks.

--*/
#pragma once
#include "CLIExecutionContext.h"

using wsl::windows::wslc::execution::CLIExecutionContext;

namespace wsl::windows::wslc::task {
void AttachToSession(CLIExecutionContext& context);
void EnterSession(CLIExecutionContext& context);
void ListSessions(CLIExecutionContext& context);
void OpenDefaultSession(CLIExecutionContext& context);
void OpenSessionIfSpecified(CLIExecutionContext& context);
void ResolveSession(CLIExecutionContext& context);
void RunInSession(CLIExecutionContext& context);
void TerminateSession(CLIExecutionContext& context);
} // namespace wsl::windows::wslc::task
