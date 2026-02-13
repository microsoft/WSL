/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    DiagLogsCommand.cpp

Abstract:

    Implementation of the diag logs command.

--*/
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
// Diag Logs Command
std::vector<Argument> DiagLogsCommand::GetArguments() const
{
    return {
        Argument::Create(ArgType::ContainerId, true, std::nullopt, L"Name of the container"),
        Argument::Create(ArgType::Follow),
    };
}

std::wstring_view DiagLogsCommand::ShortDescription() const
{
    return {L"View container logs."};
}

std::wstring_view DiagLogsCommand::LongDescription() const
{
    return {L"Displays logs for a specified container."};
}

void DiagLogsCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    context << task::LogsCommand;
}
} // namespace wsl::windows::wslc