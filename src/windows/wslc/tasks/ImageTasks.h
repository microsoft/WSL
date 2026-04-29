/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ImageTasks.h

Abstract:

    Declaration of image command execution tasks.

--*/
#pragma once
#include "CLIExecutionContext.h"

namespace wsl::windows::wslc::task {
void BuildImage(wsl::windows::wslc::execution::CLIExecutionContext& context);
void GetImages(wsl::windows::wslc::execution::CLIExecutionContext& context);
void ListImages(wsl::windows::wslc::execution::CLIExecutionContext& context);
void LoadImage(wsl::windows::wslc::execution::CLIExecutionContext& context);
void PullImage(wsl::windows::wslc::execution::CLIExecutionContext& context);
void PushImage(wsl::windows::wslc::execution::CLIExecutionContext& context);
void DeleteImage(wsl::windows::wslc::execution::CLIExecutionContext& context);
void InspectImages(wsl::windows::wslc::execution::CLIExecutionContext& context);
void TagImage(wsl::windows::wslc::execution::CLIExecutionContext& context);
void SaveImage(wsl::windows::wslc::execution::CLIExecutionContext& context);
void PruneImages(wsl::windows::wslc::execution::CLIExecutionContext& context);
} // namespace wsl::windows::wslc::task
