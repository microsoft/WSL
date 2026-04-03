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
using namespace wsl::shared;

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
    return Localization::WSLCCLI_SettingsCommandDesc();
}

std::wstring SettingsCommand::LongDescription() const
{
    return Localization::WSLCCLI_SettingsCommandLongDesc();
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
        std::filesystem::path notepadPath = std::filesystem::path{wil::GetSystemDirectoryW().get()} / L"notepad.exe";
        std::wstring quotedPath = L"\"" + path.wstring() + L"\"";
        ShellExecuteW(nullptr, nullptr, notepadPath.c_str(), quotedPath.c_str(), nullptr, SW_SHOW);
    }
}

// SettingsResetCommand
std::vector<Argument> SettingsResetCommand::GetArguments() const
{
    return {};
}

std::wstring SettingsResetCommand::ShortDescription() const
{
    return Localization::WSLCCLI_SettingsResetDesc();
}

std::wstring SettingsResetCommand::LongDescription() const
{
    return Localization::WSLCCLI_SettingsResetLongDesc();
}

void SettingsResetCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    // TODO: do we need prompt support?
    settings::User().Reset();
    PrintMessage(Localization::WSLCCLI_SettingsResetConfirm());
}

} // namespace wsl::windows::wslc
