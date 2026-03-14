/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ContainerModel.h

Abstract:

    This file contains the ContainerModel definitions

--*/

#pragma once

#include <wslservice.h>
#include <wslaservice.h>
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
    bool Detach = false;
    bool Interactive = false;
    std::string Name;
    bool Remove = false;
    bool TTY = false;
    std::vector<std::string> Ports;
};

struct CreateContainerResult
{
    std::string Id;
};

struct StopContainerOptions
{
    static constexpr LONG DefaultTimeout = -1;

    WSLASignal Signal = WSLASignalSIGTERM;
    LONG Timeout = DefaultTimeout;
};

struct KillContainerOptions
{
    int Signal = WSLASignalSIGKILL;
};

struct ContainerInformation
{
    std::string Id;
    std::string Name;
    std::string Image;
    WSLAContainerState State;
    ULONGLONG StateChangedAt{};
    ULONGLONG CreatedAt{};

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_ONLY_SERIALIZE(ContainerInformation, Id, Name, Image, State, StateChangedAt, CreatedAt);
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
        explicit IPAddress(const IN_ADDR& v4) : m_isIPv6(false)
        {
            std::memcpy(m_bytes.data(), &v4, 4);
        }

        explicit IPAddress(const IN6_ADDR& v6) : m_isIPv6(true)
        {
            std::memcpy(m_bytes.data(), &v6, 16);
        }

        bool IsIPv6() const
        {
            return m_isIPv6;
        }

        const std::array<uint8_t, 16> GetBytes() const
        {
            return m_bytes;
        }

        static IPAddress ParseHostIP(const std::string& hostIpPart);
        bool IsLoopback() const;
        bool IsAllInterfaces() const;
        std::string ToString() const;

    private:
        bool m_isIPv6 = false;
        std::array<uint8_t, 16> m_bytes{};
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
} // namespace wsl::windows::wslc::models
