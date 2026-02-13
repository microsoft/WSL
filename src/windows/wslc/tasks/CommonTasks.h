/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    CommonTasks.h

Abstract:

    Declaration of task execution interfaces.

--*/
#pragma once
#include "CLIExecutionContext.h"

using wsl::windows::wslc::execution::CLIExecutionContext;

namespace wsl::windows::wslc::task {
// Checks for SessionId and stores it in the context.
// Required Args: SessionId
// Inputs: None
// Outputs: None
void StoreSessionId(CLIExecutionContext& context);
} // namespace wsl::windows::wslc::task
