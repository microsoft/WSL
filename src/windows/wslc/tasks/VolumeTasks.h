/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    VolumeTasks.h

Abstract:

    Declaration of volume command execution tasks.

--*/
#pragma once
#include "CLIExecutionContext.h"
#include "Task.h"

using wsl::windows::wslc::execution::CLIExecutionContext;

namespace wsl::windows::wslc::task {

void CreateVolume(CLIExecutionContext& context);
void DeleteVolumes(CLIExecutionContext& context);
void GetVolumes(CLIExecutionContext& context);
void InspectVolumes(CLIExecutionContext& context);
void ListVolumes(CLIExecutionContext& context);

} // namespace wsl::windows::wslc::task
