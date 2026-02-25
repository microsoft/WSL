/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    SettingsCommand.h

Abstract:

    Settings command tree: current, default, new.

--*/
#pragma once

#include "Command.h"
#include "UserSettings.h"
#include <nlohmann/json.hpp>

using wsl::windows::common::wslutil::PrintMessage;

namespace wsl::windows::wslc {

namespace settings_helpers {

// Builds a JSON object from a UserSettings instance.
inline nlohmann::json SettingsToJson(const wsl::windows::wslc::settings::UserSettings& settings)
{
    using namespace wsl::windows::wslc::settings;

    nlohmann::json output;
    output["session"]["cpuCount"] = settings.Get<Setting::CpuCount>();
    output["session"]["memoryMb"] = settings.Get<Setting::MemoryMb>();
    output["session"]["bootTimeoutMs"] = settings.Get<Setting::BootTimeoutMs>();
    output["session"]["maximumStorageSizeMb"] = settings.Get<Setting::MaximumStorageSizeMb>();

    auto networkingMode = settings.Get<Setting::NetworkingMode>();
    output["session"]["networkingMode"] = (networkingMode == SessionNetworkingMode::Nat) ? "nat" : "none";

    output["session"]["storagePath"] = wsl::shared::string::WideToMultiByte(settings.Get<Setting::StoragePath>().wstring());

    return output;
}

// Opens a file in the default editor, falling back to notepad.
inline void OpenInEditor(const std::filesystem::path& path)
{
    auto result = ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, path.parent_path().c_str(), SW_SHOWNORMAL);

    if (reinterpret_cast<INT_PTR>(result) <= 32)
    {
        ShellExecuteW(nullptr, L"open", L"notepad.exe", path.c_str(), nullptr, SW_SHOWNORMAL);
    }
}

} // namespace settings_helpers

// Shows the current effective settings (user overrides merged with defaults).
struct SettingsCurrentCommand : Command
{
    SettingsCurrentCommand(const std::wstring& parent) : Command(L"current", parent)
    {
    }

    std::wstring ShortDescription() const override
    {
        return {L"Show current effective settings."};
    }

    std::wstring LongDescription() const override
    {
        return {L"Displays the current effective settings as JSON. "
                L"This includes default values for settings not explicitly configured."};
    }

protected:
    void ExecuteInternal(CLIExecutionContext&) const override
    {
        using namespace wsl::windows::wslc::settings;

        UserSettings settings;
        auto output = settings_helpers::SettingsToJson(settings);
        PrintMessage(wsl::shared::string::MultiByteToWide(output.dump(4)));
    }
};

// Shows the compiled default settings (ignoring user file).
struct SettingsDefaultsCommand : Command
{
    SettingsDefaultsCommand(const std::wstring& parent) : Command(L"default", parent)
    {
    }

    std::wstring ShortDescription() const override
    {
        return {L"Show default settings."};
    }

    std::wstring LongDescription() const override
    {
        return {L"Displays the compiled default settings as JSON, ignoring any user configuration."};
    }

protected:
    void ExecuteInternal(CLIExecutionContext&) const override
    {
        using namespace wsl::windows::wslc::settings;

        // Construct with empty JSON so no user values are loaded.
        UserSettings defaults(std::string("{}"));
        auto output = settings_helpers::SettingsToJson(defaults);
        PrintMessage(wsl::shared::string::MultiByteToWide(output.dump(4)));
    }
};

// Creates a settings file populated with defaults and opens it in an editor.
struct SettingsNewCommand : Command
{
    SettingsNewCommand(const std::wstring& parent) : Command(L"new", parent)
    {
    }

    std::wstring ShortDescription() const override
    {
        return {L"Create a new settings file with defaults."};
    }

    std::wstring LongDescription() const override
    {
        return {L"Creates a settings file populated with all default values and opens it in an editor. "
                L"If a settings file already exists, it is opened without modification."};
    }

protected:
    void ExecuteInternal(CLIExecutionContext&) const override
    {
        using namespace wsl::windows::wslc::settings;

        auto settingsPath = UserSettings::SettingsFilePath();

        if (!std::filesystem::exists(settingsPath))
        {
            // Build defaults JSON with the schema reference.
            UserSettings defaults(std::string("{}"));
            auto output = settings_helpers::SettingsToJson(defaults);
            output["$schema"] = "https://aka.ms/wslc-settings.schema.json";

            std::filesystem::create_directories(settingsPath.parent_path());
            std::ofstream file(settingsPath);
            file << output.dump(4);
        }

        settings_helpers::OpenInEditor(settingsPath);
    }
};

// Parent command for all settings subcommands.
struct SettingsCommand : Command
{
    SettingsCommand(const std::wstring& parent) : Command(L"settings", {L"config"}, parent)
    {
    }

    std::vector<std::unique_ptr<Command>> GetCommands() const override
    {
        std::vector<std::unique_ptr<Command>> commands;
        commands.reserve(3);
        commands.push_back(std::make_unique<SettingsCurrentCommand>(FullName()));
        commands.push_back(std::make_unique<SettingsDefaultsCommand>(FullName()));
        commands.push_back(std::make_unique<SettingsNewCommand>(FullName()));
        return commands;
    }

    std::wstring ShortDescription() const override
    {
        return {L"Manage WSLC settings."};
    }

    std::wstring LongDescription() const override
    {
        return {L"View and manage WSLC settings. Use subcommands to show current or default settings, "
                L"or create a new settings file."};
    }

protected:
    void ExecuteInternal(CLIExecutionContext&) const override
    {
        OutputHelp();
    }
};

} // namespace wsl::windows::wslc
