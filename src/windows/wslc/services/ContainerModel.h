/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ContainerModel.h

Abstract:

    This file contains the ContainerModel definitions

--*/

#pragma once

#include <wslservice.h>
#include <wslc.h>
#include <algorithm>
#include <cwctype>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wsl::windows::wslc::models {

namespace details {
inline std::wstring ToWideString(std::wstring_view value)
{
    return {value.data(), value.size()};
}

inline std::vector<std::wstring_view> SplitPreserveEmpty(std::wstring_view value, wchar_t delimiter)
{
    std::vector<std::wstring_view> parts;
    size_t start = 0;
    while (start <= value.size())
    {
        const auto end = value.find(delimiter, start);
        if (end == std::wstring_view::npos)
        {
            parts.emplace_back(value.substr(start));
            break;
        }

        parts.emplace_back(value.substr(start, end - start));
        start = end + 1;
    }

    return parts;
}
} // namespace details

// Valid formats for container list output.
enum class FormatType
{
    Table,
    Json,
};

enum class NetworkArgumentParseError
{
    None,
    EmptyNetworkName,
    EmptyAlias,
    DuplicateNetworkName,
    UnsupportedOption,
};

struct ParsedNetworkArgument
{
    std::wstring Name;
    std::vector<std::wstring> Aliases;
    NetworkArgumentParseError Error = NetworkArgumentParseError::None;
    std::wstring ErrorValue;
};

struct ContainerNetwork
{
    std::string Name;
    std::vector<std::string> Aliases;
};

inline ParsedNetworkArgument ParseNetworkArgument(std::wstring_view value)
{
    ParsedNetworkArgument result;

    auto parseOptions = [&](std::wstring_view options, bool requireName) {
        bool parsedName = false;
        for (const auto part : details::SplitPreserveEmpty(options, L','))
        {
            const auto separator = part.find(L'=');
            if (separator == std::wstring_view::npos)
            {
                result.Error = NetworkArgumentParseError::UnsupportedOption;
                result.ErrorValue = details::ToWideString(part);
                return;
            }

            const auto key = part.substr(0, separator);
            const auto optionValue = part.substr(separator + 1);
            if (key == L"name")
            {
                if (parsedName)
                {
                    result.Error = NetworkArgumentParseError::DuplicateNetworkName;
                    result.ErrorValue = details::ToWideString(key);
                    return;
                }

                parsedName = true;
                result.Name = details::ToWideString(optionValue);
            }
            else if (key == L"alias")
            {
                result.Aliases.emplace_back(details::ToWideString(optionValue));
            }
            else
            {
                result.Error = NetworkArgumentParseError::UnsupportedOption;
                result.ErrorValue = details::ToWideString(key);
                return;
            }
        }

        if (requireName && !parsedName)
        {
            result.Error = NetworkArgumentParseError::EmptyNetworkName;
        }
    };

    if (value.starts_with(L"name="))
    {
        parseOptions(value, true);
    }
    else
    {
        result.Name = details::ToWideString(value);
    }

    if (result.Error == NetworkArgumentParseError::None)
    {
        const auto nameIsEmpty =
            result.Name.empty() ||
            std::all_of(result.Name.begin(), result.Name.end(), [](wchar_t c) {
                return std::iswspace(static_cast<wint_t>(c));
            });
        if (nameIsEmpty)
        {
            result.Error = NetworkArgumentParseError::EmptyNetworkName;
            return result;
        }

        for (const auto& alias : result.Aliases)
        {
            const auto aliasIsEmpty =
                alias.empty() ||
                std::all_of(alias.begin(), alias.end(), [](wchar_t c) {
                    return std::iswspace(static_cast<wint_t>(c));
                });
            if (aliasIsEmpty)
            {
                result.Error = NetworkArgumentParseError::EmptyAlias;
                return result;
            }
        }
    }

    return result;
}

struct ContainerOptions
{
    std::vector<std::string> Arguments;
    std::vector<std::string> EnvironmentVariables;
    bool Detach = false;
    bool Interactive = false;
    std::string Name;
    bool Remove = false;
    bool TTY = false;
    bool PublishAll = false;
    WSLCSignal StopSignal = WSLCSignalNone;
    std::optional<int> StopTimeout{};
    std::optional<int64_t> ShmSize{};
    bool Gpu = false;
    std::vector<std::string> Ports;
    std::vector<std::wstring> Volumes;
    std::string WorkingDirectory;
    std::vector<std::string> Entrypoint;
    std::optional<std::string> User{};
    std::optional<std::string> Hostname{};
    std::optional<std::string> Domainname{};
    std::vector<std::string> DnsServers;
    std::vector<std::string> DnsSearchDomains;
    std::vector<std::string> DnsOptions;
    std::vector<ContainerNetwork> Networks;
    std::vector<std::string> NetworkAliases;
    std::vector<std::string> Tmpfs;
    std::vector<std::pair<std::string, std::string>> Labels;
    std::optional<std::wstring> CidFile{};
    std::optional<int64_t> MemoryBytes{};
    std::optional<int64_t> NanoCpus{};
    std::vector<std::tuple<std::string, int64_t, int64_t>> Ulimits;
};

struct CreateContainerResult
{
    std::string Id;
};

struct StopContainerOptions
{
    WSLCSignal Signal = WSLCSignalNone;
    LONG Timeout = WSLC_STOP_TIMEOUT_DEFAULT;
};

struct PruneContainersResult
{
    std::vector<std::string> PrunedContainers;
    ULONGLONG SpaceReclaimed{};
};

