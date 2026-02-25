/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    UserSettings.h

Abstract:

    UserSettings class for wslc - loads, validates, and provides typed access to settings.

--*/
#pragma once

#include "SettingDefinitions.h"
#include <map>
#include <string>
#include <vector>

namespace wsl::windows::wslc::settings {

// Type of settings file that was loaded.
enum class UserSettingsType
{
    Default, // No file found, using compiled defaults
    Standard, // Loaded from settings.json
    Custom // Loaded from string content (testing)
};

struct Warning
{
    std::wstring Message;
    std::wstring Path; // JSON path that caused the warning
    std::wstring Data; // Value that caused the warning
};

struct UserSettings
{
    // Constructs settings by loading from file. Pass content to override file loading (for tests).
    explicit UserSettings(const std::optional<std::string>& content = std::nullopt);

    // Returns the path to the settings file.
    static std::filesystem::path SettingsFilePath();

    UserSettingsType GetType() const
    {
        return m_type;
    }

    const std::vector<Warning>& GetWarnings() const
    {
        return m_warnings;
    }

    // Typed getter. Returns the validated value, or the compiled default if absent/invalid.
    template <Setting S>
    typename SettingMapping<S>::value_t Get() const
    {
        auto itr = m_settings.find(S);
        if (itr == m_settings.end())
        {
            return GetDefault<S>();
        }

        return std::get<details::SettingIndex(S)>(itr->second);
    }

private:
    // Runtime default for settings that can't be constexpr (e.g., StoragePath).
    template <Setting S>
    typename SettingMapping<S>::value_t GetDefault() const
    {
        return SettingMapping<S>::DefaultValue;
    }

    // Explicit specialization declaration for StoragePath (defined in UserSettings.cpp).
    template <>
    std::filesystem::path GetDefault<Setting::StoragePath>() const;

    UserSettingsType m_type = UserSettingsType::Default;
    std::vector<Warning> m_warnings;
    std::map<Setting, details::SettingVariant> m_settings;
};

} // namespace wsl::windows::wslc::settings
