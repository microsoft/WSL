/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    SystemCommand.cpp

Abstract:

    Definition of System command tree.

--*/
#include "CLIExecutionContext.h"
#include "SystemCommand.h"
#include "SessionCommand.h"

using namespace wsl::windows::wslc::execution;
using namespace wsl::shared;

namespace wsl::windows::wslc {
std::vector<std::unique_ptr<Command>> SystemCommand::GetCommands() const
{
    std::vector<std::unique_ptr<Command>> commands;
    commands.push_back(std::make_unique<SessionCommand>(FullName()));
    return commands;
}

std::vector<Argument> SystemCommand::GetArguments() const
{
    return {};
}

std::wstring SystemCommand::ShortDescription() const
{
    return Localization::WSLCCLI_SystemCommandDesc();
}

std::wstring SystemCommand::LongDescription() const
{
    return Localization::WSLCCLI_SystemCommandLongDesc();
}

void SystemCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    OutputHelp();
}
} // namespace wsl::windows::wslc
