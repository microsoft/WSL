/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    UserSettings.cpp

Abstract:

    UserSettings implementation - JSON loading, validation pipeline, file fallback.

--*/

#include <precomp.h>
#include "UserSettings.h"
#include "GroupPolicy.h"
#include <nlohmann/json.hpp>
#include <fstream>

using json = nlohmann::json;

namespace wsl::windows::wslc::settings {

std::filesystem::path UserSettings::SettingsFilePath()
{
    return wsl::windows::common::filesystem::GetLocalAppDataPath(nullptr) / L"wsla" / L"settings.json";
}

// --- JSON path resolver ---
// Resolves a dotted path like "session.cpuCount" into the nested JSON value.
static json ResolveJsonPath(const json& root, std::string_view path)
{
    const json* current = &root;
    size_t start = 0;
    while (start < path.size())
    {
        auto dot = path.find('.', start);
        auto segment = path.substr(start, dot == std::string_view::npos ? std::string_view::npos : dot - start);
        if (!current->is_object() || !current->contains(std::string(segment)))
        {
            return json{}; // Path not found
        }

        current = &(*current)[std::string(segment)];
        start = (dot == std::string_view::npos) ? path.size() : dot + 1;
    }

    return *current;
}

// --- Type extraction from JSON ---
template <typename T>
static std::optional<T> GetJsonValue(const json& j);

template <>
std::optional<int> GetJsonValue<int>(const json& j)
{
    return j.is_number_integer() ? std::optional<int>(j.get<int>()) : std::nullopt;
}

template <>
std::optional<bool> GetJsonValue<bool>(const json& j)
{
    return j.is_boolean() ? std::optional<bool>(j.get<bool>()) : std::nullopt;
}

template <>
std::optional<std::string> GetJsonValue<std::string>(const json& j)
{
    return j.is_string() ? std::optional<std::string>(j.get<std::string>()) : std::nullopt;
}

// --- Per-setting validation ---

template <Setting S>
static void ValidateSetting(const json& root,
                            std::map<Setting, details::SettingVariant>& settings,
                            std::vector<Warning>& warnings)
{
    using Mapping = SettingMapping<S>;
    constexpr auto path = Mapping::Path;

    // Step 1: Resolve JSON path.
    auto resolved = ResolveJsonPath(root, path);
    if (resolved.is_null())
    {
        return; // Key absent -> use default
    }

    // Step 2: Type check.
    auto jsonValue = GetJsonValue<typename Mapping::json_t>(resolved);
    if (!jsonValue.has_value())
    {
        auto pathW = wsl::shared::string::MultiByteToWide(std::string(path));
        warnings.push_back(
            {std::format(L"Invalid type for setting '{}'", pathW), pathW, wsl::shared::string::MultiByteToWide(resolved.dump())});
        return;
    }

    // Step 3: Semantic validation.
    auto validated = Mapping::Validate(jsonValue.value());
    if (!validated.has_value())
    {
        auto pathW = wsl::shared::string::MultiByteToWide(std::string(path));
        warnings.push_back({std::format(L"Invalid value for setting '{}'", pathW),
                            pathW,
                            wsl::shared::string::MultiByteToWide(resolved.dump())});
        return;
    }

    // Step 4: Store.
    details::SettingVariant variant;
    variant.emplace<details::SettingIndex(S)>(std::move(validated.value()));
    settings[S] = std::move(variant);
}

// Validate all settings using fold expression.
template <size_t... I>
static void ValidateAll(const json& root,
                        std::map<Setting, details::SettingVariant>& settings,
                        std::vector<Warning>& warnings,
                        std::index_sequence<I...>)
{
    (ValidateSetting<static_cast<Setting>(I)>(root, settings, warnings), ...);
}

// --- File loading ---

static std::optional<json> ParseJsonFile(const std::filesystem::path& path, std::vector<Warning>& warnings)
{
    std::ifstream file(path);
    if (!file.is_open())
    {
        return std::nullopt;
    }

    try
    {
        return json::parse(file, nullptr, true, true); // allow comments
    }
    catch (const json::exception& e)
    {
        warnings.push_back({std::format(L"Failed to parse settings file '{}': {}",
                                        path.wstring(),
                                        wsl::shared::string::MultiByteToWide(e.what())),
                            path.wstring(),
                            {}});
        return std::nullopt;
    }
}

static std::optional<json> ParseJsonString(const std::string& content, std::vector<Warning>& warnings)
{
    try
    {
        return json::parse(content, nullptr, true, true);
    }
    catch (const json::exception& e)
    {
        warnings.push_back(
            {std::format(L"Failed to parse settings content: {}", wsl::shared::string::MultiByteToWide(e.what())), {}, {}});
        return std::nullopt;
    }
}

// --- Constructor (loading + validation) ---

UserSettings::UserSettings(const std::optional<std::string>& content)
{
    json root;

    // Step 0: Check group policy toggle.
    if (!GroupPolicies().IsEnabled(TogglePolicy::AllowSettings))
    {
        // Settings file is disabled by policy. Use all defaults.
        return;
    }

    // Step 1: Try custom content (for testing).
    if (content.has_value())
    {
        auto parsed = ParseJsonString(content.value(), m_warnings);
        if (parsed.has_value())
        {
            m_type = UserSettingsType::Custom;
            root = std::move(parsed.value());
        }
    }
    else
    {
        // Step 2: Try settings file.
        auto primary = ParseJsonFile(SettingsFilePath(), m_warnings);
        if (primary.has_value())
        {
            m_type = UserSettingsType::Standard;
            root = std::move(primary.value());
        }
        // If file doesn't exist or is corrupt, use defaults silently (warnings added by ParseJsonFile).
    }

    // Step 6: Validate all settings.
    if (!root.is_null())
    {
        ValidateAll(root, m_settings, m_warnings, std::make_index_sequence<static_cast<size_t>(Setting::Max)>());
    }
}

// --- Validate() implementations for each setting ---

// CpuCount: must be >= 1, capped to host logical CPU count.
std::optional<int> SettingMapping<Setting::CpuCount>::Validate(const int& value)
{
    if (value < 1)
    {
        return std::nullopt;
    }

    SYSTEM_INFO sysInfo{};
    GetSystemInfo(&sysInfo);
    int maxCpus = static_cast<int>(sysInfo.dwNumberOfProcessors);

    int result = std::min(value, maxCpus);

    // Apply policy cap.
    auto policyCap = GroupPolicies().GetValue<ValuePolicy::MaxCpuCount>();
    if (policyCap.has_value() && result > static_cast<int>(policyCap.value()))
    {
        result = static_cast<int>(policyCap.value());
    }

    return result;
}

// MemoryMb: must be >= 256.
std::optional<int> SettingMapping<Setting::MemoryMb>::Validate(const int& value)
{
    if (value < 256)
    {
        return std::nullopt;
    }

    int result = value;
    auto policyCap = GroupPolicies().GetValue<ValuePolicy::MaxMemoryMb>();
    if (policyCap.has_value() && result > static_cast<int>(policyCap.value()))
    {
        result = static_cast<int>(policyCap.value());
    }

    return result;
}

// BootTimeoutMs: must be > 0.
std::optional<int> SettingMapping<Setting::BootTimeoutMs>::Validate(const int& value)
{
    return value > 0 ? std::optional<int>(value) : std::nullopt;
}

// MaximumStorageSizeMb: must be > 0.
std::optional<int> SettingMapping<Setting::MaximumStorageSizeMb>::Validate(const int& value)
{
    return value > 0 ? std::optional<int>(value) : std::nullopt;
}

// NetworkingMode: "nat" or "none" (case-insensitive).
std::optional<SessionNetworkingMode> SettingMapping<Setting::NetworkingMode>::Validate(const std::string& value)
{
    // Policy override (exact value, not cap).
    auto policyOverride = GroupPolicies().GetValue<ValuePolicy::DefaultNetworkingMode>();
    if (policyOverride.has_value())
    {
        return static_cast<SessionNetworkingMode>(policyOverride.value());
    }

    if (!GroupPolicies().IsEnabled(TogglePolicy::AllowCustomNetworkingMode))
    {
        return std::nullopt; // Policy blocks custom networking mode.
    }

    auto lower = value;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    if (lower == "nat")
    {
        return SessionNetworkingMode::Nat;
    }

    if (lower == "none")
    {
        return SessionNetworkingMode::None;
    }

    return std::nullopt;
}

// StoragePath: must be a non-empty absolute path.
std::optional<std::filesystem::path> SettingMapping<Setting::StoragePath>::Validate(const std::string& value)
{
    if (value.empty())
    {
        return std::nullopt;
    }

    std::filesystem::path p = wsl::shared::string::MultiByteToWide(value);
    return p.is_absolute() ? std::optional<std::filesystem::path>(p) : std::nullopt;
}

// Specialization for StoragePath default (runtime-computed).
template <>
std::filesystem::path UserSettings::GetDefault<Setting::StoragePath>() const
{
    return wsl::windows::common::filesystem::GetLocalAppDataPath(nullptr) / L"wsla";
}

} // namespace wsl::windows::wslc::settings
