/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    SessionTerminateCommand.cpp

Abstract:

    Implementation of the session terminate command.

--*/
#include "CLIExecutionContext.h"
#include "SessionCommand.h"
#include "SessionTasks.h"
#include "Task.h"

using namespace wsl::windows::wslc::execution;
using namespace wsl::windows::wslc::task;

namespace wsl::windows::wslc {
// Session Terminate Command
std::vector<Argument> SessionTerminateCommand::GetArguments() const
{
    return {
        Argument::Create(ArgType::SessionId),
    };
}

std::wstring SessionTerminateCommand::ShortDescription() const
{
    return {L"Terminate a session."};
}

std::wstring SessionTerminateCommand::LongDescription() const
{
    return {L"Terminates an active session. If no session is specified, the default session will be terminated."};
}

void SessionTerminateCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    context << TerminateSession;
}
} // namespace wsl::windows::wslc