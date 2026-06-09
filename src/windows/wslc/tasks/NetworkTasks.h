/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    NetworkTasks.h

Abstract:

    Declaration of network command execution tasks.

--*/
#pragma once
#include "CLIExecutionContext.h"

namespace wsl::windows::wslc::task {

void CreateNetwork(wsl::windows::wslc::execution::CLIExecutionContext& context);
void DeleteNetworks(wsl::windows::wslc::execution::CLIExecutionContext& context);
void GetNetworks(wsl::windows::wslc::execution::CLIExecutionContext& context);
void InspectNetworks(wsl::windows::wslc::execution::CLIExecutionContext& context);
void ListNetworks(wsl::windows::wslc::execution::CLIExecutionContext& context);
} // namespace wsl::windows::wslc::task
