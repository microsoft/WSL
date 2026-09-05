// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include "CLIExecutionContext.h"

using wsl::windows::wslc::execution::CLIExecutionContext;

namespace wsl::windows::wslc::task {

void AttachCompose(CLIExecutionContext& Context);
void CreateCompose(CLIExecutionContext& Context);
void GetComposeProjects(CLIExecutionContext& Context);
void ListComposeProjects(CLIExecutionContext& Context);
void StartCompose(CLIExecutionContext& Context);
void StopCompose(CLIExecutionContext& Context);
void UpCompose(CLIExecutionContext& Context);

} // namespace wsl::windows::wslc::task
