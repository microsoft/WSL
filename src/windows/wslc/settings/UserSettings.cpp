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

// Default settings file template — written on first run.
// All entries are commented out; the values shown are the built-in defaults.
// TODO: localization for comments needed?
static constexpr std::string_view s_DefaultSettingsTemplate =
    "# wslc user settings\n"
    "# https://aka.ms/wslc-settings\n"
    "\n"
    "session:\n"
    "  # Number of virtual CPUs allocated to the session (default: 4)\n"
    "  # cpuCount: 4\n"
    "\n"
    "  # Memory limit for the session in megabytes (default: 2048)\n"
    "  # memorySizeMb: 2048\n"
    "\n"
    "  # Maximum disk image size in megabytes (default: 10000)\n"
    "  # maxStorageSizeMb: 10000\n"
    "\n"
    "  # Default path for container storage (default: %LocalAppData%\\wslc\\storage)\n"
    "  # defaultStoragePath: \"\"\n";

// Validate individual setting specializations
namespace details {

#define WSLC_VALIDATE_SETTING(_setting_) \
    std::optional<SettingMapping<Setting::_setting_>::value_t> SettingMapping<Setting::_setting_>::Validate( \
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

// Helpers
namespace {

    // Traverses a dot-separated path (e.g. "session.cpuCount") through a YAML node tree.
    // Returns nullopt if any segment is invalid or missing.
    std::optional<YAML::Node> NavigateYamlPath(const YAML::Node& root, std::string_view path)
    {
        YAML::Node current = root;
        auto subPaths = wsl::shared::string::Split(std::string{path}, '.');
        for (auto const& subPath : subPaths)
        {
            if (current.IsDefined() && current.IsMap())
            {
                // Use the const operator[] to avoid yaml-cpp's AssignNode/set_ref side-effect,
                // which mutates the shared detail::node and corrupts subsequent lookups.
                // Then use reset() to rebind 'current' without triggering set_ref.
                auto child = static_cast<const YAML::Node&>(current)[subPath];
                if (!child.IsDefined())
                {
                    return std::nullopt;
                }
                current.reset(child);
            }
            else
            {
                return std::nullopt;
            }
        }
        return current;
    }

    // Validates and stores a single setting from the YAML document.
    template <Setting S>
    void ValidateSetting(const YAML::Node& root, SettingsMap& map, std::vector<Warning>& warnings)
    {
        constexpr auto path = details::SettingMapping<S>::YamlPath;
        auto node = NavigateYamlPath(root, path);

        if (!node || !node->IsDefined() || node->IsNull())
        {
            // Key absent — silently use the built-in default.
            return;
        }

        try
        {
            auto rawValue = node->as<typename details::SettingMapping<S>::yaml_t>();
            auto validated = details::SettingMapping<S>::Validate(rawValue);
            if (validated.has_value())
            {
                map.Add<S>(std::move(validated.value()));
            }
            else
            {
                const auto widePath = MultiByteToWide(path);
                warnings.push_back({std::format(L"Warning: Invalid value for setting '{}'. Using default.", widePath), widePath});
            }
        }
        catch (const YAML::Exception&)
        {
            const auto widePath = MultiByteToWide(path);
            warnings.push_back({std::format(L"Warning: Invalid type for setting '{}'. Using default.", widePath), widePath});
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
            warnings.push_back(
                {std::format(L"Warning: '{}' could not be parsed: {}.", path.filename().wstring(), MultiByteToWide(e.what())), {}});
            return std::nullopt;
        }
    }

    const std::filesystem::path& SettingsDir()
    {
        static const std::filesystem::path dir =
            wsl::windows::common::filesystem::GetLocalAppDataPath(nullptr) / L"wslc\\settings";
        return dir;
    }
} // namespace

UserSettings const& UserSettings::Instance()
{
    static UserSettings instance;
    return instance;
}

UserSettings::UserSettings() : UserSettings(SettingsDir())
{
}

UserSettings::UserSettings(const std::filesystem::path& settingsDir)
{
    m_primaryPath = settingsDir / L"UserSettings.yaml";
    m_backupPath = settingsDir / L"UserSettings.yaml.bak";

    // Try the primary file first.
    auto root = TryLoadYaml(m_primaryPath, m_warnings);
    if (root.has_value())
    {
        m_type = UserSettingsType::Standard;
    }
    else
    {
        // Primary missing or failed — try the backup.
        root = TryLoadYaml(m_backupPath, m_warnings);
        if (root.has_value())
        {
            m_type = UserSettingsType::Backup;
            m_warnings.push_back({L"Warning: UserSettings.yaml could not be loaded. Using backup settings.", {}});
        }
    }

    if (root.has_value())
    {
        constexpr auto settingCount = static_cast<size_t>(Setting::Max);
        ValidateAll(root.value(), m_settings, m_warnings, std::make_index_sequence<settingCount>());

        // TODO: Iterate through all nodes and warn about unknown keys?
    }
}

void UserSettings::Reset() const
{
    std::filesystem::create_directories(m_primaryPath.parent_path());
    std::ofstream file(m_primaryPath);
    THROW_HR_IF_MSG(E_UNEXPECTED, !file.is_open(), "Failed to create settings file");
    file << s_DefaultSettingsTemplate;
    file.flush();
    file.close();
}

void UserSettings::PrepareToShellExecuteFile() const
{
    if (m_type == UserSettingsType::Standard)
    {
        // Valid settings loaded — back them up before the user edits.
        std::filesystem::copy_file(m_primaryPath, m_backupPath, std::filesystem::copy_options::overwrite_existing);
    }
    else if (m_type == UserSettingsType::Default)
    {
        // First run — create the directory and write the commented-out defaults template.
        Reset();
    }
}

std::filesystem::path UserSettings::SettingsFilePath() const
{
    return m_primaryPath;
}

} // namespace wsl::windows::wslc::settings
