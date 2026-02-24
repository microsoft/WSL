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
    commands.reserve(5);
    commands.push_back(std::make_unique<ContainerListCommand>(FullName()));
    commands.push_back(std::make_unique<ContainerCreateCommand>(FullName()));
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
    return {L"Container command"};
}

std::wstring ContainerCommand::LongDescription() const
{
    return {L"Container command for demonstration purposes."};
}

void ContainerCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    OutputHelp();
}
} // namespace wsl::windows::wslc