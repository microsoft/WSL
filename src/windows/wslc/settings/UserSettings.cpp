/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    UserSettings.cpp

Abstract:

    Implementation of UserSettings — YAML loading, validation, and fallback logic.

--*/
#include "UserSettings.h"
#include "filesystem.hpp"
#include "string.hpp"
#include <yaml-cpp/yaml.h>
#include <algorithm>
#include <format>
#include <fstream>
#include <set>

using namespace wsl::windows::common::string;

namespace wsl::windows::wslc::settings {

// ---------------------------------------------------------------------------
// Default settings file template — written on first run.
// All entries are commented out; the values shown are the built-in defaults.
// ---------------------------------------------------------------------------
static constexpr std::string_view s_DefaultSettingsTemplate =
    "# wslc user settings\n"
    "# https://aka.ms/wslc-settings\n"
    "\n"
    "session:\n"
    "  # Number of virtual CPUs allocated to the session (default: 4)\n"
    "  # cpuCount: 4\n"
    "\n"
    "  # Memory limit for the session in megabytes (default: 8192)\n"
    "  # memorySizeMb: 8192\n"
    "\n"
    "  # Maximum disk image size in megabytes (default: 51200)\n"
    "  # maxStorageSizeMb: 51200\n"
    "\n"
    "  # Default path for container storage (default: system managed)\n"
    "  # defaultStoragePath: \"\"\n";

// ---------------------------------------------------------------------------
// Validate specializations
// ---------------------------------------------------------------------------
namespace details {

#define WSLC_VALIDATE_SETTING(_setting_)                                                 \
    std::optional<SettingMapping<Setting::_setting_>::value_t>                           \
    SettingMapping<Setting::_setting_>::Validate(                                        \
        const SettingMapping<Setting::_setting_>::yaml_t& value)

WSLC_VALIDATE_SETTING(SessionCpuCount)
{
    return value > 0 ? std::optional{value} : std::nullopt;
}

WSLC_VALIDATE_SETTING(SessionMemoryMb)
{
    return value > 0 ? std::optional{value} : std::nullopt;
}

WSLC_VALIDATE_SETTING(SessionStorageSizeMb)
{
    return value > 0 ? std::optional{value} : std::nullopt;
}

// yaml_t = std::string (UTF-8 from yaml-cpp), value_t = std::wstring
WSLC_VALIDATE_SETTING(SessionStoragePath)
{
    return MultiByteToWide(value);
}

#undef WSLC_VALIDATE_SETTING

} // namespace details

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------
namespace {

// Traverses a dot-separated path (e.g. "session.cpuCount") through a YAML node tree.
// Returns an undefined node if any segment is missing.
YAML::Node NavigateYamlPath(const YAML::Node& root, std::string_view path)
{
    YAML::Node current = root;
    size_t start = 0;
    while (start < path.size() && current.IsDefined())
    {
        const size_t dot = path.find('.', start);
        const auto segment = std::string(path.substr(start, dot == std::string_view::npos ? dot : dot - start));
        current = current[segment];
        start = (dot == std::string_view::npos) ? path.size() : dot + 1;
    }
    return current;
}

// Builds the set of all known YAML paths from the SettingMapping specializations.
template <size_t... S>
std::set<std::string> BuildKnownPaths(std::index_sequence<S...>)
{
    return {std::string(details::SettingMapping<static_cast<Setting>(S)>::YamlPath)...};
}

// Recursively walks a YAML map node and warns about any key whose full dot-separated
// path is not a known setting path or a known intermediate prefix.
void WarnUnknownKeysInMap(
    const YAML::Node& node,
    const std::set<std::string>& knownPaths,
    const std::string& prefix,
    std::vector<Warning>& warnings)
{
    if (!node.IsMap())
    {
        return;
    }

    for (const auto& kv : node)
    {
        const auto key = kv.first.as<std::string>();
        const auto fullPath = prefix.empty() ? key : prefix + "." + key;

        const bool isKnownLeaf = knownPaths.count(fullPath) > 0;
        const bool isKnownPrefix = std::any_of(knownPaths.begin(), knownPaths.end(), [&](const std::string& p) {
            return p.starts_with(fullPath + ".");
        });

        if (!isKnownLeaf && !isKnownPrefix)
        {
            const auto widePath = MultiByteToWide(fullPath);
            warnings.push_back({std::format(L"Warning: Unknown settings key '{}'. Ignoring.", widePath), widePath});
        }
        else if (isKnownPrefix && kv.second.IsMap())
        {
            WarnUnknownKeysInMap(kv.second, knownPaths, fullPath, warnings);
        }
    }
}

// Validates and stores a single setting from the YAML document.
template <Setting S>
void ValidateSetting(const YAML::Node& root, SettingsMap& map, std::vector<Warning>& warnings)
{
    constexpr auto path = details::SettingMapping<S>::YamlPath;
    const YAML::Node node = NavigateYamlPath(root, path);

    if (!node.IsDefined() || node.IsNull())
    {
        // Key absent — silently use the built-in default.
        return;
    }

    try
    {
        auto rawValue = node.as<typename details::SettingMapping<S>::yaml_t>();
        auto validated = details::SettingMapping<S>::Validate(rawValue);
        if (validated.has_value())
        {
            map.Add<S>(std::move(validated.value()));
        }
        else
        {
            const auto widePath = MultiByteToWide(path);
            warnings.push_back(
                {std::format(L"Warning: Invalid value for setting '{}'. Using default.", widePath), widePath});
        }
    }
    catch (const YAML::Exception&)
    {
        const auto widePath = MultiByteToWide(path);
        warnings.push_back(
            {std::format(L"Warning: Invalid type for setting '{}'. Using default.", widePath), widePath});
    }
}

// Validates all settings via a fold over the Setting enum index sequence.
template <size_t... S>
void ValidateAll(const YAML::Node& root, SettingsMap& map, std::vector<Warning>& warnings, std::index_sequence<S...>)
{
    (ValidateSetting<static_cast<Setting>(S)>(root, map, warnings), ...);
}

// Attempts to parse a YAML document from the given file path.
// Returns an empty optional and pushes a warning if the file exists but fails to parse.
std::optional<YAML::Node> TryLoadYaml(const std::filesystem::path& path, std::vector<Warning>& warnings)
{
    std::ifstream stream(path);
    if (!stream.is_open())
    {
        return std::nullopt;
    }

    try
    {
        return YAML::Load(stream);
    }
    catch (const YAML::Exception& e)
    {
        warnings.push_back({std::format(L"Warning: '{}' could not be parsed: {}.",
                                        path.filename().wstring(),
                                        MultiByteToWide(e.what())),
                            {}});
        return std::nullopt;
    }
}

} // namespace

// ---------------------------------------------------------------------------
// UserSettings
// ---------------------------------------------------------------------------

// static
UserSettings const& UserSettings::Instance()
{
    static UserSettings instance;
    return instance;
}

// static
const std::filesystem::path& UserSettings::SettingsDir()
{
    static const std::filesystem::path dir = wsl::windows::common::filesystem::GetLocalAppDataPath(nullptr) / L"wslc";
    return dir;
}

// static
std::filesystem::path UserSettings::PrimaryFilePath()
{
    return SettingsDir() / L"UserSettings.yaml";
}

// static
std::filesystem::path UserSettings::BackupFilePath()
{
    return SettingsDir() / L"UserSettings.yaml.bak";
}

UserSettings::UserSettings()
{
    const auto primaryPath = PrimaryFilePath();
    const auto backupPath = BackupFilePath();

    // Try the primary file first.
    auto root = TryLoadYaml(primaryPath, m_warnings);
    if (root.has_value())
    {
        m_type = UserSettingsType::Standard;
    }
    else
    {
        // Primary missing or failed — try the backup.
        root = TryLoadYaml(backupPath, m_warnings);
        if (root.has_value())
        {
            m_type = UserSettingsType::Backup;
            m_warnings.push_back({L"Warning: UserSettings.yaml could not be loaded. Using backup settings.", {}});
        }
        // If neither file exists at all, emit no warning (first run).
    }

    if (root.has_value())
    {
        constexpr auto settingCount = static_cast<size_t>(Setting::Max);
        ValidateAll(root.value(), m_settings, m_warnings, std::make_index_sequence<settingCount>());

        const auto knownPaths = BuildKnownPaths(std::make_index_sequence<settingCount>());
        WarnUnknownKeysInMap(root.value(), knownPaths, {}, m_warnings);
    }
}

// static
void UserSettings::Reset()
{
    const auto primaryPath = PrimaryFilePath();
    std::filesystem::create_directories(primaryPath.parent_path());
    std::ofstream file(primaryPath);
    THROW_HR_IF_MSG(E_UNEXPECTED, !file.is_open(), "Failed to create settings file");
    file << s_DefaultSettingsTemplate;
}

void UserSettings::PrepareToShellExecuteFile() const
{
    const auto primaryPath = PrimaryFilePath();

    if (m_type == UserSettingsType::Standard)
    {
        // Valid settings loaded — back them up before the user edits.
        std::filesystem::copy_file(primaryPath, BackupFilePath(), std::filesystem::copy_options::overwrite_existing);
    }
    else if (m_type == UserSettingsType::Default)
    {
        // First run — create the directory and write the commented-out defaults template.
        std::filesystem::create_directories(primaryPath.parent_path());
        std::ofstream file(primaryPath);
        THROW_HR_IF_MSG(E_UNEXPECTED, !file.is_open(), "Failed to create settings file");
        file << s_DefaultSettingsTemplate;
    }
}

} // namespace wsl::windows::wslc::settings
