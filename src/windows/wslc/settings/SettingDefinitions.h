/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    SettingDefinitions.h

Abstract:

    Setting enum, mapping template, and all setting specializations for wslc.

--*/
#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <variant>

namespace wsl::windows::wslc::settings {

// Networking mode for sessions (mirrors WSLA enum values).
enum class SessionNetworkingMode
{
    Nat = 0,
    None = 1
};

// Each setting in the system. Values must start at 0, increment by 1, Max must be last.
enum class Setting : size_t
{
    CpuCount,
    MemoryMb,
    BootTimeoutMs,
    MaximumStorageSizeMb,
    NetworkingMode,
    StoragePath,

    Max
};

// Group policy value policies.
enum class ValuePolicy
{
    None,
    MaxCpuCount,
    MaxMemoryMb,
    DefaultNetworkingMode,

    Max
};

// Maps a Setting enum value to its types, default, JSON path, and optional policy link.
// Primary template - each setting must specialize this.
template <Setting S>
struct SettingMapping;

// Macro for declaring a setting mapping specialization with a group policy link.
#define WSLC_SETTING_POLICY(_setting_, _json_t_, _value_t_, _default_, _path_, _policy_) \
    template <>                                                                          \
    struct SettingMapping<_setting_>                                                      \
    {                                                                                    \
        using json_t = _json_t_;                                                         \
        using value_t = _value_t_;                                                       \
        inline static const value_t DefaultValue = _default_;                            \
        static constexpr std::string_view Path = _path_;                                 \
        static constexpr ValuePolicy Policy = _policy_;                                  \
        static std::optional<value_t> Validate(const json_t& value);                     \
    };

// Convenience: setting without a policy link.
#define WSLC_SETTING(_setting_, _json_t_, _value_t_, _default_, _path_) \
    WSLC_SETTING_POLICY(_setting_, _json_t_, _value_t_, _default_, _path_, ValuePolicy::None)

// --- Setting Declarations ---

WSLC_SETTING_POLICY(Setting::CpuCount, int, int, 4, "session.cpuCount", ValuePolicy::MaxCpuCount)

WSLC_SETTING_POLICY(Setting::MemoryMb, int, int, 2048, "session.memoryMb", ValuePolicy::MaxMemoryMb)

WSLC_SETTING(Setting::BootTimeoutMs, int, int, 30000, "session.bootTimeoutMs")

WSLC_SETTING(Setting::MaximumStorageSizeMb, int, int, 10000, "session.maximumStorageSizeMb")

WSLC_SETTING_POLICY(Setting::NetworkingMode,
                     std::string,
                     SessionNetworkingMode,
                     SessionNetworkingMode::Nat,
                     "session.networkingMode",
                     ValuePolicy::DefaultNetworkingMode)

// Note: StoragePath default is computed at runtime (LocalAppData/wsla), not a constexpr.
// The Validate() function handles this. Default here is just a placeholder.
WSLC_SETTING(Setting::StoragePath, std::string, std::filesystem::path, {}, "session.storagePath")

// --- Variant type auto-deduction ---

namespace details {

template <size_t... I>
inline auto DeduceVariant(std::index_sequence<I...>)
{
    return std::variant<std::monostate, typename SettingMapping<static_cast<Setting>(I)>::value_t...>{};
}

using SettingVariant = decltype(DeduceVariant(std::make_index_sequence<static_cast<size_t>(Setting::Max)>()));

constexpr size_t SettingIndex(Setting s)
{
    return static_cast<size_t>(s) + 1;
}

} // namespace details
} // namespace wsl::windows::wslc::settings
