/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ContainerCommand.cpp

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
// Container Root Command
std::vector<std::unique_ptr<Command>> ContainerCommand::GetCommands() const
{
    std::vector<std::unique_ptr<Command>> commands;
    commands.reserve(9);
    commands.push_back(std::make_unique<ContainerCreateCommand>(FullName()));
    commands.push_back(std::make_unique<ContainerDeleteCommand>(FullName()));
    commands.push_back(std::make_unique<ContainerExecCommand>(FullName()));
    commands.push_back(std::make_unique<ContainerInspectCommand>(FullName()));
    commands.push_back(std::make_unique<ContainerKillCommand>(FullName()));
    commands.push_back(std::make_unique<ContainerListCommand>(FullName()));
    commands.push_back(std::make_unique<ContainerRunCommand>(FullName()));
    commands.push_back(std::make_unique<ContainerStartCommand>(FullName()));
    commands.push_back(std::make_unique<ContainerStopCommand>(FullName()));
    return commands;
}

std::vector<Argument> ContainerCommand::GetArguments() const
{
    return {};
}

std::wstring_view ContainerCommand::ShortDescription() const
{
    return {L"Container command"};
}

std::wstring_view ContainerCommand::LongDescription() const
{
    return {L"Container command for demonstration purposes."};
}

void ContainerCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    PrintMessage(L"Container base command executing..", stdout);
}
} // namespace wsl::windows::wslc
