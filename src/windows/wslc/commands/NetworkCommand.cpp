/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    NetworkCommand.cpp

Abstract:

    Implementation of command execution logic.

--*/
#include "CLIExecutionContext.h"
#include "NetworkCommand.h"

using namespace wsl::windows::wslc::execution;
using namespace wsl::shared;

namespace wsl::windows::wslc {
// Network Root Command
std::vector<std::unique_ptr<Command>> NetworkCommand::GetCommands() const
{
    std::vector<std::unique_ptr<Command>> commands;
    commands.push_back(std::make_unique<NetworkCreateCommand>(FullName()));
    commands.push_back(std::make_unique<NetworkRemoveCommand>(FullName()));
    commands.push_back(std::make_unique<NetworkInspectCommand>(FullName()));
    commands.push_back(std::make_unique<NetworkListCommand>(FullName()));
    return commands;
}

std::vector<Argument> NetworkCommand::GetArguments() const
{
    return {};
}

std::wstring NetworkCommand::ShortDescription() const
{
    return Localization::WSLCCLI_NetworkCommandDesc();
}

std::wstring NetworkCommand::LongDescription() const
{
    return Localization::WSLCCLI_NetworkCommandLongDesc();
}

void NetworkCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    OutputHelp();
}
} // namespace wsl::windows::wslc
