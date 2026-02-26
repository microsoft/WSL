/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ContainerLogsCommand.cpp

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
// Container Logs Command
std::vector<Argument> ContainerLogsCommand::GetArguments() const
{
    return {
        Argument::Create(ArgType::ContainerId, true),
        Argument::Create(ArgType::Session),
        Argument::Create(ArgType::Follow),
    };
}

std::wstring ContainerLogsCommand::ShortDescription() const
{
    return {L"View container logs"};
}

std::wstring ContainerLogsCommand::LongDescription() const
{
    return {L"View logs for a container."};
}

void ContainerLogsCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    context              //
        << CreateSession //
        << ViewContainerLogs;
}
} // namespace wsl::windows::wslc