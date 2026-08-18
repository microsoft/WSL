/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    VolumeTasks.h

Abstract:

    Declaration of volume command execution tasks.

--*/
#pragma once
#include "CLIExecutionContext.h"

namespace wsl::windows::wslc::task {

void CreateVolume(wsl::windows::wslc::execution::CLIExecutionContext& context);
void DeleteVolumes(wsl::windows::wslc::execution::CLIExecutionContext& context);
void GetVolumes(wsl::windows::wslc::execution::CLIExecutionContext& context);
void InspectVolumes(wsl::windows::wslc::execution::CLIExecutionContext& context);
void ListVolumes(wsl::windows::wslc::execution::CLIExecutionContext& context);
void PruneVolumes(wsl::windows::wslc::execution::CLIExecutionContext& context);
} // namespace wsl::windows::wslc::task
