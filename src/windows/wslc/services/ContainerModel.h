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
#include <string>

namespace wsl::windows::wslc::models {

// Valid formats for container list output.
enum class FormatType
{
    Table,
    Json,
};

struct ContainerOptions
{
    std::vector<std::string> Arguments;
    std::vector<std::string> EnvironmentVariables;
    bool Detach = false;
    bool Interactive = false;
    std::string Name;
    bool Remove = false;
    bool TTY = false;
    std::vector<std::string> Ports;
    std::vector<std::wstring> Volumes;
    std::string WorkingDirectory;
    std::vector<std::string> Entrypoint;
    std::optional<std::string> User{};
    std::vector<std::string> Tmpfs;
    std::vector<std::pair<std::string, std::string>> Labels;
};

struct CreateContainerResult
{
    std::string Id;
};

struct StopContainerOptions
{
    static constexpr LONG DefaultTimeout = -1;

    WSLCSignal Signal = WSLCSignalSIGTERM;
    LONG Timeout = DefaultTimeout;
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
            return m_start == EPHEMERAL_PORT && m_end == EPHEMERAL_PORT;
        }

        static PublishPort::PortRange ParsePortPart(const std::string& portPart);
        static PublishPort::PortRange Ephemeral() noexcept
        {
            return {EPHEMERAL_PORT, EPHEMERAL_PORT};
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
    static constexpr uint16_t EPHEMERAL_PORT = 0;

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
} // namespace wsl::windows::wslc::models
