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

using namespace wsl::windows::wslc::execution;

namespace wsl::windows::wslc {
// Diag Root Command
std::vector<std::unique_ptr<Command>> DiagCommand::GetCommands() const
{
    std::vector<std::unique_ptr<Command>> commands;
    commands.reserve(1);
    commands.push_back(std::make_unique<DiagListCommand>(FullName()));
    return commands;
}

std::vector<Argument> DiagCommand::GetArguments() const
{
    return {};
}

std::wstring DiagCommand::ShortDescription() const
{
    return {L"Diag command"};
}

std::wstring DiagCommand::LongDescription() const
{
    return {L"Diag command for demonstration purposes."};
}

void DiagCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    OutputHelp();
}
} // namespace wsl::windows::wslc
