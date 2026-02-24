/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ContainerKillCommand.cpp

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
// Container Kill Command
std::vector<Argument> ContainerKillCommand::GetArguments() const
{
    return {
        Argument::Create(ArgType::ContainerId, std::nullopt, NO_LIMIT),
        Argument::Create(ArgType::Session),
        Argument::Create(ArgType::Signal, std::nullopt, std::nullopt, L"Signal to send (default: SIGKILL)"),
    };
}

std::wstring ContainerKillCommand::ShortDescription() const
{
    return {L"Kill containers"};
}

std::wstring ContainerKillCommand::LongDescription() const
{
    return {L"Kills containers."};
}

// clang-format off
void ContainerKillCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    context
        << CreateSession
        << KillContainers;
}
// clang-format on
} // namespace wsl::windows::wslc