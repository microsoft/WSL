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

namespace {

    template <ArgType E>
    void ThrowIfSupplied(ArgMap& args)
    {
        bool supplied = false;
        if constexpr (std::is_same_v<ArgMap::value_t<E>, bool>)
        {
            supplied = args.GetValue<E>();
        }
        else
        {
            supplied = args.Contains(E);
        }

        if (!supplied)
        {
            return;
        }

        const auto name = std::wstring(2, WSLC_CLI_ARG_ID_CHAR) + Argument::Create(E).Name();
        THROW_HR_WITH_USER_ERROR(E_NOTIMPL, Localization::WSLCCLI_ComposeOptionNotImplementedError(name));
    }

    template <ArgType... Types>
    void ThrowIfAnySupplied(ArgMap& args)
    {
        (ThrowIfSupplied<Types>(args), ...);
    }

} // namespace

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

void ComposeCommand::ValidateArgumentsInternal(ArgMap& execArgs) const
{
    ThrowIfAnySupplied<ArgType::ComposeAllResources, ArgType::ComposeAnsi, ArgType::ComposeCompatibility, ArgType::ComposeGlobalDryRun, ArgType::ComposeEnvFile, ArgType::ComposeParallel, ArgType::ComposeProfile, ArgType::ComposeProgress, ArgType::ComposeDryRun>(
        execArgs);
}

} // namespace wsl::windows::wslc
