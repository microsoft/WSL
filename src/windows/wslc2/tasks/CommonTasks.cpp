// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once
#include "pch.h"
#include "util.h"
#include "context.h"
#include "TaskBase.h"
#include "CommonTasks.h"

using namespace wsl::windows::common;
using namespace wsl::windows::wslc::execution;

namespace wsl::windows::wslc::task
{

    void EnsureRunningAsAdmin(CLIExecutionContext& context)
    {
        if (!util::IsRunningAsAdmin())
        {
            wslutil::PrintMessage(Localization::WSLCCLI_CommandRequiresAdmin(), stderr);
            WSLC_TERMINATE_CONTEXT(WSLC_CLI_ERROR_COMMAND_REQUIRES_ADMIN);
        }
    }
}