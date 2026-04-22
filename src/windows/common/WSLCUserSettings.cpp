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
    "# All settings support string value \"default\" which use built-in defaults.\n"
    "\n"
    "session:\n"
    "  # Number of virtual CPUs allocated to the session (e.g. 4 default: all available CPUs)\n"
    "  # cpuCount: default\n"
    "\n"
    "  # Memory limit for the session (e.g. 2GB default: half of available memory)\n"
    "  # memorySize: default\n"
    "\n"
    "  # Maximum disk image size (default: 1TB)\n"
    "  # maxStorageSize: 1TB\n"
    "\n"
    "  # Default path for session storage. By default, storage is per-session under:\n"
    "  #   %LocalAppData%\\wslc\\sessions\\wslc-cli-<user>        (standard sessions)\n"
    "  #   %LocalAppData%\\wslc\\sessions\\wslc-cli-admin-<user>  (elevated sessions)\n"
    "  # defaultStoragePath: default\n"
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

        // Check "default"
        try
        {
            if (node->IsScalar() && node->as<std::string>() == "default")
            {
                return;
            }
        }
        catch (...)
        {
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

    // Collects the set of known dot-separated YAML paths from all SettingMapping specializations.
    template <size_t... S>
    std::set<std::string> CollectKnownPaths(std::index_sequence<S...>)
    {
        std::set<std::string> paths;
        (paths.insert(std::string(details::SettingMapping<static_cast<Setting>(S)>::YamlPath)), ...);
        return paths;
    }

    // Derives the set of all prefixes from the known paths.
    // e.g. "a.b.c" contributes both "a" and "a.b" as known prefixes.
    std::set<std::string> CollectKnownPrefixes(const std::set<std::string>& knownPaths)
    {
        std::set<std::string> prefixes;
        for (const auto& path : knownPaths)
        {
            for (size_t pos = path.find('.'); pos != std::string::npos; pos = path.find('.', pos + 1))
            {
                prefixes.insert(path.substr(0, pos));
            }
        }
        return prefixes;
    }

    // Iteratively walks the YAML tree and warns about keys not in the known set.
    void WarnUnknownKeys(const YAML::Node& root, const std::set<std::string>& knownPaths, const std::set<std::string>& knownPrefixes, std::vector<Warning>& warnings)
    {
        // Stack of (node, prefix) pairs to process.
        std::vector<std::pair<YAML::Node, std::string>> stack;
        stack.emplace_back(root, std::string{});

        while (!stack.empty())
        {
            auto [node, prefix] = std::move(stack.back());
            stack.pop_back();

            for (auto it = node.begin(); it != node.end(); ++it)
            {
                std::string key;
                try
                {
                    key = it->first.as<std::string>();
                }
                catch (...)
                {
                    auto location = prefix.empty() ? std::wstring(L"root") : MultiByteToWide(prefix);
                    warnings.push_back({std::format(L"Warning: Non-string key in section '{}'.", location), location});
                    continue;
                }

                auto fullPath = prefix.empty() ? key : prefix + '.' + key;

                if (it->second.IsMap())
                {
                    if (knownPrefixes.count(fullPath))
                    {
                        // Known section — add to stack to traverse.
                        stack.emplace_back(it->second, fullPath);
                    }
                    else
                    {
                        // Unknown section — warn once, don't traverse.
                        const auto widePath = MultiByteToWide(fullPath);
                        warnings.push_back({std::format(L"Warning: Unknown setting section '{}'.", widePath), widePath});
                    }
                }
                else if (!knownPaths.count(fullPath) && !knownPrefixes.count(fullPath))
                {
                    // Unknown setting
                    const auto widePath = MultiByteToWide(fullPath);
                    warnings.push_back({std::format(L"Warning: Unknown setting '{}'.", widePath), widePath});
                }
            }
        }
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

        if (root->IsMap())
        {
            constexpr auto settingCount = static_cast<size_t>(Setting::Max);
            ValidateAll(root.value(), m_settings, m_warnings, std::make_index_sequence<settingCount>());

            constexpr auto indexSeq = std::make_index_sequence<settingCount>();
            auto knownPaths = CollectKnownPaths(indexSeq);
            auto knownPrefixes = CollectKnownPrefixes(knownPaths);
            WarnUnknownKeys(root.value(), knownPaths, knownPrefixes, m_warnings);
        }
        else
        {
            m_warnings.push_back(
                {std::format(
                     L"Warning: '{}' is empty or has invalid structure. Expected a YAML mapping.", m_settingsPath.filename().wstring()),
                 {}});
        }
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
