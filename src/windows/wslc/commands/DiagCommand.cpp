/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    DiagCommand.cpp

Abstract:

    Implementation of DiagCommand command tree.

--*/
#pragma once
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
    return InitializeFromMoveOnly<std::vector<std::unique_ptr<Command>>>({
        std::make_unique<DiagAttachCommand>(FullName()),
        std::make_unique<DiagBuildCommand>(FullName()),
        std::make_unique<DiagListCommand>(FullName()),
        std::make_unique<DiagLogsCommand>(FullName()),
        std::make_unique<DiagPullCommand>(FullName()),
        std::make_unique<DiagRunCommand>(FullName()),
        std::make_unique<DiagShellCommand>(FullName()),
    });
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
