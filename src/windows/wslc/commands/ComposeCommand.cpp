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

std::vector<std::unique_ptr<Command>> ComposeCommand::CreateCommands() const
{
    std::vector<std::unique_ptr<Command>> commands;
    commands.push_back(std::make_unique<ComposeAttachCommand>(FullName()));
    commands.push_back(std::make_unique<ComposeCreateCommand>(FullName()));
    commands.push_back(std::make_unique<ComposeStartCommand>(FullName()));
    commands.push_back(std::make_unique<ComposeStopCommand>(FullName()));
    commands.push_back(std::make_unique<ComposeUpCommand>(FullName()));
    return commands;
}

std::vector<Argument> ComposeCommand::GetGlobalArguments() const
{
    return {
        CreateGlobalArgument(ArgType::ComposeAllResources),
        CreateGlobalArgument(ArgType::ComposeAnsi),
        CreateGlobalArgument(ArgType::ComposeCompatibility),
        CreateGlobalArgument(ArgType::ComposeGlobalDryRun),
        CreateGlobalArgument(ArgType::ComposeEnvFile, {.Limit = Limit::Unlimited}),
        CreateGlobalArgument(ArgType::ComposeFile, {.Limit = Limit::Unlimited}),
        CreateGlobalArgument(ArgType::ComposeParallel),
        CreateGlobalArgument(ArgType::ComposeProfile, {.Limit = Limit::Unlimited}),
        CreateGlobalArgument(ArgType::ComposeProgress),
        CreateGlobalArgument(ArgType::ComposeProjectDirectory),
        CreateGlobalArgument(ArgType::ComposeProjectName),
    };
}

std::wstring ComposeCommand::ShortDescription() const
{
    return Localization::WSLCCLI_ComposeCommandDesc();
}

std::wstring ComposeCommand::LongDescription() const
{
    return Localization::WSLCCLI_ComposeCommandLongDesc();
}

void ComposeCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    OutputHelp(context.Terminal);
}

std::vector<Argument> ComposeSubcommand::GetArguments() const
{
    auto arguments = GetComposeArguments();
    arguments.push_back(Argument::Create(ArgType::ComposeDryRun));
    return arguments;
}

} // namespace wsl::windows::wslc
