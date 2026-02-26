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
void GetImages(CLIExecutionContext& context);
void ListImages(CLIExecutionContext& context);
void PullImage(CLIExecutionContext& context);
} // namespace wsl::windows::wslc::task
