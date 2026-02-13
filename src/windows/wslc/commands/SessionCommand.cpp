/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    SessionCommand.cpp

Abstract:

    Implementation of command execution logic.

--*/
#include "pch.h"
#include "CLIExecutionContext.h"
#include "ExecutionContextData.h"
#include "SessionCommand.h"
#include "CommonTasks.h"
#include "TaskBase.h"

using namespace wsl::windows::common::wslutil;
using namespace wsl::windows::wslc::execution;
using namespace wsl::windows::wslc::task;

namespace wsl::windows::wslc {
// Session Root Command
std::vector<std::unique_ptr<Command>> SessionCommand::GetCommands() const
{
    std::vector<std::unique_ptr<Command>> commands;
    commands.reserve(1);
    commands.push_back(std::make_unique<SessionListCommand>(FullName()));
    return commands;
}

std::vector<Argument> SessionCommand::GetArguments() const
{
    return {};
}

std::wstring_view SessionCommand::ShortDescription() const
{
    return {L"Session command"};
}

std::wstring_view SessionCommand::LongDescription() const
{
    return {L"Session command"};
}

void SessionCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    PrintMessage(L"Session command executing..", stdout);
}
} // namespace wsl::windows::wslc
