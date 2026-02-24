/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ContainerTasks.h

Abstract:

    Declaration of container command execution tasks.

--*/
#pragma once
#include "CLIExecutionContext.h"

using wsl::windows::wslc::execution::CLIExecutionContext;

namespace wsl::windows::wslc::task {
void CreateSession(CLIExecutionContext& context);
void GetContainers(CLIExecutionContext& context);
} // namespace wsl::windows::wslc::task