struct KillContainerOptions
{
    int Signal = WSLCSignalSIGKILL;
};

struct PortInformation
{
    uint16_t HostPort{};
    uint16_t ContainerPort{};
    int Protocol{}; // IP protocol number (e.g., IPPROTO_TCP or IPPROTO_UDP)
    std::string BindingAddress;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(PortInformation, HostPort, ContainerPort, Protocol, BindingAddress);
};

struct ContainerInformation
{
    std::string Id;
    std::string Name;
    std::string Image;
    WSLCContainerState State;
    ULONGLONG StateChangedAt{};
    ULONGLONG CreatedAt{};
    std::vector<PortInformation> Ports;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(ContainerInformation, Id, Name, Image, State, StateChangedAt, CreatedAt, Ports);
};

struct EnvironmentVariable
{
    static std::optional<std::wstring> Parse(const std::wstring& entry);
    static std::vector<std::wstring> ParseFile(const std::wstring& filePath);
};

struct PublishPort
{
    enum class Protocol
    {
        UDP,
        TCP,
    };

    struct PortRange
    {
        PortRange(uint16_t start, uint16_t end) : m_start(start), m_end(end)
        {
        }

        uint16_t Start() const
        {
            return m_start;
        }

        uint16_t End() const
        {
            return m_end;
        }

        constexpr uint16_t Count() const noexcept
        {
            return (m_end >= m_start) ? (m_end - m_start + 1) : 0;
        }

        constexpr bool IsSingle() const noexcept
        {
            return Count() == 1;
        }

        constexpr bool IsValid() const noexcept
        {
            return Count() > 0 && IsValidPort(m_start) && IsValidPort(m_end);
        }

        constexpr bool IsEphemeral() const noexcept
        {
            return m_start == WSLC_EPHEMERAL_PORT && m_end == WSLC_EPHEMERAL_PORT;
        }

        static PublishPort::PortRange ParsePortPart(const std::string& portPart);
        static PublishPort::PortRange Ephemeral() noexcept
        {
            return {WSLC_EPHEMERAL_PORT, WSLC_EPHEMERAL_PORT};
        }

    private:
        uint16_t m_start{};
        uint16_t m_end{};
    };

    struct IPAddress
    {
        explicit IPAddress(std::string ip) : m_ip(std::move(ip))
        {
            if (!m_ip.empty() && m_ip.front() == '[' && m_ip.back() == ']')
            {
                m_isIPv6 = true;
                m_ip = m_ip.substr(1, m_ip.size() - 2);
            }
        }

        std::string IP() const
        {
            return m_ip;
        }

        bool IsIPv6() const
        {
            return m_isIPv6;
        }

    private:
        bool m_isIPv6 = false;
        std::string m_ip{};
    };

    static constexpr uint16_t MAX_PORT = std::numeric_limits<uint16_t>::max();
    static constexpr uint16_t MIN_PORT = 1;

    std::optional<IPAddress> HostIP() const noexcept
    {
        return m_hostIP;
    }

    PortRange HostPort() const noexcept
    {
        return m_hostPort;
    }

    PortRange ContainerPort() const noexcept
    {
        return m_containerPort;
    }

    Protocol PortProtocol() const noexcept
    {
        return m_protocol;
    }

    std::string Original() const noexcept
    {
        return m_original;
    }

    bool IsRangeMapping() const noexcept
    {
        return !m_containerPort.IsSingle();
    }

    static PublishPort Parse(const std::string& value);

private:
    std::optional<IPAddress> m_hostIP;
    PortRange m_hostPort = PortRange::Ephemeral();
    PortRange m_containerPort = PortRange::Ephemeral();
    Protocol m_protocol = Protocol::TCP;
    std::string m_original;
    void Validate() const;
    PublishPort() = default;
    static constexpr bool IsValidPort(unsigned long port) noexcept
    {
        return port >= MIN_PORT && port <= MAX_PORT;
    }
};

struct VolumeMount
{
    std::wstring Host() const
    {
        return m_host;
    }

    std::string ContainerPath() const
    {
        return m_containerPath;
    }

    bool IsReadOnly() const
    {
        return m_isReadOnlyMode;
    }

    bool IsNamedVolume() const
    {
        return m_isNamedVolume;
    }

    static bool IsValidNamedVolumeName(const std::wstring& name);

    static VolumeMount Parse(const std::wstring& value);

private:
    std::wstring m_host;
    std::string m_containerPath;
    bool m_isReadOnlyMode = false;
    bool m_isNamedVolume = false;

    static bool IsReadOnlyMode(const std::wstring& mode)
    {
        return mode == L"ro";
    }

    static bool IsValidMode(const std::wstring& mode)
    {
        return IsReadOnlyMode(mode) || mode == L"rw";
    }
};

struct TmpfsMount
{
    std::string ContainerPath() const
    {
        return m_containerPath;
    }
    std::string Options() const
    {
        return m_options;
    }
    static TmpfsMount Parse(const std::string& value);

private:
    std::string m_containerPath;
    std::string m_options;
};

class CidFile
{
public:
    explicit CidFile(const std::optional<std::wstring>& path);
    ~CidFile();

    NON_COPYABLE(CidFile);
    NON_MOVABLE(CidFile);

    void Commit(const std::string& containerId);

private:
    std::optional<std::wstring> m_path{};
    wil::unique_hfile m_file;
    bool m_committed = false;
};
} // namespace wsl::windows::wslc::models
