// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include "CLIExecutionContext.h"

using wsl::windows::wslc::execution::CLIExecutionContext;

namespace wsl::windows::wslc::task {

void AttachCompose(CLIExecutionContext& context);
void CreateCompose(CLIExecutionContext& context);
void GetComposeProjects(CLIExecutionContext& context);
void ListComposeProjects(CLIExecutionContext& context);
void RemoveCompose(CLIExecutionContext& context);
void StartCompose(CLIExecutionContext& context);
void StopCompose(CLIExecutionContext& context);
void UpCompose(CLIExecutionContext& context);

} // namespace wsl::windows::wslc::task
