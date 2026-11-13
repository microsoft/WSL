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
#include <cctype>
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
    "# All settings support string value \"default\" which uses built-in defaults.\n"
    "\n"
    "session:\n"
    "  # Number of virtual CPUs allocated to the session (e.g. 4 default: all available CPUs)\n"
    "  # cpuCount: default\n"
    "\n"
    "  # Memory limit for the session (e.g. 2GB default: half of available memory)\n"
    "  # memorySize: default\n"
    "\n"
    "  # Maximum disk image size (e.g. 500GB default: 1TB)\n"
    "  # maxStorageSize: default\n"
    "\n"
    "  # Base directory for the default session's storage; the session VHD is created at\n"
    "  # <storagePath>\\wslc\\sessions\\<session>\\storage.vhdx. Must be an absolute path (e.g. D:\\data default: "
    "%LOCALAPPDATA%). Changing this after a session already exists does not move existing storage, containers, or\n"
    "  # images; the previous location is left in place and a new empty session is created at the new path.\n"
    "  # storagePath: default\n"
    "\n"
    "  # Default host address that published ports bind to when 'container run -p' is\n"
    "  # used without an explicit address (default: 127.0.0.1)\n"
    "  # defaultBindingAddress: default\n"
    "\n"
    "  # DNS name that resolves to the host loopback address (default: host.wslc.internal).\n"
    "  # Set to \"none\" to disable the entry.\n"
    "  # hostLoopback: default\n"
    "\n"
    "  # Seconds an idle session VM stays running before it is torn down (default: 30)\n"
    "  # idleTimeout: default\n"
    "\n"
    "  # Container registry configuration, merged into the session's /etc/docker/daemon.json.\n"
    "  # Changes apply the next time the session starts.\n"
    "  #   mirrors            - image pull mirrors, like \"registry-mirrors\" in daemon.json\n"
    "  #   insecureRegistries - registries served over plain HTTP or with an untrusted certificate,\n"
    "  #                        like \"insecure-registries\" in daemon.json\n"
    "  # registry:\n"
    "  #   mirrors:\n"
    "  #     - https://mirror.example.com\n"
    "  #   insecureRegistries:\n"
    "  #     - registry.example.internal:5000\n"
    "\n"
    "# Credential storage backend: \"wincred\" or \"file\" (default: wincred)\n"
    "# credentialStore: wincred\n";

// Validate individual setting specializations
namespace {

    bool ContainsWhitespace(const std::string& value)
    {
        return value.find_first_of(" \t\r\n\f\v") != std::string::npos;
    }

    bool StartsWithIgnoreCase(std::string_view value, std::string_view prefix)
    {
        if (value.size() < prefix.size())
        {
            return false;
        }

        for (size_t i = 0; i < prefix.size(); i++)
        {
            if (std::tolower(static_cast<unsigned char>(value[i])) != std::tolower(static_cast<unsigned char>(prefix[i])))
            {
                return false;
            }
        }

        return true;
    }

    bool IsValidIpLiteral(std::string_view value)
    {
        const std::string text{value};

        in_addr address4{};
        if (inet_pton(AF_INET, text.c_str(), &address4) == 1)
        {
            return true;
        }

        in6_addr address6{};
        return inet_pton(AF_INET6, text.c_str(), &address6) == 1;
    }

    // Matches the hostnames dockerd accepts for a registry endpoint: dot-separated labels of
    // alphanumeric characters and hyphens, without leading or trailing hyphens.
    bool IsValidHostname(std::string_view value)
    {
        if (value.empty() || value.size() > 253)
        {
            return false;
        }

        size_t labelStart = 0;
        while (true)
        {
            const auto dot = value.find('.', labelStart);
            const auto label = value.substr(labelStart, dot == std::string_view::npos ? std::string_view::npos : dot - labelStart);
            if (label.empty() || label.size() > 63 || label.front() == '-' || label.back() == '-')
            {
                return false;
            }

            for (const auto character : label)
            {
                if (!std::isalnum(static_cast<unsigned char>(character)) && character != '-')
                {
                    return false;
                }
            }

            if (dot == std::string_view::npos)
            {
                return true;
            }

            labelStart = dot + 1;
        }
    }

    // dockerd parses the port with strconv.Atoi and rejects anything outside [0, 65535].
    bool IsValidPort(std::string_view value)
    {
        const auto firstDigit = value.find_first_not_of('0');
        if (firstDigit == std::string_view::npos)
        {
            // "0" and zero-padded variants are valid.
            return !value.empty();
        }

        const auto digits = value.substr(firstDigit);
        if (digits.size() > 5)
        {
            return false;
        }

        uint32_t port = 0;
        for (const auto character : digits)
        {
            if (!std::isdigit(static_cast<unsigned char>(character)))
            {
                return false;
            }

            port = port * 10 + static_cast<uint32_t>(character - '0');
        }

        return port <= 65535;
    }

