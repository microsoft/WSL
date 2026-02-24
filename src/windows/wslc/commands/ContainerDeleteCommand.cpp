/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ContainerDeleteCommand.cpp

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
// Container Delete Command
std::vector<Argument> ContainerDeleteCommand::GetArguments() const
{
    return {
        Argument::Create(ArgType::ContainerId, std::nullopt, NO_LIMIT),
        Argument::Create(ArgType::Force),
        Argument::Create(ArgType::Session),
    };
}

std::wstring ContainerDeleteCommand::ShortDescription() const
{
    return {L"Delete containers"};
}

std::wstring ContainerDeleteCommand::LongDescription() const
{
    return {L"Deletes containers."};
}

// clang-format off
void ContainerDeleteCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    context 
        << CreateSession
        << DeleteContainers;
}
// clang-format on
} // namespace wsl::windows::wslc