/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    SettingsCommand.h

Abstract:

    Declaration of SettingsCommand command tree.

--*/
#pragma once
#include "Command.h"

namespace wsl::windows::wslc {

// Root settings command: opens the settings file in the user's default editor.
struct SettingsCommand final : public Command
{
    constexpr static std::wstring_view CommandName = L"settings";

    SettingsCommand(const std::wstring& parent) : Command(CommandName, parent)
    {
    }

    std::vector<std::unique_ptr<Command>> GetCommands() const override;
    std::vector<Argument> GetArguments() const override;
    std::wstring ShortDescription() const override;
    std::wstring LongDescription() const override;

protected:
    void ExecuteInternal(CLIExecutionContext& context) const override;
};

// Resets the settings file to built-in defaults.
struct SettingsResetCommand final : public Command
{
    constexpr static std::wstring_view CommandName = L"reset";

    SettingsResetCommand(const std::wstring& parent) : Command(CommandName, parent)
    {
    }

    std::vector<Argument> GetArguments() const override;
    std::wstring ShortDescription() const override;
    std::wstring LongDescription() const override;

protected:
    void ExecuteInternal(CLIExecutionContext& context) const override;
};

} // namespace wsl::windows::wslc
