/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ContainerRemoveCommand.cpp

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
// Container Remove Command
std::vector<Argument> ContainerRemoveCommand::GetArguments() const
{
    return {
        Argument::Create(ArgType::ContainerId, true, NO_LIMIT),
        Argument::Create(ArgType::Force),
        Argument::Create(ArgType::Session),
    };
}

std::wstring ContainerRemoveCommand::ShortDescription() const
{
    return Localization::WSLCCLI_ContainerRemoveDesc();
}

std::wstring ContainerRemoveCommand::LongDescription() const
{
    return Localization::WSLCCLI_ContainerRemoveLongDesc();
}

void ContainerRemoveCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    context              //
        << CreateSession //
        << RemoveContainers;
}
} // namespace wsl::windows::wslc