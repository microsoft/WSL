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
void StartContainer(CLIExecutionContext& context);
void SetCreateContainerOptionsFromArgs(CLIExecutionContext& context);
void SetRunContainerOptionsFromArgs(CLIExecutionContext& context);
void CreateContainer(CLIExecutionContext& context);
void RunContainer(CLIExecutionContext& context);
} // namespace wsl::windows::wslc::task
