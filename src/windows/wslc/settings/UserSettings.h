/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    UserSettings.h

Abstract:

    Declaration of UserSettings — the singleton that loads, validates, and
    provides access to the wslc user settings file.

--*/
#pragma once
#include "SettingDefinitions.h"
#include <filesystem>
#include <string>
#include <vector>

namespace wsl::windows::wslc::settings {

// Indicates which source the settings were loaded from.
enum class UserSettingsType
{
    Default,  // Neither settings file existed (first run); built-in defaults are used.
    Standard, // Primary file (UserSettings.yaml) loaded successfully.
    Backup,   // Primary file failed to parse; backup file (UserSettings.yaml.bak) was used.
};

struct Warning
{
    std::wstring Message;
    std::wstring SettingPath; // Empty for file-level warnings; key path for per-field warnings.
};

// Singleton that owns the parsed settings for the current process lifetime.
// Load order:
//   1. UserSettings.yaml  (Standard)
//   2. UserSettings.yaml.bak  (Backup, if primary fails)
//   3. Built-in defaults  (Default, if both fail; no warning if neither file exists)
class UserSettings
{
public:
    // Returns the singleton instance. Loaded on first call; subsequent calls are no-ops.
    static UserSettings const& Instance();

    UserSettings(const UserSettings&) = delete;
    UserSettings& operator=(const UserSettings&) = delete;
    UserSettings(UserSettings&&) = delete;
    UserSettings& operator=(UserSettings&&) = delete;

    // Returns the value for setting S, or its built-in default if not present in the file.
    template <Setting S>
    typename details::SettingMapping<S>::value_t Get() const
    {
        return m_settings.GetOrDefault<S>();
    }

    SettingsMap const& GetSettings() const
    {
        return m_settings;
    }

    std::vector<Warning> const& GetWarnings() const
    {
        return m_warnings;
    }

    UserSettingsType GetType() const
    {
        return m_type;
    }

    // Full path to %LOCALAPPDATA%\Microsoft\wslc\UserSettings.yaml
    static std::filesystem::path PrimaryFilePath();

    // Full path to %LOCALAPPDATA%\Microsoft\wslc\UserSettings.yaml.bak
    static std::filesystem::path BackupFilePath();

    // Called before opening the settings file in an editor.
    // - If type is Standard: copies the primary file to the backup path.
    // - If type is Default:  creates the primary file from the commented-out defaults template.
    void PrepareToShellExecuteFile() const;

    // Overwrites the primary settings file with the commented-out defaults template.
    // Called by SettingsResetCommand.
    static void Reset();

private:
    UserSettings();
    ~UserSettings() = default;

    // Base directory shared by PrimaryFilePath() and BackupFilePath().
    // Lazily initialized on first call; safe to call from any static context.
    static const std::filesystem::path& SettingsDir();

    SettingsMap          m_settings;
    std::vector<Warning> m_warnings;
    UserSettingsType     m_type = UserSettingsType::Default;
};

// Convenience free function — returns the singleton instance.
// Usage: settings::User().Get<Setting::Foo>()
inline UserSettings const& User()
{
    return UserSettings::Instance();
}

} // namespace wsl::windows::wslc::settings
