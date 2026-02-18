/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ContainerModel.cpp

Abstract:

    This file contains the ContainerModel definitions

--*/

#include <precomp.h>
#include "ContainerModel.h"

namespace wsl::windows::wslc::models {

PublishPort::PortRange PublishPort::PortRange::ParsePortPart(const std::string& portPart)
{
    static auto parsePort = [](const std::string& value, const char* errorMessage) -> int {
        try
        {
            size_t idx = 0;
            int port = std::stoi(value, &idx, 10);
            if (idx != value.size())
            {
                THROW_HR_WITH_USER_ERROR(E_INVALIDARG, errorMessage);
            }
            return port;
        }
        catch (const std::exception&)
        {
            THROW_HR_WITH_USER_ERROR(E_INVALIDARG, errorMessage);
        }
    };

    // Find optional port range separator
    auto dashPos = portPart.find('-');
    if (dashPos != std::string::npos)
    {
        // Port range specified
        auto startPortStr = portPart.substr(0, dashPos);
        auto endPortStr = portPart.substr(dashPos + 1);
        int startPort = parsePort(startPortStr, "Invalid port range specified in port mapping.");
        int endPort = parsePort(endPortStr, "Invalid port range specified in port mapping.");

        PublishPort::PortRange result{};
        result.m_start = startPort;
        result.m_end = endPort;
        return result;
    }
    else
    {
        // Single port specified
        int port = parsePort(portPart, "Invalid port specified in port mapping.");

        PublishPort::PortRange result{};
        result.m_start = port;
        result.m_end = port;
        return result;
    }
}

PublishPort::IPAddress PublishPort::IPAddress::ParseHostIP(const std::string& hostIpPart)
{
    // Check if it's an IPv6 address (enclosed in square brackets)
    if (!hostIpPart.empty() && hostIpPart.front() == '[' && hostIpPart.back() == ']')
    {
        auto address = hostIpPart.substr(1, hostIpPart.size() - 2);
        IN6_ADDR v6{};
        if (inet_pton(AF_INET6, address.c_str(), &(v6)) == 1)
        {
            return PublishPort::IPAddress(v6);
        }

        THROW_HR_WITH_USER_ERROR(E_INVALIDARG, "Invalid IPv6 address specified in port mapping.");
    }
    else
    {
        IN_ADDR v4{};
        if (inet_pton(AF_INET, hostIpPart.c_str(), &(v4)) == 1)
        {
            return PublishPort::IPAddress(v4);
        }

        THROW_HR_WITH_USER_ERROR(E_INVALIDARG, "Invalid IPv4 address specified in port mapping.");
    }
}

bool PublishPort::IPAddress::IsAllInterfaces() const
{
    for (size_t i = 0; i < 16; i++)
    {
        if (m_bytes[i] != 0)
        {
            return false;
        }
    }

    return true;
}

bool PublishPort::IPAddress::IsLoopback() const
{
    if (IsIPv6())
    {
        // IPv6 loopback is ::1
        static const std::array<uint8_t, 16> loopbackV6Bytes = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
        return m_bytes == loopbackV6Bytes;
    }

    // IPv4 loopback is 127.0.0.1
    static const std::array<uint8_t, 4> loopbackV4Bytes = {127, 0, 0, 1};
    return std::equal(m_bytes.begin(), m_bytes.begin() + 4, loopbackV4Bytes.begin());
}

PublishPort PublishPort::Parse(const std::string& value)
{
    PublishPort result{};
    result.m_original = value;
    
    // 1. Strip optional protocol suffix
    std::string portPart = value;
    auto slashPos = value.find('/');
    if (slashPos != std::string::npos)
    {
        portPart = value.substr(0, slashPos);
        auto protocolPart = value.substr(slashPos + 1);
        if (protocolPart == "tcp")
        {
            result.m_protocol = PublishPort::Protocol::TCP;
        }
        else if (protocolPart == "udp")
        {
            result.m_protocol = PublishPort::Protocol::UDP;
        }
        else
        {
            THROW_HR_WITH_USER_ERROR(E_INVALIDARG, "Invalid protocol specified in port mapping. Only 'tcp' and 'udp' are supported.");
        }
    }

    // 2.  Split off the container port from the right
    auto colonPos = portPart.rfind(':');
    std::optional<std::string> hostPortPart;
    if (colonPos != std::string::npos)
    {
        result.m_containerPort = PublishPort::PortRange::ParsePortPart(portPart.substr(colonPos + 1));
        hostPortPart = portPart.substr(0, colonPos);
    }
    else
    {
        result.m_containerPort = PublishPort::PortRange::ParsePortPart(portPart);
    }

    // 3. Parse the host port
    if (hostPortPart.has_value())
    {
        auto colonPos = hostPortPart->rfind(':');
        if (colonPos != std::string::npos)
        {
            result.m_hostIP = PublishPort::IPAddress::ParseHostIP(hostPortPart->substr(0, colonPos));
            auto hostPort = hostPortPart->substr(colonPos + 1);
            if(!hostPort.empty())
            {
                result.m_hostPort = PublishPort::PortRange::ParsePortPart(hostPort);
            }
        }
        else
        {
            result.m_hostPort = PublishPort::PortRange::ParsePortPart(*hostPortPart);
        }
    }

    result.Validate();
    return result;
}

void PublishPort::Validate() const {
    if (m_containerPort.Count() == 0)
    {
        THROW_HR_WITH_USER_ERROR(E_INVALIDARG, "Container port must specify at least one port.");
    }

    if (!m_containerPort.IsValid())
    {
        if (m_containerPort.IsSingle())
        {
            THROW_HR_WITH_USER_ERROR(E_INVALIDARG, "Container port must be a valid port number (1-65535).");
        }
        else
        {
            THROW_HR_WITH_USER_ERROR(E_INVALIDARG, "Container port range must be valid port numbers (1-65535) and the start must be less than or equal to the end.");
        }
    }

    if (m_hostPort.has_value() && m_hostPort->Count() != m_containerPort.Count())
    {
        THROW_HR_WITH_USER_ERROR(E_INVALIDARG, "Host port range must match the container port range.");
    }
}
}