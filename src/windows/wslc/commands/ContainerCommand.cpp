/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ContainerCommand.cpp

Abstract:

    Implementation of command execution logic.

--*/
#include "CLIExecutionContext.h"
#include "ContainerCommand.h"

using namespace wsl::windows::wslc::execution;

namespace wsl::windows::wslc {
// Container Root Command
std::vector<std::unique_ptr<Command>> ContainerCommand::GetCommands() const
{
    std::vector<std::unique_ptr<Command>> commands;
    commands.push_back(std::make_unique<ContainerAttachCommand>(FullName()));
    commands.push_back(std::make_unique<ContainerCreateCommand>(FullName()));
    commands.push_back(std::make_unique<ContainerExecCommand>(FullName()));
    commands.push_back(std::make_unique<ContainerInspectCommand>(FullName()));
    commands.push_back(std::make_unique<ContainerKillCommand>(FullName()));
    commands.push_back(std::make_unique<ContainerLogsCommand>(FullName()));
    commands.push_back(std::make_unique<ContainerListCommand>(FullName()));
    commands.push_back(std::make_unique<ContainerRemoveCommand>(FullName()));
    commands.push_back(std::make_unique<ContainerRunCommand>(FullName()));
    commands.push_back(std::make_unique<ContainerStartCommand>(FullName()));
    commands.push_back(std::make_unique<ContainerStopCommand>(FullName()));
    return commands;
}

std::vector<Argument> ContainerCommand::GetArguments() const
{
    return {};
}

std::wstring ContainerCommand::ShortDescription() const
{
    return {L"Manage containers."};
}

std::wstring ContainerCommand::LongDescription() const
{
    return {L"Manage the lifecycle of WSL containers, including creating, starting, stopping, and removing them."};
}

void ContainerCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    OutputHelp();
}
} // namespace wsl::windows::wslc