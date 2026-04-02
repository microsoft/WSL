/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    SessionListCommand.cpp

Abstract:

    Implementation of the session list command.

--*/
#include "CLIExecutionContext.h"
#include "SessionCommand.h"
#include "SessionTasks.h"
#include "Task.h"

using namespace wsl::windows::wslc::execution;
using namespace wsl::windows::wslc::task;
using namespace wsl::shared;

namespace wsl::windows::wslc {
// Session List Command
std::vector<Argument> SessionListCommand::GetArguments() const
{
    return {
        Argument::Create(ArgType::Verbose, std::nullopt, std::nullopt, Localization::WSLCCLI_SessionListVerboseArgDescription()),
    };
}

std::wstring SessionListCommand::ShortDescription() const
{
    return Localization::WSLCCLI_SessionListDesc();
}

std::wstring SessionListCommand::LongDescription() const
{
    return Localization::WSLCCLI_SessionListLongDesc();
}

void SessionListCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    context << ListSessions;
}
} // namespace wsl::windows::wslc
