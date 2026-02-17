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

static PublishPort::PortRange ParsePortPart(const std::string& portPart)
{
    // Find optional port range separator
    auto dashPos = portPart.find('-');
    if (dashPos != std::string::npos)
    {
        // Port range specified
        try
        {
            auto startPortStr = portPart.substr(0, dashPos);
            auto endPortStr = portPart.substr(dashPos + 1);
            return PublishPort::PortRange{ .Start = std::stoi(startPortStr), .End = std::stoi(endPortStr) };
        }
        catch (const std::exception&)
        {
            THROW_HR_WITH_USER_ERROR(E_INVALIDARG, "Invalid port range specified in port mapping.");
        }
    }
    else
    {
        // Single port specified
        try
        {
            int port = std::stoi(portPart);
            return PublishPort::PortRange{ .Start = port, .End = port };
        }
        catch (const std::exception&)
        {
            THROW_HR_WITH_USER_ERROR(E_INVALIDARG, "Invalid port specified in port mapping.");
        }
    }
}

static PublishPort::IPAddress ParseHostIP(const std::string& hostIpPart)
{
    PublishPort::IPAddress ipAddress;

    // Check if it's an IPv6 address (enclosed in square brackets)
    if (!hostIpPart.empty() && hostIpPart.front() == '[' && hostIpPart.back() == ']')
    {
        ipAddress.Value = hostIpPart.substr(1, hostIpPart.size() - 2);
        ipAddress.IsIPv6 = true;
        sockaddr_in6 sa6{};
        if (inet_pton(AF_INET6, ipAddress.Value.c_str(), &(sa6.sin6_addr)) != 1)
        {
            THROW_HR_WITH_USER_ERROR(E_INVALIDARG, "Invalid IPv6 address specified in port mapping.");
        }
    }
    else
    {
        ipAddress.Value = hostIpPart;
        ipAddress.IsIPv6 = false;
        sockaddr_in sa4{};
        if (inet_pton(AF_INET, ipAddress.Value.c_str(), &(sa4.sin_addr)) != 1)
        {
            THROW_HR_WITH_USER_ERROR(E_INVALIDARG, "Invalid IPv4 address specified in port mapping.");
        }
    }

    return ipAddress;
}

PublishPort PublishPort::Parse(const std::string& value)
{
    PublishPort result{};
    result.Original = value;
    
    // 1. Strip optional protocol suffix
    std::string portPart = value;
    auto slashPos = value.find('/');
    if (slashPos != std::string::npos)
    {
        portPart = value.substr(0, slashPos);
        auto protocolPart = value.substr(slashPos + 1);
        if (protocolPart == "tcp")
        {
            result.PortProtocol = PublishPort::Protocol::TCP;
        }
        else if (protocolPart == "udp")
        {
            result.PortProtocol = PublishPort::Protocol::UDP;
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
        result.ContainerPort = ParsePortPart(portPart.substr(colonPos + 1));
        hostPortPart = portPart.substr(0, colonPos);
    }
    else
    {
        result.ContainerPort = ParsePortPart(portPart);
    }

    // 3. Parse the host port
    if (hostPortPart.has_value())
    {
        auto colonPos = hostPortPart->rfind(':');
        if (colonPos != std::string::npos)
        {
            result.HostIP = ParseHostIP(hostPortPart->substr(0, colonPos));
            auto hostPort = hostPortPart->substr(colonPos + 1);
            if(!hostPort.empty())
            {
                result.HostPort = ParsePortPart(hostPort);
            }
        }
        else
        {
            result.HostPort = ParsePortPart(*hostPortPart);
        }
    }

    result.Validate();
    return result;
}

void PublishPort::Validate() const {
    if (ContainerPort.Count() == 0)
    {
        THROW_HR_WITH_USER_ERROR(E_INVALIDARG, "Container port must specify at least one port.");
    }

    if (!ContainerPort.IsValid())
    {
        if (ContainerPort.IsSingle())
        {
            THROW_HR_WITH_USER_ERROR(E_INVALIDARG, "Container port must be a valid port number (1-65535).");
        }
        else
        {
            THROW_HR_WITH_USER_ERROR(E_INVALIDARG, "Container port range must be valid port numbers (1-65535) and the start must be less than or equal to the end.");
        }
    }

    if (HostPort.has_value() && HostPort->Count() != ContainerPort.Count())
    {
        THROW_HR_WITH_USER_ERROR(E_INVALIDARG, "Host port range must match the container port range.");
    }
}
}