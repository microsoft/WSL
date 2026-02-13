/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    SessionCommand.h

Abstract:

    Declaration of command classes and interfaces.

--*/
#pragma once
#include "Command.h"

namespace wsl::windows::wslc {
// Root Session Command
struct SessionCommand final : public Command
{
    constexpr static std::wstring_view CommandName = L"session";
    SessionCommand(std::wstring parent) : Command(CommandName, parent, Visibility::Show)
    {
    }
    std::vector<Argument> GetArguments() const override;
    std::wstring_view ShortDescription() const override;
    std::wstring_view LongDescription() const override;

    std::vector<std::unique_ptr<Command>> GetCommands() const override;

protected:
    void ExecuteInternal(CLIExecutionContext& context) const override;
};

// List Command
struct SessionListCommand final : public Command
{
    constexpr static std::wstring_view CommandName = L"list";
    SessionListCommand(std::wstring parent) : Command(CommandName, parent, Visibility::Show)
    {
    }
    std::vector<Argument> GetArguments() const override;
    std::wstring_view ShortDescription() const override;
    std::wstring_view LongDescription() const override;

protected:
    void ExecuteInternal(CLIExecutionContext& context) const override;
};
} // namespace wsl::windows::wslc
