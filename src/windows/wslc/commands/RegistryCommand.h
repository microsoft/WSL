/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    RegistryCommand.h

Abstract:

    Declaration of command classes and interfaces.

--*/
#pragma once
#include "Command.h"

namespace wsl::windows::wslc {
// Root Registry Command
struct RegistryCommand final : public Command
{
    constexpr static std::wstring_view CommandName = L"registry";
    RegistryCommand(std::wstring parent) : Command(CommandName, parent, Visibility::Show)
    {
    }
    std::vector<Argument> GetArguments() const override;
    std::wstring_view ShortDescription() const override;
    std::wstring_view LongDescription() const override;

    std::vector<std::unique_ptr<Command>> GetCommands() const override;

protected:
    void ExecuteInternal(CLIExecutionContext& context) const override;
};

// Login Command
struct RegistryLoginCommand final : public Command
{
    constexpr static std::wstring_view CommandName = L"login";
    RegistryLoginCommand(std::wstring parent) : Command(CommandName, parent, Visibility::Show)
    {
    }
    std::vector<Argument> GetArguments() const override;
    std::wstring_view ShortDescription() const override;
    std::wstring_view LongDescription() const override;

protected:
    void ExecuteInternal(CLIExecutionContext& context) const override;
};

// Logout Command
struct RegistryLogoutCommand final : public Command
{
    constexpr static std::wstring_view CommandName = L"logout";
    RegistryLogoutCommand(std::wstring parent) : Command(CommandName, parent, Visibility::Show)
    {
    }
    std::vector<Argument> GetArguments() const override;
    std::wstring_view ShortDescription() const override;
    std::wstring_view LongDescription() const override;

protected:
    void ExecuteInternal(CLIExecutionContext& context) const override;
};
} // namespace wsl::windows::wslc
