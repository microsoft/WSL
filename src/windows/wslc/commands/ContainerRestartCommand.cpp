/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ContainerRestartCommand.cpp

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
// Container Restart Command
std::vector<Argument> ContainerRestartCommand::GetArguments() const
{
    return {
        Argument::Create(ArgType::ContainerId, std::nullopt, NO_LIMIT),
        Argument::Create(ArgType::Signal),
        Argument::Create(ArgType::Time),
    };
}

std::wstring ContainerRestartCommand::ShortDescription() const
{
    return Localization::WSLCCLI_ContainerRestartDesc();
}

std::wstring ContainerRestartCommand::LongDescription() const
{
    return Localization::WSLCCLI_ContainerRestartLongDesc();
}

// clang-format off
void ContainerRestartCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    context
        << ResolveSession
        << RestartContainers;
}
// clang-format on
} // namespace wsl::windows::wslc
