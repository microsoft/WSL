/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    SettingsCommand.cpp

Abstract:

    Implementation of SettingsCommand command tree.

--*/
#include "Argument.h"
#include "SettingsCommand.h"
#include "UserSettings.h"
#include "wslutil.h"
#include <iostream>

using namespace wsl::windows::common::wslutil;
using namespace wsl::windows::wslc::execution;
using namespace wsl::windows::wslc::settings;

namespace wsl::windows::wslc {


// SettingsCommand
std::vector<std::unique_ptr<Command>> SettingsCommand::GetCommands() const
{
    std::vector<std::unique_ptr<Command>> commands;
    commands.push_back(std::make_unique<SettingsResetCommand>(FullName()));
    return commands;
}

std::vector<Argument> SettingsCommand::GetArguments() const
{
    return {};
}

std::wstring SettingsCommand::ShortDescription() const
{
    return {L"Open the settings file in the default editor."};
}

std::wstring SettingsCommand::LongDescription() const
{
    return {L"Opens the wslc user settings file in the system default editor for .yaml files.\n"
            L"On first run, creates the file with all settings commented out at their defaults.\n"
            L"A backup of the current settings is saved before the editor opens."};
}

void SettingsCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    settings::User().PrepareToShellExecuteFile();

    const auto path = settings::User().SettingsFilePath();
    const auto result = reinterpret_cast<INT_PTR>(
        ShellExecuteW(nullptr, L"open", path.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL));

    if (result <= 32)
    {
        THROW_HR_MSG(E_UNEXPECTED, "ShellExecuteW failed to open settings file (error %lld)", result);
    }
}

// SettingsResetCommand
std::vector<Argument> SettingsResetCommand::GetArguments() const
{
    return {};
}

std::wstring SettingsResetCommand::ShortDescription() const
{
    return {L"Reset settings to built-in defaults."};
}

std::wstring SettingsResetCommand::LongDescription() const
{
    return {L"Overwrites the settings file with a commented-out defaults template.\n"
            L"Use --force / -f to skip the confirmation prompt."};
}

void SettingsResetCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    // Todo: do we need prompt support?
    settings::User().Reset();
    PrintMessage(L"Settings reset to defaults.");
}

} // namespace wsl::windows::wslc
