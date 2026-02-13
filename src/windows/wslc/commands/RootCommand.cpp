/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    RootCommand.cpp

Abstract:

    Implementation of the RootCommand, which is the root of all commands in the CLI.

--*/
#include "pch.h"
#include "RootCommand.h"
#include "TaskBase.h"

// Include all commands that parent to the root.
#include "ContainerCommand.h"
#include "ImageCommand.h"
#include "RegistryCommand.h"
#include "SessionCommand.h"
#include "VolumeCommand.h"
#include "DiagCommand.h"

using namespace wsl::shared;
using namespace wsl::windows::common::wslutil;
using namespace wsl::windows::wslc::execution;

namespace wsl::windows::wslc {
std::vector<std::unique_ptr<Command>> RootCommand::GetCommands() const
{
    std::vector<std::unique_ptr<Command>> commands;
    commands.reserve(15);
    commands.push_back(std::make_unique<ContainerCommand>(FullName()));
    commands.push_back(std::make_unique<ImageCommand>(FullName()));
    commands.push_back(std::make_unique<RegistryCommand>(FullName()));
    commands.push_back(std::make_unique<SessionCommand>(FullName()));
    commands.push_back(std::make_unique<VolumeCommand>(FullName()));
    commands.push_back(std::make_unique<DiagCommand>(FullName()));
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

std::vector<Argument> RootCommand::GetArguments() const
{
    return {
        Argument::Create(ArgType::Info),
    };
}

std::wstring_view RootCommand::ShortDescription() const
{
    return {L"WSLC is the Windows Subsystem for Linux Container CLI tool."};
}

std::wstring_view RootCommand::LongDescription() const
{
    return {
        L"WSLC is the Windows Subsystem for Linux Container CLI tool. It enables management and interaction with WSL containers "
        L"from the command line."};
}

void RootCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    OutputHelp();
}
} // namespace wsl::windows::wslc
