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
#include <iostream>

using namespace wsl::windows::wslc::execution;
using namespace wsl::windows::wslc::task;
using namespace wsl::shared;

namespace {

auto MaskInput()
{
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;

    if ((input != INVALID_HANDLE_VALUE) && GetConsoleMode(input, &mode))
    {
        SetConsoleMode(input, mode & ~ENABLE_ECHO_INPUT);
        return wil::scope_exit(std::function<void()>([input, mode] {
            SetConsoleMode(input, mode);
            std::wcerr << L'\n';
        }));
    }

    return wil::scope_exit(std::function<void()>([] {}));
}

std::wstring Prompt(const std::wstring& label, bool maskInput)
{
    // Write without a trailing newline so the cursor stays inline (matching Docker's behavior).
    std::wcerr << label;

    auto restoreConsole = maskInput ? MaskInput() : wil::scope_exit(std::function<void()>([] {}));

    std::wstring value;
    std::getline(std::wcin, value);

    return value;
}

} // namespace

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
    OutputHelp();
}

// Registry Login Command
std::vector<Argument> RegistryLoginCommand::GetArguments() const
{
    return {
        Argument::Create(ArgType::Password),
        Argument::Create(ArgType::PasswordStdin),
        Argument::Create(ArgType::Username),
        Argument::Create(ArgType::Server),
        Argument::Create(ArgType::Session),
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

void RegistryLoginCommand::ValidateArgumentsInternal(const ArgMap& execArgs) const
{
    if (execArgs.Contains(ArgType::Password) && execArgs.Contains(ArgType::PasswordStdin))
    {
        throw CommandException(Localization::WSLCCLI_LoginPasswordAndStdinMutuallyExclusive());
    }

    if (execArgs.Contains(ArgType::PasswordStdin) && !execArgs.Contains(ArgType::Username))
    {
        throw CommandException(Localization::WSLCCLI_LoginPasswordStdinRequiresUsername());
    }
}

void RegistryLoginCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    // Prompt for username if not provided.
    if (!context.Args.Contains(ArgType::Username))
    {
        context.Args.Add(ArgType::Username, Prompt(L"Username: ", false));
    }

    // Resolve password: --password, --password-stdin, or interactive prompt.
    if (!context.Args.Contains(ArgType::Password))
    {
        if (context.Args.Contains(ArgType::PasswordStdin))
        {
            std::wstring line;
            std::getline(std::wcin, line);
            if (!line.empty() && line.back() == L'\r')
            {
                line.pop_back();
            }

            context.Args.Add(ArgType::Password, std::move(line));
        }
        else
        {
            context.Args.Add(ArgType::Password, Prompt(L"Password: ", true));
        }
    }

    context //
        << CreateSession << Login;
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
