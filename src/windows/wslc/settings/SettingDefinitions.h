/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    SettingDefinitions.h

Abstract:

    Declaration of user settings with their YAML paths, types, and defaults.

--*/
#pragma once
#include "EnumVariantMap.h"
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// How to add a setting:
// 1 - Add an entry to the Setting enum.
// 2 - Add a DEFINE_SETTING_MAPPING specialization with yaml_t, value_t, default, and YAML path.
// 3 - Implement the Validate function in UserSettings.cpp.

namespace wsl::windows::wslc::settings {

// Enum of all user settings.
// Must start at 0 to enable direct variant indexing.
// Max must be last and unused.
enum class Setting : size_t
{
    SessionCpuCount,
    SessionMemoryMb,
    SessionStorageSizeMb,
    SessionStoragePath,

    Max
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

//                         Setting               yaml_t       value_t       default  YamlPath
DEFINE_SETTING_MAPPING(SessionCpuCount,      uint32_t,    uint32_t,     4,     "session.cpuCount")
DEFINE_SETTING_MAPPING(SessionMemoryMb,      uint32_t,    uint32_t,     2048,  "session.memorySizeMb")
DEFINE_SETTING_MAPPING(SessionStorageSizeMb, uint32_t,    uint32_t,     10000, "session.maxStorageSizeMb")
DEFINE_SETTING_MAPPING(SessionStoragePath,   std::string, std::wstring, {},    "session.defaultStoragePath")
// clang-format on

#undef DEFINE_SETTING_MAPPING

} // namespace details

// Type-safe enum-indexed map of all settings values, backed by EnumBasedVariantMap.
// Each setting key holds at most one value; Contains(S) indicates whether the setting
// was explicitly loaded from the file (vs. falling back to DefaultValue).
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

} // namespace wsl::windows::wslc::settings
