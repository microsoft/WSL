/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ContainerKillCommand.cpp

Abstract:

    Implementation of command execution logic.

--*/
#include "pch.h"
#include "CLIExecutionContext.h"
#include "ExecutionContextData.h"
#include "ContainerCommand.h"
#include "CommonTasks.h"
#include "TaskBase.h"

using namespace wsl::windows::common::wslutil;
using namespace wsl::windows::wslc::execution;
using namespace wsl::windows::wslc::task;

namespace wsl::windows::wslc {
// Container Kill Command
std::vector<Argument> ContainerKillCommand::GetArguments() const
{
    return {
        Argument::Create(ArgType::ContainerId, false, 10),
        Argument::Create(ArgType::All),
        Argument::Create(ArgType::SessionId),
        Argument::Create(ArgType::Signal),
    };
}

std::wstring_view ContainerKillCommand::ShortDescription() const
{
    return {L"Immediately kill containers."};
}

std::wstring_view ContainerKillCommand::LongDescription() const
{
    return {L"Sends SIGKILL (default option) to running containers to immediately kill the containers. "};
}

void ContainerKillCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    PrintMessage(L"Container Kill subcommand executing..", stdout);
}
} // namespace wsl::windows::wslc