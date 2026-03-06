/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ContainerAttachCommand.cpp

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
// Container Attach Command
std::vector<Argument> ContainerAttachCommand::GetArguments() const
{
    return {
        Argument::Create(ArgType::ContainerId, true),
        Argument::Create(ArgType::Session),
    };
}

std::wstring ContainerAttachCommand::ShortDescription() const
{
    return {L"Attach to a container."};
}

std::wstring ContainerAttachCommand::LongDescription() const
{
    return {L"Attaches to a container."};
}

void ContainerAttachCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    context              //
        << CreateSession //
        << AttachContainer(context.Args.Get<ArgType::ContainerId>());
}
} // namespace wsl::windows::wslc
