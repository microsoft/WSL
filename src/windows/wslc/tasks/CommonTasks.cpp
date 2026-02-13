/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    CommonTasks.cpp

Abstract:

    Implementation of task execution logic.

--*/
#pragma once
#include "pch.h"
#include "CLIExecutionContext.h"
#include "TaskBase.h"
#include "CommonTasks.h"

using namespace wsl::windows::common;
using namespace wsl::windows::wslc::execution;

using namespace wsl::shared;

namespace wsl::windows::wslc::task {
// Sample of arg retrieval and storage of state into context Data.
void StoreSessionId(CLIExecutionContext& context)
{
    ////WSLC_LOG(Task, Verbose, << L"In StoreSessionId Testing...");
    if (context.Args.Contains(ArgType::SessionId))
    {
        auto sessionId = context.Args.Get<ArgType::SessionId>();

        ////WSLC_LOG(Task, Verbose, << L"Storing SessionId: " << sessionId);
        context.Data.Add<Data::SessionId>(sessionId);
    }
}
} // namespace wsl::windows::wslc::task
