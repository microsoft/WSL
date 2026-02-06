// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once
#include "pch.h"
#include "SystemUtilities.h"
#include "CLIExecutionContext.h"
#include "TaskBase.h"
#include "CommonTasks.h"


using namespace wsl::windows::common;
using namespace wsl::windows::wslc::execution;
using namespace wsl::windows::wslc::logging;

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

    void StoreSessionId(CLIExecutionContext& context)
    {;
        WSLC_LOG(Task, Verbose, << L"In StoreSessionId Testing...");
        if (context.Args.Contains(ArgType::SessionId))
        {
            context.Data.Add<Data::SessionId>(std::wstring{context.Args.Get<ArgType::SessionId>()});
        }
    }
}