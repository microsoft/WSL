/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    VolumeCommand.cpp

Abstract:

    Implementation of command execution logic.

--*/
#include "CLIExecutionContext.h"
#include "VolumeCommand.h"

using namespace wsl::windows::wslc::execution;
using namespace wsl::shared;

namespace wsl::windows::wslc {
// Volume Root Command
std::vector<std::unique_ptr<Command>> VolumeCommand::GetCommands() const
{
    std::vector<std::unique_ptr<Command>> commands;
    commands.push_back(std::make_unique<VolumeCreateCommand>(FullName()));
    commands.push_back(std::make_unique<VolumeRemoveCommand>(FullName()));
    commands.push_back(std::make_unique<VolumeInspectCommand>(FullName()));
    commands.push_back(std::make_unique<VolumeListCommand>(FullName()));
    return commands;
}

std::vector<Argument> VolumeCommand::GetArguments() const
{
    return {};
}

std::wstring VolumeCommand::ShortDescription() const
{
    return Localization::WSLCCLI_VolumeCommandDesc();
}

std::wstring VolumeCommand::LongDescription() const
{
    return Localization::WSLCCLI_VolumeCommandLongDesc();
}

void VolumeCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    OutputHelp();
}
} // namespace wsl::windows::wslc
