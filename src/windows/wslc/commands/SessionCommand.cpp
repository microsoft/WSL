/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    SessionCommand.cpp

Abstract:

    Implementation of SessionCommand command tree.

--*/
#include "CLIExecutionContext.h"
#include "ExecutionContextData.h"
#include "SessionCommand.h"

using namespace wsl::windows::wslc::execution;

namespace wsl::windows::wslc {
// Session Root Command
std::vector<std::unique_ptr<Command>> SessionCommand::GetCommands() const
{
    std::vector<std::unique_ptr<Command>> commands;
    commands.reserve(2);
    commands.push_back(std::make_unique<SessionListCommand>(FullName()));
    commands.push_back(std::make_unique<SessionShellCommand>(FullName()));
    return commands;
}

std::vector<Argument> SessionCommand::GetArguments() const
{
    return {};
}

std::wstring SessionCommand::ShortDescription() const
{
    return {L"Session command"};
}

std::wstring SessionCommand::LongDescription() const
{
    return {L"Session command."};
}

void SessionCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    OutputHelp();
}
} // namespace wsl::windows::wslc