    // Accepts the endpoint forms dockerd supports: "host", "host:port", an IPv4/IPv6 literal,
    // or a bracketed IPv6 literal with an optional port.
    bool IsValidHostPort(std::string_view value)
    {
        if (value.empty())
        {
            return false;
        }

        if (value.front() == '[')
        {
            const auto closingBracket = value.find(']');
            if (closingBracket == std::string_view::npos)
            {
                return false;
            }

            in6_addr address{};
            const std::string literal{value.substr(1, closingBracket - 1)};
            if (inet_pton(AF_INET6, literal.c_str(), &address) != 1)
            {
                return false;
            }

            const auto remainder = value.substr(closingBracket + 1);
            return remainder.empty() || (remainder.front() == ':' && IsValidPort(remainder.substr(1)));
        }

        // A bare address literal can contain colons of its own (IPv6 without brackets).
        if (IsValidIpLiteral(value))
        {
            return true;
        }

        const auto colon = value.rfind(':');
        if (colon == std::string_view::npos)
        {
            return IsValidHostname(value);
        }

        return IsValidHostname(value.substr(0, colon)) && IsValidPort(value.substr(colon + 1));
    }

    // Accepts the CIDR blocks dockerd supports: "<ipv4>/<0-32>" or "<ipv6>/<0-128>".
    bool IsValidCidr(std::string_view value)
    {
        const auto slash = value.rfind('/');
        if (slash == std::string_view::npos || slash == 0 || slash + 1 == value.size())
        {
            return false;
        }

        const auto prefixText = value.substr(slash + 1);
        if (prefixText.size() > 3)
        {
            return false;
        }

        uint32_t prefix = 0;
        for (const auto character : prefixText)
        {
            if (!std::isdigit(static_cast<unsigned char>(character)))
            {
                return false;
            }

            prefix = prefix * 10 + static_cast<uint32_t>(character - '0');
        }

        const std::string address{value.substr(0, slash)};

        in_addr address4{};
        if (inet_pton(AF_INET, address.c_str(), &address4) == 1)
        {
            return prefix <= 32;
        }

        in6_addr address6{};
        if (inet_pton(AF_INET6, address.c_str(), &address6) == 1)
        {
            return prefix <= 128;
        }

        return false;
    }

    // dockerd rejects the whole daemon.json (and refuses to start) when a mirror is not an absolute
    // http(s) URL, so validate the same shape here and let the caller fall back to the default.
    bool IsValidRegistryMirror(const std::string& value)
    {
        if (value.empty() || ContainsWhitespace(value))
        {
            return false;
        }

        auto authority = std::string_view{value};
        if (StartsWithIgnoreCase(authority, "https://"))
        {
            authority.remove_prefix(8);
        }
        else if (StartsWithIgnoreCase(authority, "http://"))
        {
            authority.remove_prefix(7);
        }
        else
        {
            return false;
        }

        // dockerd rejects fragments, query strings and credentials in a mirror URI.
        if (authority.find_first_of("?#@") != std::string_view::npos)
        {
            return false;
        }

        if (!authority.empty() && authority.back() == '/')
        {
            authority.remove_suffix(1);
        }

        // Mirrors are registry roots: a path is not a valid endpoint.
        return !authority.empty() && authority.find('/') == std::string_view::npos && IsValidHostPort(authority);
    }

    // dockerd validates insecure registries as "host", "host:port" or a CIDR block and refuses to
    // start when one entry is malformed, so apply the same rules here.
    bool IsValidInsecureRegistry(const std::string& value)
    {
        if (value.empty() || ContainsWhitespace(value) || value.find("://") != std::string::npos ||
            value.find_first_of("\"'\\") != std::string::npos)
        {
            return false;
        }

        // Anything with a '/' must be a CIDR block; dockerd rejects a path after a host.
        if (value.find('/') != std::string::npos)
        {
            return IsValidCidr(value);
        }

        return IsValidHostPort(value);
    }

} // namespace

namespace details {

