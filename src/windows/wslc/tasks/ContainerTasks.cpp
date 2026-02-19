/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ContainerTasks.cpp

Abstract:

    Implementation of container command related execution logic.

--*/
#include "Argument.h"
#include "CLIExecutionContext.h"
#include "ContainerModel.h"
#include "ContainerService.h"
#include "SessionModel.h"
#include "SessionService.h"
#include "Task.h"
#include "ContainerTasks.h"
#include <optional>
#include <wil/result_macros.h>

using namespace wsl::windows::wslc::execution;
using namespace wsl::windows::wslc::services;
using namespace wsl::windows::wslc::models;

namespace wsl::windows::wslc::task {
void CreateSession(CLIExecutionContext& context)
{
    std::optional<SessionOptions> options = std::nullopt;
    if (context.Args.Contains(ArgType::SessionId))
    {
        // TODO: Add session ID to the session options to open the specified session.
    }

    context.Data.Add<Data::Session>(SessionService::CreateSession(options));
}

void GetContainers(CLIExecutionContext& context)
{
    THROW_HR_IF(E_NOT_SET, !context.Data.Contains(Data::Session));
    auto& session = context.Data.Get<Data::Session>();
    context.Data.Add<Data::Containers>(ContainerService::List(session));
}
} // namespace wsl::windows::wslc::task
