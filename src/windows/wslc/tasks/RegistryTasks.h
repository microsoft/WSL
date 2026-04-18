/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    RegistryTasks.h

Abstract:

    Declaration of registry command execution tasks.

--*/
#pragma once
#include "CLIExecutionContext.h"

using wsl::windows::wslc::execution::CLIExecutionContext;

namespace wsl::windows::wslc::task {
void Login(CLIExecutionContext& context);
void Logout(CLIExecutionContext& context);
} // namespace wsl::windows::wslc::task
