/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    RegistryCommand.h

Abstract:

    Declaration of the registry command tree (login, logout).

--*/
#pragma once
#include "Command.h"

namespace wsl::windows::wslc {

// Root registry command: wslc registry [login|logout]
struct RegistryCommand final : public Command
{
    constexpr static std::wstring_view CommandName = L"registry";
    RegistryCommand(const std::wstring& parent) : Command(CommandName, parent)
    {
    }

    std::vector<std::unique_ptr<Command>> GetCommands() const override;
    std::vector<Argument> GetArguments() const override;
    std::wstring ShortDescription() const override;
    std::wstring LongDescription() const override;

protected:
    void ExecuteInternal(CLIExecutionContext& context) const override;
};

// Login Command
struct RegistryLoginCommand final : public Command
{
    constexpr static std::wstring_view CommandName = L"login";

    RegistryLoginCommand(const std::wstring& parent) : Command(CommandName, parent)
    {
    }

    std::vector<Argument> GetArguments() const override;
    std::wstring ShortDescription() const override;
    std::wstring LongDescription() const override;

protected:
    void ExecuteInternal(CLIExecutionContext& context) const override;
};

// Logout Command
struct RegistryLogoutCommand final : public Command
{
    constexpr static std::wstring_view CommandName = L"logout";

    RegistryLogoutCommand(const std::wstring& parent) : Command(CommandName, parent)
    {
    }

    std::vector<Argument> GetArguments() const override;
    std::wstring ShortDescription() const override;
    std::wstring LongDescription() const override;

protected:
    void ExecuteInternal(CLIExecutionContext& context) const override;
};

} // namespace wsl::windows::wslc
