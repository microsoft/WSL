/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCUserSettings.cpp

Abstract:

    Implementation of UserSettings — YAML loading and validation.

--*/
#include "precomp.h"
#include "WSLCUserSettings.h"
#include "filesystem.hpp"
#include "string.hpp"
#include "wslutil.h"

#pragma warning(push)
#pragma warning(disable : 4251 4275)
#include <yaml-cpp/yaml.h>
#pragma warning(pop)
#include <algorithm>
#include <format>
#include <fstream>
#include <set>

using namespace wsl::windows::common::string;

namespace wsl::windows::wslc::settings {

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
    "  # Memory limit for the session in megabytes (default: 2GB)\n"
    "  # memorySize: 2GB\n"
    "\n"
    "  # Maximum disk image size in megabytes (default: 100GB)\n"
    "  # maxStorageSize: 100GB\n"
    "\n"
    "  # Default path for session storage. By default, storage is per-session under:\n"
    "  #   %LocalAppData%\\wslc\\sessions\\wslc-cli        (standard sessions)\n"
    "  #   %LocalAppData%\\wslc\\sessions\\wslc-cli-admin (elevated sessions)\n"
    "  # defaultStoragePath: \"\"\n"
    "\n"
    "# Credential storage backend: \"wincred\" or \"file\" (default: wincred)\n"
    "# credentialStore: wincred\n";

// Validate individual setting specializations
namespace details {

    std::optional<uint32_t> ParseSettingsMemoryValue(const std::string& value)
    {
        auto parsed = wsl::shared::string::ParseMemorySize(value.c_str());
        auto converted = parsed.has_value() ? *parsed / _1MB : 0; // To Mb, and anything less than 1Mb is considered invalid.
        return converted > 0 ? std::optional{static_cast<uint32_t>(converted)} : std::nullopt;
    }

#define WSLC_VALIDATE_SETTING(_setting_) \
    std::optional<SettingMapping<Setting::_setting_>::value_t> SettingMapping<Setting::_setting_>::Validate( \
        const SettingMapping<Setting::_setting_>::yaml_t& value)

    WSLC_VALIDATE_SETTING(SessionCpuCount)
    {
        return value > 0 ? std::optional{value} : std::nullopt;
    }

    WSLC_VALIDATE_SETTING(SessionMemoryMb)
    {
        return ParseSettingsMemoryValue(value);
    }

    WSLC_VALIDATE_SETTING(SessionStorageSizeMb)
    {
        return ParseSettingsMemoryValue(value);
    }

    // yaml_t = std::string (UTF-8 from yaml-cpp), value_t = std::wstring
    WSLC_VALIDATE_SETTING(SessionStoragePath)
    {
        return MultiByteToWide(value);
    }

    WSLC_VALIDATE_SETTING(SessionNetworkingMode)
    {
        if (value == "none")
        {
            return WSLCNetworkingModeNone;
        }
        if (value == "nat")
        {
            return WSLCNetworkingModeNAT;
        }
        if (value == "virtioproxy")
        {
            return WSLCNetworkingModeVirtioProxy;
        }

        return std::nullopt;
    }

    WSLC_VALIDATE_SETTING(SessionHostFileShareMode)
    {
        if (value == "plan9")
        {
            return HostFileShareMode::Plan9;
        }
        if (value == "virtiofs")
        {
            return HostFileShareMode::VirtioFs;
        }

        return std::nullopt;
    }

    WSLC_VALIDATE_SETTING(SessionDnsTunneling)
    {
        return value;
    }

    WSLC_VALIDATE_SETTING(CredentialStore)
    {
        if (value == "wincred")
        {
            return CredentialStoreType::WinCred;
        }
        if (value == "file")
        {
            return CredentialStoreType::File;
        }

        return std::nullopt;
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
        catch (...)
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
            auto err = errno;
            // If the file exists but cannot be opened (permissions, sharing violation, etc.),
            // emit a warning so the user understands why settings were ignored.
            if (err != ENOENT)
            {
                warnings.push_back(
                    {std::format(L"Warning: Failed to open '{}', errno: {}. Using default settings.", path.filename().wstring(), err), {}});
            }

            return std::nullopt;
        }

        try
        {
            return YAML::Load(stream);
        }
        catch (const std::exception& e)
        {
            warnings.push_back(
                {std::format(L"Warning: '{}' could not be parsed: {}.", path.filename().wstring(), MultiByteToWide(e.what())), {}});
            return std::nullopt;
        }
    }

    const std::filesystem::path& SettingsDir()
    {
        static const std::filesystem::path dir = wsl::windows::common::filesystem::GetLocalAppDataPath(nullptr) / L"wslc";
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
    m_settingsPath = settingsDir / L"settings.yaml";

    auto root = TryLoadYaml(m_settingsPath, m_warnings);
    if (root.has_value())
    {
        m_type = UserSettingsType::Standard;
    }

    if (root.has_value())
    {
        constexpr auto settingCount = static_cast<size_t>(Setting::Max);
        ValidateAll(root.value(), m_settings, m_warnings, std::make_index_sequence<settingCount>());

        // TODO: Iterate through all nodes and warn about unknown keys?
    }

    // Emit any settings load warnings.
    for (const auto& warning : m_warnings)
    {
        wsl::windows::common::wslutil::PrintMessage(warning.Message, stderr);
    }
}

void UserSettings::Reset() const
{
    std::filesystem::create_directories(m_settingsPath.parent_path());
    std::ofstream file(m_settingsPath);
    THROW_HR_IF_MSG(E_UNEXPECTED, !file.is_open(), "Failed to create settings file");
    file << s_DefaultSettingsTemplate;
}

void UserSettings::PrepareToShellExecuteFile() const
{
    if (m_type == UserSettingsType::Default && !std::filesystem::exists(m_settingsPath))
    {
        // First run — create the directory and write the commented-out defaults template.
        Reset();
    }
}

std::filesystem::path UserSettings::SettingsFilePath() const
{
    return m_settingsPath;
}

} // namespace wsl::windows::wslc::settings
