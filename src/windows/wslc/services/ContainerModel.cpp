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
    static auto parsePort = [](const std::string& value, const std::string& errorMessage) -> uint16_t {
        try
        {
            // Ensure the value is not empty and contains only digits before parsing
            if (value.empty() || !std::all_of(value.begin(), value.end(), ::isdigit))
            {
                THROW_HR_WITH_USER_ERROR(E_INVALIDARG, errorMessage);
            }

            // Parse the port number and validate the range
            auto port = std::stoul(value, nullptr, 10);
            if (!PublishPort::IsValidPort(port))
            {
                THROW_HR_WITH_USER_ERROR(E_INVALIDARG, errorMessage);
            }
            return static_cast<uint16_t>(port);
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
        auto startPort = parsePort(startPortStr, std::format("Invalid port range specified in port mapping: '{}'.", portPart));
        auto endPort = parsePort(endPortStr, std::format("Invalid port range specified in port mapping: '{}'.", portPart));
        return {startPort, endPort};
    }

    // Single port specified
    auto port = parsePort(portPart, std::format("Invalid port specified in port mapping: '{}'.", portPart));
    return {port, port};
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

        THROW_HR_WITH_USER_ERROR(E_INVALIDARG, std::format("Invalid IPv6 address specified in port mapping: '{}'.", hostIpPart));
    }
    else
    {
        IN_ADDR v4{};
        if (inet_pton(AF_INET, hostIpPart.c_str(), &(v4)) == 1)
        {
            return PublishPort::IPAddress(v4);
        }

        THROW_HR_WITH_USER_ERROR(E_INVALIDARG, std::format("Invalid IPv4 address specified in port mapping: '{}'.", hostIpPart));
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

std::string PublishPort::IPAddress::ToString() const
{
    char strBuffer[INET6_ADDRSTRLEN] = {};
    if (IsIPv6())
    {
        inet_ntop(AF_INET6, m_bytes.data(), strBuffer, INET6_ADDRSTRLEN);
    }
    else
    {
        inet_ntop(AF_INET, m_bytes.data(), strBuffer, INET_ADDRSTRLEN);
    }
    return std::string(strBuffer);
}

bool PublishPort::IPAddress::IsLoopback() const
{
    if (IsIPv6())
    {
        // IPv6 loopback is ::1
        static const std::array<uint8_t, 16> loopbackV6Bytes = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
        return m_bytes == loopbackV6Bytes;
    }

    // IPv4 loopback is 127.0.0.0/8
    return m_bytes[0] == 127;
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
            THROW_HR_WITH_USER_ERROR(
                E_INVALIDARG, "Invalid protocol specified in port mapping. Only 'tcp' and 'udp' are supported.");
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
            if (!hostPort.empty())
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

void PublishPort::Validate() const
{
    if (m_containerPort.Count() == 0)
    {
        THROW_HR_WITH_USER_ERROR(E_INVALIDARG, "Container port must specify at least one port.");
    }

    if (!m_containerPort.IsValid())
    {
        THROW_HR_WITH_USER_ERROR(E_INVALIDARG, "Container port must be a valid port number (1-65535).");
    }

    if (!m_hostPort.IsEphemeral())
    {
        if (!m_hostPort.IsValid())
        {
            THROW_HR_WITH_USER_ERROR(E_INVALIDARG, "Host port must be a valid port number (1-65535).");
        }

        if (m_hostPort.Count() != m_containerPort.Count())
        {
            THROW_HR_WITH_USER_ERROR(E_INVALIDARG, "Host port range must match the container port range.");
        }
    }
}
} // namespace wsl::windows::wslc::models