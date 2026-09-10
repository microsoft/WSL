/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    RegistryCommand.cpp

Abstract:

    Implementation of the registry command tree (login, logout).

--*/

#include "CLIExecutionContext.h"
#include "RegistryCommand.h"
#include "RegistryTasks.h"
#include "SessionTasks.h"
#include "Task.h"

using namespace wsl::windows::wslc::execution;
using namespace wsl::windows::wslc::task;
using namespace wsl::shared;

namespace wsl::windows::wslc {

// Registry Root Command
std::vector<std::unique_ptr<Command>> RegistryCommand::GetCommands() const
{
    std::vector<std::unique_ptr<Command>> commands;
    commands.push_back(std::make_unique<RegistryLoginCommand>(FullName()));
    commands.push_back(std::make_unique<RegistryLogoutCommand>(FullName()));
    return commands;
}

std::vector<Argument> RegistryCommand::GetArguments() const
{
    return {};
}

std::wstring RegistryCommand::ShortDescription() const
{
    return Localization::WSLCCLI_RegistryCommandDesc();
}

std::wstring RegistryCommand::LongDescription() const
{
    return Localization::WSLCCLI_RegistryCommandLongDesc();
}

void RegistryCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    OutputHelp(context.Terminal);
}

// Registry Login Command
std::vector<Argument> RegistryLoginCommand::GetArguments() const
{
    return {
        Argument::Create(ArgType::Password),
        Argument::Create(ArgType::PasswordStdin),
        Argument::Create(ArgType::Username),
        Argument::Create(ArgType::Server),
    };
}

std::wstring RegistryLoginCommand::ShortDescription() const
{
    return Localization::WSLCCLI_LoginDesc();
}

std::wstring RegistryLoginCommand::LongDescription() const
{
    return Localization::WSLCCLI_LoginLongDesc();
}

void RegistryLoginCommand::ValidateArgumentsInternal(ArgMap& execArgs) const
{
    if (execArgs.Contains(ArgType::Password) && execArgs.GetValue<ArgType::PasswordStdin>())
    {
        throw ArgumentException(
            Localization::WSLCCLI_LoginPasswordAndStdinMutuallyExclusive(), GetArgumentsForHelp({ArgType::Password, ArgType::PasswordStdin}));
    }

    if (execArgs.GetValue<ArgType::PasswordStdin>() && !execArgs.Contains(ArgType::Username))
    {
        throw ArgumentException(
            Localization::WSLCCLI_LoginPasswordStdinRequiresUsername(), GetArgumentsForHelp({ArgType::PasswordStdin, ArgType::Username}));
    }
}

void RegistryLoginCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    if (!context.Args.Contains(ArgType::Username))
    {
        auto username = context.Terminal.PromptForLine(Localization::WSLCCLI_LoginUsernamePrompt());
        context.Args.Add(ArgType::Username, std::move(username));
    }

    // Resolve password: --password, --password-stdin, or interactive prompt.
    if (!context.Args.Contains(ArgType::Password))
    {
        if (context.Args.GetValue<ArgType::PasswordStdin>())
        {
            context.Args.Add(ArgType::Password, context.Terminal.ReadLine().value_or(std::wstring{}));
        }
        else
        {
            auto password = context.Terminal.PromptForLine(Localization::WSLCCLI_LoginPasswordPrompt(), true);
            context.Args.Add(ArgType::Password, std::move(password));
        }
    }

    context //
        << ResolveSession << Login;
}

// Registry Logout Command
std::vector<Argument> RegistryLogoutCommand::GetArguments() const
{
    return {
        Argument::Create(ArgType::Server),
    };
}

std::wstring RegistryLogoutCommand::ShortDescription() const
{
    return Localization::WSLCCLI_LogoutDesc();
}

std::wstring RegistryLogoutCommand::LongDescription() const
{
    return Localization::WSLCCLI_LogoutLongDesc();
}

void RegistryLogoutCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    context //
        << Logout;
}

} // namespace wsl::windows::wslc
