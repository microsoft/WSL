/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    SessionShellCommand.cpp

Abstract:

    Implementation of the session shell command.

--*/
#include "CLIExecutionContext.h"
#include "SessionCommand.h"
#include "SessionTasks.h"
#include "Task.h"

using namespace wsl::windows::wslc::execution;
using namespace wsl::windows::wslc::task;
using namespace wsl::shared;

namespace wsl::windows::wslc {
// Session Shell Command
std::vector<Argument> SessionShellCommand::GetArguments() const
{
    return {
        Argument::Create(ArgType::SessionId),
    };
}

std::wstring SessionShellCommand::ShortDescription() const
{
    return Localization::WSLCCLI_SessionShellDesc();
}

std::wstring SessionShellCommand::LongDescription() const
{
    return Localization::WSLCCLI_SessionShellLongDesc();
}

void SessionShellCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    context << AttachToSession;
}
} // namespace wsl::windows::wslc
