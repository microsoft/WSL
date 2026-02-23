/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    RootCommand.cpp

Abstract:

    Implementation of the RootCommand, which is the root of all commands in the CLI.

--*/
#include "RootCommand.h"

// Include all commands that parent to the root.
#include "ContainerCommand.h"
#include "DiagCommand.h"

using namespace wsl::windows::wslc::execution;

namespace wsl::windows::wslc {
std::vector<std::unique_ptr<Command>> RootCommand::GetCommands() const
{
    std::vector<std::unique_ptr<Command>> commands;
    commands.reserve(3);
    commands.push_back(std::make_unique<ContainerCommand>(FullName()));
    commands.push_back(std::make_unique<DiagCommand>(FullName()));
    commands.push_back(std::make_unique<ContainerListCommand>(FullName()));
    return commands;
}

std::vector<Argument> RootCommand::GetArguments() const
{
    return {
        Argument::Create(ArgType::Info),
    };
}

std::wstring RootCommand::ShortDescription() const
{
    return {L"WSLC is the Windows Subsystem for Linux Container CLI tool."};
}

std::wstring RootCommand::LongDescription() const
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
