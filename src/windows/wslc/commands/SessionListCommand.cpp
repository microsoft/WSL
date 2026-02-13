/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    SessionListCommand.cpp

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
// Session List Command
std::vector<Argument> SessionListCommand::GetArguments() const
{
    return {};
}

std::wstring_view SessionListCommand::ShortDescription() const
{
    return {L"Lists sessions."};
}

std::wstring_view SessionListCommand::LongDescription() const
{
    return {L"Lists sessions."};
}

void SessionListCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    PrintMessage(L"Session List subcommand executing..", stdout);
}
} // namespace wsl::windows::wslc