    std::optional<uint32_t> ParseSettingsMemoryValue(const std::string& value)
    {

        // WSLC settings accept leading whitespace for compatibility with existing settings files.
        const auto wideValue = MultiByteToWide(value);
        const auto parsed = ParseStorageSize(StripLeadingWhitespace(wideValue), StorageSizeUnit::Binary);
        const auto converted = parsed.has_value() ? *parsed / _1MB : 0;
        if (converted == 0 || converted > std::numeric_limits<uint32_t>::max())
        {
            return std::nullopt;
        }

        return static_cast<uint32_t>(converted);
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
        if (value == "consomme")
        {
            return WSLCNetworkingModeConsomme;
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

    WSLC_VALIDATE_SETTING(SessionHostLoopback)
    {
        if (value == "none")
        {
            return std::string{};
        }

        return !value.empty() ? std::optional{value} : std::nullopt;
    }

    WSLC_VALIDATE_SETTING(SessionPortRelay)
    {
        if (value == "virtionet")
        {
            return PortRelayType::VirtioNet;
        }
        if (value == "wslrelay")
        {
            return PortRelayType::WslRelay;
        }

        return std::nullopt;
    }

    WSLC_VALIDATE_SETTING(SessionDefaultBindingAddress)
    {
        // The default binding address only applies to IPv4 ports (IPv6 bindings are always
        // explicit), so it must parse as a valid IPv4 literal.
        in_addr address{};
        if (inet_pton(AF_INET, value.c_str(), &address) != 1)
        {
            return std::nullopt;
        }

        return value;
    }

    WSLC_VALIDATE_SETTING(SessionStoragePath)
    {
        if (value.empty() || !std::filesystem::path(value).is_absolute())
        {
            return std::nullopt;
        }

        return value;
    }

    WSLC_VALIDATE_SETTING(SessionIdleTimeout)
    {
        return value > 0 ? std::optional{value} : std::nullopt;
    }

    WSLC_VALIDATE_SETTING(SessionRegistryMirrors)
    {
        for (const auto& mirror : value)
        {
            if (!IsValidRegistryMirror(mirror))
            {
                return std::nullopt;
            }
        }

        return value;
    }

    WSLC_VALIDATE_SETTING(SessionRegistryInsecure)
    {
        for (const auto& registry : value)
        {
            if (!IsValidInsecureRegistry(registry))
            {
                return std::nullopt;
            }
        }

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
    void ValidateSetting(const YAML::Node& root, SettingsMap& map, const std::wstring& filePath, std::vector<Warning>& warnings)
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
                warnings.push_back(
                    {wsl::shared::Localization::WSLCUserSettings_Warning_InvalidValue(widePath, filePath, node->Mark().line + 1), widePath});
            }
        }
        catch (...)
        {
            const auto widePath = MultiByteToWide(path);
            warnings.push_back(
                {wsl::shared::Localization::WSLCUserSettings_Warning_InvalidType(widePath, filePath, node->Mark().line + 1), widePath});
        }
    }

    // Validates all settings via a fold over the Setting enum index sequence.
    template <size_t... S>
    void ValidateAll(const YAML::Node& root, SettingsMap& map, const std::wstring& filePath, std::vector<Warning>& warnings, std::index_sequence<S...>)
    {
        (ValidateSetting<static_cast<Setting>(S)>(root, map, filePath, warnings), ...);
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
    void WarnUnknownKeys(
        const YAML::Node& root,
        const std::set<std::string>& knownPaths,
        const std::set<std::string>& knownPrefixes,
        const std::wstring& filePath,
        std::vector<Warning>& warnings)
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
                    warnings.push_back(
                        {wsl::shared::Localization::WSLCUserSettings_Warning_NonStringKey(location, filePath, it->first.Mark().line + 1), location});
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
                        warnings.push_back(
                            {wsl::shared::Localization::WSLCUserSettings_Warning_UnknownSection(
                                 widePath, filePath, it->first.Mark().line + 1),
                             widePath});
                    }
                }
                else if (!knownPaths.count(fullPath) && !knownPrefixes.count(fullPath))
                {
                    // Unknown setting
                    const auto widePath = MultiByteToWide(fullPath);
                    warnings.push_back(
                        {wsl::shared::Localization::WSLCUserSettings_Warning_UnknownKey(widePath, filePath, it->first.Mark().line + 1), widePath});
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
                warnings.push_back({wsl::shared::Localization::WSLCUserSettings_Warning_FailedToOpen(path.wstring(), err), {}});
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
                {wsl::shared::Localization::WSLCUserSettings_Warning_ParseError(path.wstring(), MultiByteToWide(e.what())), {}});
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
        const auto filePath = m_settingsPath.wstring();

        if (root->IsMap())
        {
            constexpr auto settingCount = static_cast<size_t>(Setting::Max);
            ValidateAll(root.value(), m_settings, filePath, m_warnings, std::make_index_sequence<settingCount>());

            constexpr auto indexSeq = std::make_index_sequence<settingCount>();
            auto knownPaths = CollectKnownPaths(indexSeq);
            auto knownPrefixes = CollectKnownPrefixes(knownPaths);
            WarnUnknownKeys(root.value(), knownPaths, knownPrefixes, filePath, m_warnings);
        }
        else
        {
            m_warnings.push_back({wsl::shared::Localization::WSLCUserSettings_Warning_InvalidStructure(filePath), {}});
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
