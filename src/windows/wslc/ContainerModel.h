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
#include <docker_schema.h>

namespace wsl::windows::wslc::models {
struct ContainerCreateOptions
{
    bool TTY = false;
    bool Interactive = false;
    std::vector<std::string> Arguments;
    std::string Name;
    std::string Port;
};

struct ContainerRunOptions : public ContainerCreateOptions
{
    bool Detach = false;
};

struct CreateContainerResult
{
    std::string Id;
};

struct StopContainerOptions
{
    static constexpr ULONG DefaultTimeout = -1;

    int Signal = WSLASignalSIGTERM;
    ULONG Timeout = DefaultTimeout;
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
    WSLA_CONTAINER_STATE State;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_ONLY_SERIALIZE(ContainerInformation, Id, Name, Image, State);
};

struct ExecContainerOptions
{
    bool TTY = false;
    bool Interactive = false;
    std::vector<std::string> Arguments;
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
        int Start() const
        {
            return m_start;
        }
        int End() const
        {
            return m_end;
        }

        constexpr unsigned int Count() const noexcept { return (m_end >= m_start) ? (m_end - m_start + 1) : 0; }
        constexpr bool IsSingle() const noexcept { return Count() == 1; }
        constexpr bool IsValid() const noexcept { return Count() > 0 && IsValidPort(m_start) && IsValidPort(m_end); }
        static PublishPort::PortRange ParsePortPart(const std::string& portPart);
    private:
        int m_start;
        int m_end;
    };

    struct IPAddress
    {
        std::string Value() const { return m_value; }
        bool IsIPv6() const { return m_isIPv6; }
        static IPAddress ParseHostIP(const std::string& hostIpPart);
    private:
        std::string m_value;
        bool m_isIPv6 = false;
    };

    std::optional<IPAddress> HostIP() const noexcept { return m_hostIP; }
    std::optional<PortRange> HostPort() const noexcept { return m_hostPort; }
    PortRange ContainerPort() const noexcept { return m_containerPort; }
    Protocol PortProtocol() const noexcept { return m_protocol; }
    std::string Original() const noexcept { return m_original; }

    static PublishPort Parse(const std::string& value);
    bool HasEphemeralHostPort() const noexcept { return !m_hostPort.has_value(); }
    bool IsRangeMapping() const noexcept { return !m_containerPort.IsSingle(); }
private:
    std::optional<IPAddress> m_hostIP;
    std::optional<PortRange> m_hostPort;
    PortRange m_containerPort;
    Protocol m_protocol = Protocol::TCP;
    std::string m_original;
    void Validate() const;
    PublishPort() = default;
    static constexpr bool IsValidPort(int port) noexcept { return port >= 1 && port <= 65535; }
};

} // namespace wsl::windows::wslc::models
