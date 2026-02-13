/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ContainerStopCommand.cpp

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
// Container Stop Command
std::vector<Argument> ContainerStopCommand::GetArguments() const
{
    return {
        Argument::Create(ArgType::ContainerId, std::nullopt, 10),
        Argument::Create(ArgType::All),
        Argument::Create(ArgType::SessionId),
        Argument::Create(ArgType::Signal, std::nullopt, std::nullopt, L"Signal to send (default: SIGTERM)"),
        Argument::Create(ArgType::Time),
    };
}

std::wstring_view ContainerStopCommand::ShortDescription() const
{
    return {L"Sends a signal to stop running containers."};
}

std::wstring_view ContainerStopCommand::LongDescription() const
{
    return {
        L"Sends a signal to stop running containers. Waits for a specified timout before issuing "
        "a SIGKILL. Stops all running containers if --all is used. Multiple space-separated container "
        "ids can be specified to stop the specified multiple containers. No containers are stopped if "
        "neither container-ids nor --all options are specified. "};
}

void ContainerStopCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    PrintMessage(L"Container Stop subcommand executing..", stdout);
}
} // namespace wsl::windows::wslc