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

namespace wsl::windows::wslc {
// Container Start Command
std::vector<Argument> ContainerStartCommand::GetArguments() const
{
    return {
        Argument::Create(ArgType::ContainerId, true),
        Argument::Create(ArgType::Attach),      // NYI
        Argument::Create(ArgType::Interactive), // NYI
        Argument::Create(ArgType::Session),     // NYI
    };
}

std::wstring ContainerStartCommand::ShortDescription() const
{
    return {L"Start a container."};
}

std::wstring ContainerStartCommand::LongDescription() const
{
    return {
        L"Starts a container. Provides options to attach to the container's stdout and stderr streams and could be interactive "
        L"to keep stdin open."};
}
// clang-format off
void ContainerStartCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    context 
        << CreateSession
        << StartContainer;
}
// clang-format on
} // namespace wsl::windows::wslc
