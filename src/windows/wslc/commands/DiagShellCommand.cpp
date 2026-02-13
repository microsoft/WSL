/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    DiagShellCommand.cpp

Abstract:

    Implementation of the diag shell command.

--*/
#pragma once
#include "pch.h"
#include "CLIExecutionContext.h"
#include "ExecutionContextData.h"
#include "DiagCommand.h"
#include "DiagTasks.h"
#include "TaskBase.h"

using namespace wsl::windows::common::wslutil;
using namespace wsl::windows::wslc::execution;
using namespace wsl::windows::wslc::task;

namespace wsl::windows::wslc {
// Diag Shell Command
std::vector<Argument> DiagShellCommand::GetArguments() const
{
    return {
        Argument::Create(ArgType::SessionId, true, std::nullopt, L"Name of the session to attach the shell to."),
        Argument::Create(ArgType::Verbose, std::nullopt, std::nullopt, L"Show detailed information about the shell process."),
    };
}

std::wstring_view DiagShellCommand::ShortDescription() const
{
    return {L"Open a shell in a container."};
}

std::wstring_view DiagShellCommand::LongDescription() const
{
    return {L"Opens a shell in a specified container."};
}

void DiagShellCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    context << task::RunShellCommand;
}
} // namespace wsl::windows::wslc