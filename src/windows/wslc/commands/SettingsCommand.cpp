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
    return {
        L"Opens the wslc user settings file in the system default editor for .yaml files.\n"
        L"On first run, creates the file with all settings commented out at their defaults.\n"
        L"A backup of the current settings is saved before the editor opens."};
}

void SettingsCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    settings::User().PrepareToShellExecuteFile();

    const auto& path = settings::User().SettingsFilePath();

    // Some versions of windows will fail if no file extension association exists, other will pop up the dialog
    // to make the user pick their default.
    HINSTANCE res = ShellExecuteW(nullptr, nullptr, path.c_str(), nullptr, nullptr, SW_SHOW);
    if (static_cast<int>(reinterpret_cast<uintptr_t>(res)) <= 32)
    {
        // User doesn't have file type association. Default to notepad
        // Quote the path so that Notepad treats it as a single argument even if it contains spaces.
        std::wstring quotedPath = L"\"" + path.wstring() + L"\"";
        ShellExecuteW(nullptr, nullptr, L"notepad", quotedPath.c_str(), nullptr, SW_SHOW);
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
    return {L"Overwrites the settings file with a commented-out defaults template."};
}

void SettingsResetCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    // Todo: do we need prompt support?
    settings::User().Reset();
    PrintMessage(L"Settings reset to defaults.");
}

} // namespace wsl::windows::wslc
