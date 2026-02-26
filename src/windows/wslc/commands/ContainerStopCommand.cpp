/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ContainerStopCommand.cpp

Abstract:

    Implementation of command execution logic.

--*/

#include "ContainerCommand.h"
#include "CLIExecutionContext.h"
#include "ContainerTasks.h"
#include "SessionTasks.h"
#include "Task.h"

using namespace wsl::windows::wslc::execution;
using namespace wsl::windows::wslc::task;

namespace wsl::windows::wslc {
// Container Stop Command
std::vector<Argument> ContainerStopCommand::GetArguments() const
{
    return {
        Argument::Create(ArgType::ContainerId, std::nullopt, NO_LIMIT),
        Argument::Create(ArgType::Session),
        Argument::Create(ArgType::Signal, std::nullopt, std::nullopt, L"Signal to send (default: SIGTERM)"),
        Argument::Create(ArgType::Time),
    };
}

std::wstring ContainerStopCommand::ShortDescription() const
{
    return {L"Stop containers"};
}

std::wstring ContainerStopCommand::LongDescription() const
{
    return {L"Stops containers."};
}

// clang-format off
void ContainerStopCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    context
        << CreateSession
        << StopContainers;
}
// clang-format on
} // namespace wsl::windows::wslc