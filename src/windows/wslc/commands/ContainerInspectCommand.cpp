/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ContainerInspectCommand.cpp

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
// Container Inspect Command
std::vector<Argument> ContainerInspectCommand::GetArguments() const
{
    return {
        Argument::Create(ArgType::ContainerId, true, NO_LIMIT),
        Argument::Create(ArgType::Session),
    };
}

std::wstring ContainerInspectCommand::ShortDescription() const
{
    return Localization::WSLCCLI_ContainerInspectDesc();
}

std::wstring ContainerInspectCommand::LongDescription() const
{
    return Localization::WSLCCLI_ContainerInspectLongDesc();
}

// clang-format off
void ContainerInspectCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    context
        << CreateSession
        << InspectContainers;
}
// clang-format on
} // namespace wsl::windows::wslc