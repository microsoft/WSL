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
using namespace wsl::shared;

namespace wsl::windows::wslc {
// Container Logs Command
std::vector<Argument> ContainerLogsCommand::GetArguments() const
{
    return {
        Argument::Create(ArgType::ContainerId, true),
        Argument::Create(ArgType::Session),
        Argument::Create(ArgType::Follow),
        Argument::Create(ArgType::Tail),
        Argument::Create(ArgType::Since),
        Argument::Create(ArgType::Until),
        Argument::Create(ArgType::Timestamps),
    };
}

std::wstring ContainerLogsCommand::ShortDescription() const
{
    return Localization::WSLCCLI_ContainerLogsDesc();
}

std::wstring ContainerLogsCommand::LongDescription() const
{
    return Localization::WSLCCLI_ContainerLogsLongDesc();
}

void ContainerLogsCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    context              //
        << CreateSession //
        << ViewContainerLogs;
}
} // namespace wsl::windows::wslc