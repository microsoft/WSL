/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    UserSettings.h

Abstract:

    Declaration of UserSettings — the singleton that loads, validates, and
    provides access to the wslc user settings file.

--*/
#pragma once
#include "defs.h"
#include "EnumVariantMap.h"
#include "wslc.h"
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// How to add a setting:
// 1 - Add an entry to the Setting enum.
// 2 - Add a DEFINE_SETTING_MAPPING specialization with yaml_t, value_t, default, and YAML path.
// 3 - Implement the Validate function in UserSettings.cpp if needed, otherwise use pass through.

namespace wsl::windows::wslc::settings {

// Enum of all user settings.
// Must start at 0 to enable direct variant indexing.
// Max must be last and unused.
enum class Setting : size_t
{
    SessionCpuCount = 0,
    SessionMemoryMb,
    SessionStorageSizeMb,
    SessionStoragePath,
    SessionNetworkingMode,
    SessionHostFileShareMode,
    SessionDnsTunneling,
    CredentialStore,

    Max
};

enum class HostFileShareMode
{
    Plan9,
    VirtioFs
};

enum class CredentialStoreType
{
    WinCred,
    File
};

namespace details {

    template <Setting S>
    struct SettingMapping
    {
        // yaml_t       - the C++ type read from the YAML node via node.as<yaml_t>()
        // value_t      - the native type stored in SettingsMap
        // DefaultValue - used when the key is absent or fails validation
        // YamlPath     - dot-separated path into the YAML document (e.g. "session.cpuCount")
        // Validate     - semantic validation; returns nullopt to reject and fall back to default
    };

    // clang-format off
#define DEFINE_SETTING_MAPPING(_setting_, _yaml_t_, _value_t_, _default_, _path_) \
    template <>                                                                     \
    struct SettingMapping<Setting::_setting_>                                       \
    {                                                                               \
        using yaml_t  = _yaml_t_;                                                  \
        using value_t = _value_t_;                                                  \
        inline static const value_t DefaultValue = _default_;                      \
        static constexpr std::string_view YamlPath = _path_;                       \
        static std::optional<value_t> Validate(const yaml_t& value);               \
    };

    DEFINE_SETTING_MAPPING(SessionCpuCount,          uint32_t,    uint32_t,           4,                             "session.cpuCount")
    DEFINE_SETTING_MAPPING(SessionMemoryMb,          std::string, uint32_t,           2048,                          "session.memorySize")
    DEFINE_SETTING_MAPPING(SessionStorageSizeMb,     std::string, uint32_t,           102400,                        "session.maxStorageSize")
    DEFINE_SETTING_MAPPING(SessionStoragePath,       std::string, std::wstring,       {},                            "session.defaultStoragePath")
    DEFINE_SETTING_MAPPING(SessionNetworkingMode,    std::string, WSLCNetworkingMode, WSLCNetworkingModeVirtioProxy, "session.networkingMode")
    DEFINE_SETTING_MAPPING(SessionHostFileShareMode, std::string, HostFileShareMode,  HostFileShareMode::VirtioFs,   "session.hostFileShareMode")
    DEFINE_SETTING_MAPPING(SessionDnsTunneling,      bool,        bool,               true,                          "session.dnsTunneling")
    DEFINE_SETTING_MAPPING(CredentialStore,            std::string, CredentialStoreType, CredentialStoreType::WinCred,  "credentialStore")

#undef DEFINE_SETTING_MAPPING
    // clang-format on

} // namespace details

// Type-safe enum-indexed map of all settings values, backed by EnumBasedVariantMap.
struct SettingsMap : wsl::windows::wslc::EnumBasedVariantMap<Setting, details::SettingMapping>
{
    // Returns the stored value if present, otherwise the compile-time default.
    template <Setting S>
    typename details::SettingMapping<S>::value_t GetOrDefault() const
    {
        if (Contains(S))
        {
            return Get<S>();
        }
        return details::SettingMapping<S>::DefaultValue;
    }
};

// Indicates which source the settings were loaded from.
enum class UserSettingsType
{
    Default,  // Settings file did not exist or failed to parse; built-in defaults are used.
    Standard, // Settings file (settings.yaml) loaded successfully.
};

struct Warning
{
    std::wstring Message;
    std::wstring SettingPath; // Empty for file-level warnings; key path for per-field warnings.
};

// Singleton that owns the parsed settings for the current process lifetime.
// Load order:
//   1. settings.yaml  (Standard)
//   2. Built-in defaults  (Default, if the file is absent or fails to parse)
class UserSettings
{
public:
    // Returns the singleton instance. Loaded on first call; subsequent calls are no-ops.
    static UserSettings const& Instance();

    NON_COPYABLE(UserSettings);
    NON_MOVABLE(UserSettings);

    // Returns the value for setting S, or its built-in default if not present in the file.
    template <Setting S>
    typename details::SettingMapping<S>::value_t Get() const
    {
        return m_settings.GetOrDefault<S>();
    }

    std::vector<Warning> const& GetWarnings() const
    {
        return m_warnings;
    }

    UserSettingsType GetType() const
    {
        return m_type;
    }

    // Called before opening the settings file in an editor.
    // If type is Default, creates the file from the commented-out defaults template.
    void PrepareToShellExecuteFile() const;

    std::filesystem::path SettingsFilePath() const;

    // Overwrites the settings file with the commented-out defaults template.
    void Reset() const;

protected:
    // Loads settings from an explicit directory. Used by the singleton (via
    // the private zero-arg constructor) and by test subclasses.
    explicit UserSettings(const std::filesystem::path& settingsDir);
    ~UserSettings() = default;

private:
    UserSettings();

    SettingsMap m_settings;
    std::vector<Warning> m_warnings;
    UserSettingsType m_type = UserSettingsType::Default;
    std::filesystem::path m_settingsPath;
};

// Convenience free function — returns the singleton instance.
// Usage: settings::User().Get<Setting::Foo>()
inline UserSettings const& User()
{
    return UserSettings::Instance();
}

} // namespace wsl::windows::wslc::settings
