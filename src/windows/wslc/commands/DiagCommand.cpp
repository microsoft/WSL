/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    DiagCommand.cpp

Abstract:

    Implementation of DiagCommand command tree.

--*/
#include "pch.h"
#include "CLIExecutionContext.h"
#include "ExecutionContextData.h"
#include "DiagCommand.h"
#include "CommonTasks.h"
#include "TaskBase.h"

using namespace wsl::windows::common::wslutil;
using namespace wsl::windows::wslc::execution;
using namespace wsl::windows::wslc::task;

namespace wsl::windows::wslc {
// Diag Root Command
std::vector<std::unique_ptr<Command>> DiagCommand::GetCommands() const
{
    std::vector<std::unique_ptr<Command>> commands;
    commands.reserve(7);
    commands.push_back(std::make_unique<DiagAttachCommand>(FullName()));
    commands.push_back(std::make_unique<DiagBuildCommand>(FullName()));
    commands.push_back(std::make_unique<DiagListCommand>(FullName()));
    commands.push_back(std::make_unique<DiagLogsCommand>(FullName()));
    commands.push_back(std::make_unique<DiagPullCommand>(FullName()));
    commands.push_back(std::make_unique<DiagRunCommand>(FullName()));
    commands.push_back(std::make_unique<DiagShellCommand>(FullName()));
    return commands;
}

std::vector<Argument> DiagCommand::GetArguments() const
{
    return {};
}

std::wstring_view DiagCommand::ShortDescription() const
{
    return {L"Diag command"};
}

std::wstring_view DiagCommand::LongDescription() const
{
    return {L"Diag command for demonstration purposes."};
}

void DiagCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    PrintMessage(L"Diag base command executing..", stdout);
}
} // namespace wsl::windows::wslc
