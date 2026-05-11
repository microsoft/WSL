/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ContainerStatsCommand.cpp

Abstract:

    Implementation of stats command execution logic.

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
std::vector<Argument> ContainerStatsCommand::GetArguments() const
{
    return {
        Argument::Create(ArgType::ContainerId, false, NO_LIMIT),
        Argument::Create(ArgType::All),
        Argument::Create(ArgType::NoTrunc),
        Argument::Create(ArgType::Session),
    };
}

std::wstring ContainerStatsCommand::ShortDescription() const
{
    return Localization::WSLCCLI_ContainerStatsDesc();
}

std::wstring ContainerStatsCommand::LongDescription() const
{
    return Localization::WSLCCLI_ContainerStatsLongDesc();
}

void ContainerStatsCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    context              //
        << CreateSession //
        << ShowContainerStats;
}
} // namespace wsl::windows::wslc
