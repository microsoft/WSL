/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ImageTasks.h

Abstract:

    Declaration of image command execution tasks.

--*/
#pragma once
#include "CLIExecutionContext.h"

using wsl::windows::wslc::execution::CLIExecutionContext;

namespace wsl::windows::wslc::task {
void BuildImage(CLIExecutionContext& context);
void GetImages(CLIExecutionContext& context);
void ListImages(CLIExecutionContext& context);
void LoadImage(CLIExecutionContext& context);
void ImportImage(CLIExecutionContext& context);
void PullImage(CLIExecutionContext& context);
void PushImage(CLIExecutionContext& context);
void DeleteImage(CLIExecutionContext& context);
void InspectImages(CLIExecutionContext& context);
void TagImage(CLIExecutionContext& context);
void SaveImage(CLIExecutionContext& context);
void PruneImages(CLIExecutionContext& context);
} // namespace wsl::windows::wslc::task
