/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ContainerModel.cpp

Abstract:

    This file contains the ContainerModel implementation
--*/

#include "precomp.h"
#include "ContainerModel.h"

namespace wsl::windows::wslc::models {

using namespace wsl::shared::string;

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

VolumeMount VolumeMount::Parse(const std::wstring& value)
{
    auto lastColon = value.rfind(':');
    if (lastColon == std::wstring::npos)
    {
        THROW_HR_WITH_USER_ERROR(
            E_INVALIDARG, std::format(L"Invalid volume specifications: '{}'. Expected format: <host path>:<container path>[:mode]", value));
    }

    VolumeMount vm;
    auto splitColon = lastColon;
    const auto lastToken = value.substr(lastColon + 1);
    if (IsValidMode(lastToken))
    {
        vm.m_isReadOnlyMode = IsReadOnlyMode(lastToken);
        if (lastColon == 0)
        {
            THROW_HR_WITH_USER_ERROR(
                E_INVALIDARG,
                std::format(L"Invalid volume specifications: '{}'. Expected format: <host path>:<container path>[:mode]", value));
        }

        splitColon = value.rfind(':', lastColon - 1);
        if (splitColon == std::wstring::npos)
        {
            THROW_HR_WITH_USER_ERROR(
                E_INVALIDARG,
                std::format(L"Invalid volume specifications: '{}'. Expected format: <host path>:<container path>[:mode]", value));
        }

        vm.m_containerPath = WideToMultiByte(value.substr(splitColon + 1, lastColon - splitColon - 1));
    }
    else
    {
        vm.m_containerPath = WideToMultiByte(lastToken);
    }

    vm.m_hostPath = value.substr(0, splitColon);

    if (vm.m_hostPath.empty())
    {
        THROW_HR_WITH_USER_ERROR(
            E_INVALIDARG,
            std::format(L"Invalid volume specifications: '{}'. Host path cannot be empty. Expected format: <host path>:<container path>[:mode]", value));
    }

    if (!vm.m_containerPath.empty() && vm.m_containerPath[0] != '/')
    {
        THROW_HR_WITH_USER_ERROR(
            E_INVALIDARG,
            std::format(L"Invalid volume specifications: '{}'. Container path must be an absolute path (starting with '/').", value));
    }

    return vm;
}

std::optional<std::wstring> EnvironmentVariable::Parse(const std::wstring& entry)
{
    if (entry.empty() || std::all_of(entry.begin(), entry.end(), std::iswspace))
    {
        return std::nullopt;
    }

    std::wstring key;
    std::optional<std::wstring> value;

    auto delimiterPos = entry.find('=');
    if (delimiterPos == std::wstring::npos)
    {
        key = entry;
    }
    else
    {
        key = entry.substr(0, delimiterPos);
        value = entry.substr(delimiterPos + 1);
    }

    if (key.empty())
    {
        THROW_HR_WITH_USER_ERROR(E_INVALIDARG, L"Environment variable key cannot be empty");
    }

    if (std::any_of(key.begin(), key.end(), std::iswspace))
    {
        THROW_HR_WITH_USER_ERROR(E_INVALIDARG, std::format(L"Environment variable key '{}' cannot contain whitespace", key));
    }

    if (!value.has_value())
    {
        std::wstring envValue;
        auto hr = wil::GetEnvironmentVariableW(key.c_str(), envValue);
        if (FAILED(hr))
        {
            return std::nullopt;
        }

        value = envValue;
    }

    return std::format(L"{}={}", key, value.value());
}

std::vector<std::wstring> EnvironmentVariable::ParseFile(const std::wstring& filePath)
{
    std::ifstream file(filePath);
    if (!file.is_open() || !file.good())
    {
        THROW_HR_WITH_USER_ERROR(E_INVALIDARG, std::format(L"Environment file '{}' cannot be opened for reading", filePath));
    }

    // Read the file line by line
    std::vector<std::wstring> envVars;
    std::string line;
    while (std::getline(file, line))
    {
        // Remove leading whitespace
        line.erase(line.begin(), std::find_if(line.begin(), line.end(), [](unsigned char ch) { return !std::isspace(ch); }));

        // Skip empty lines and comments
        if (line.empty() || line[0] == '#')
        {
            continue;
        }

        auto envVar = Parse(wsl::shared::string::MultiByteToWide(line));
        if (envVar.has_value())
        {
            envVars.push_back(std::move(envVar.value()));
        }
    }

    return envVars;
}
} // namespace wsl::windows::wslc::models
