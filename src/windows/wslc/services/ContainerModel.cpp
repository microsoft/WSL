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
            result.m_hostIP = PublishPort::IPAddress(hostPortPart->substr(0, colonPos));
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