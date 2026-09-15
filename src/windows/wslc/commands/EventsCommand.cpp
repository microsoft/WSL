/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    EventsCommand.cpp

Abstract:

    Implementation of command execution logic.

--*/

#include "EventsCommand.h"
#include "CLIExecutionContext.h"
#include "SessionTasks.h"
#include "Task.h"

using namespace wsl::shared;
using namespace wsl::windows::wslc::execution;
using namespace wsl::windows::wslc::task;

namespace wsl::windows::wslc {
std::vector<Argument> EventsCommand::GetArguments() const
{
    return {
        Argument::Create(ArgType::Since, {.Desc = Localization::WSLCCLI_EventsSinceArgDescription()}),
        Argument::Create(ArgType::Until, {.Desc = Localization::WSLCCLI_EventsUntilArgDescription()}),
        Argument::Create(ArgType::Filter, {.Limit = Limit::Unlimited}),
    };
}

std::wstring EventsCommand::ShortDescription() const
{
    return Localization::WSLCCLI_EventsDesc();
}

std::wstring EventsCommand::LongDescription() const
{
    return Localization::WSLCCLI_EventsLongDesc();
}

void EventsCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    context               //
        << ResolveSession //
        << StreamEvents;
}
} // namespace wsl::windows::wslc
