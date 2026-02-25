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
void CreateContainer(CLIExecutionContext& context);
void CreateSession(CLIExecutionContext& context);
void DeleteContainers(CLIExecutionContext& context);
void ExecContainer(CLIExecutionContext& context);
void GetContainers(CLIExecutionContext& context);
void InspectContainers(CLIExecutionContext& context);
void KillContainers(CLIExecutionContext& context);
void ListContainers(CLIExecutionContext& context);
void RunContainer(CLIExecutionContext& context);
void SetContainerOptionsFromArgs(CLIExecutionContext& context);
void StartContainer(CLIExecutionContext& context);
void StopContainers(CLIExecutionContext& context);
} // namespace wsl::windows::wslc::task
