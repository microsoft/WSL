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
} // namespace wsl::windows::wslc::task
