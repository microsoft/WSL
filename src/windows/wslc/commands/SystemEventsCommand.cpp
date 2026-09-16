/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    SystemEventsCommand.cpp

Abstract:

    Implementation of command execution logic.

--*/
#include "CLIExecutionContext.h"
#include "SessionTasks.h"
#include "SystemCommand.h"
#include "Task.h"

using namespace wsl::shared;
using namespace wsl::windows::wslc::execution;
using namespace wsl::windows::wslc::task;

namespace wsl::windows::wslc {
std::vector<Argument> SystemEventsCommand::GetArguments() const
{
    return {
        Argument::Create(ArgType::Since, {.Desc = Localization::WSLCCLI_EventsSinceArgDescription()}),
        Argument::Create(ArgType::Until, {.Desc = Localization::WSLCCLI_EventsUntilArgDescription()}),
        Argument::Create(ArgType::EventFilter, {.Limit = Limit::Unlimited}),
    };
}

std::wstring SystemEventsCommand::ShortDescription() const
{
    return Localization::WSLCCLI_EventsDesc();
}

std::wstring SystemEventsCommand::LongDescription() const
{
    return Localization::WSLCCLI_EventsLongDesc();
}

void SystemEventsCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    context               //
        << ResolveSession //
        << StreamEvents;
}
} // namespace wsl::windows::wslc
