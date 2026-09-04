/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ComposeCommand.cpp

Abstract:

    Implements the minimal compose command tree.

--*/

#include "precomp.h"
#include "ComposeCommand.h"

using namespace wsl::shared;

namespace wsl::windows::wslc {

std::vector<std::unique_ptr<Command>> ComposeCommand::GetCommands() const
{
    std::vector<std::unique_ptr<Command>> commands;
    commands.push_back(std::make_unique<ComposeCreateCommand>(FullName()));
    commands.push_back(std::make_unique<ComposeListCommand>(FullName()));
    commands.push_back(std::make_unique<ComposeUpCommand>(FullName()));
    commands.push_back(std::make_unique<ComposeStartCommand>(FullName()));
    commands.push_back(std::make_unique<ComposeAttachCommand>(FullName()));
    commands.push_back(std::make_unique<ComposeStopCommand>(FullName()));
    return commands;
}

std::wstring ComposeCommand::ShortDescription() const
{
    return Localization::WSLCCLI_ComposeCommandDesc();
}

std::wstring ComposeCommand::LongDescription() const
{
    return Localization::WSLCCLI_ComposeCommandLongDesc();
}

void ComposeCommand::ExecuteInternal(CLIExecutionContext& Context) const
{
    OutputHelp(Context.Terminal);
}

} // namespace wsl::windows::wslc
