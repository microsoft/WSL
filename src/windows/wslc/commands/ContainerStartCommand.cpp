/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ContainerStartCommand.cpp

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
// Container Start Command
std::vector<Argument> ContainerStartCommand::GetArguments() const
{
    return {
        Argument::Create(ArgType::ContainerId, true),
        Argument::Create(ArgType::Attach),
        Argument::Create(ArgType::Interactive), // NYI
        Argument::Create(ArgType::Session),     // NYI
    };
}

std::wstring ContainerStartCommand::ShortDescription() const
{
    return Localization::WSLCCLI_ContainerStartDesc();
}

std::wstring ContainerStartCommand::LongDescription() const
{
    return Localization::WSLCCLI_ContainerStartLongDesc();
}

void ContainerStartCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    context << CreateSession << StartContainer;
}
} // namespace wsl::windows::wslc
