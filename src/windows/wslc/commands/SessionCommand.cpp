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
using namespace wsl::shared;

namespace wsl::windows::wslc {
// Session Root Command
std::vector<std::unique_ptr<Command>> SessionCommand::GetCommands() const
{
    std::vector<std::unique_ptr<Command>> commands;
    commands.push_back(std::make_unique<SessionEnterCommand>(FullName()));
    commands.push_back(std::make_unique<SessionListCommand>(FullName()));
    commands.push_back(std::make_unique<SessionShellCommand>(FullName()));
    commands.push_back(std::make_unique<SessionTerminateCommand>(FullName()));
    return commands;
}

std::vector<Argument> SessionCommand::GetArguments() const
{
    return {};
}

std::wstring SessionCommand::ShortDescription() const
{
    return Localization::WSLCCLI_SessionCommandDesc();
}

std::wstring SessionCommand::LongDescription() const
{
    return Localization::WSLCCLI_SessionCommandLongDesc();
}

void SessionCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    OutputHelp();
}
} // namespace wsl::windows::wslc
