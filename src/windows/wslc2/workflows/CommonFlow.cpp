// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once
#include "pch.h"
#include "util.h"
#include "context.h"
#include "WorkflowBase.h"
#include "CommonFlow.h"

using namespace wsl::windows::wslc::execution;

namespace wsl::windows::wslc::workflow
{

    void EnsureRunningAsAdmin(CLIExecutionContext& context)
    {
        if (!util::IsRunningAsAdmin())
        {
            WSLC_TERMINATE_CONTEXT(WSLC_CLI_ERROR_COMMAND_REQUIRES_ADMIN);
        }
    }
}