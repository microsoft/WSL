/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    SessionEnterCommand.cpp

Abstract:

    Implementation of the session enter command.

--*/
#include "CLIExecutionContext.h"
#include "SessionCommand.h"
#include "SessionTasks.h"
#include "Task.h"

using namespace wsl::windows::wslc::execution;
using namespace wsl::windows::wslc::task;

namespace wsl::windows::wslc {
// Session Enter Command
std::vector<Argument> SessionEnterCommand::GetArguments() const
{
    return {
        Argument::Create(ArgType::StoragePath, true),
        Argument::Create(ArgType::Name, std::nullopt, std::nullopt, L"Name for the session. If not provided, a GUID is generated."),
    };
}

std::wstring SessionEnterCommand::ShortDescription() const
{
    return {L"Enter a temporary session."};
}

std::wstring SessionEnterCommand::LongDescription() const
{
    return {L"Creates a non-persistent session with the given storage path and opens a shell into it. "
            L"The session is deleted when the shell exits. If no name is provided, a GUID is generated and printed to stderr."};
}

void SessionEnterCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    context << EnterSession;
}
} // namespace wsl::windows::wslc
