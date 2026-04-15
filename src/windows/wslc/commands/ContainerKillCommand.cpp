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
using namespace wsl::shared;

namespace wsl::windows::wslc {
// Container Kill Command
std::vector<Argument> ContainerKillCommand::GetArguments() const
{
    return {
        Argument::Create(ArgType::ContainerId, true, NO_LIMIT),
        Argument::Create(ArgType::Session),
        Argument::Create(ArgType::Signal),
    };
}

std::wstring ContainerKillCommand::ShortDescription() const
{
    return Localization::WSLCCLI_ContainerKillDesc();
}

std::wstring ContainerKillCommand::LongDescription() const
{
    return Localization::WSLCCLI_ContainerKillLongDesc();
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