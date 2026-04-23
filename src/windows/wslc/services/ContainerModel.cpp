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

using namespace wsl::shared;
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

// Returns true if the given string is a valid Docker named volume name.
// Based on Docker's named volume validation: ^[a-zA-Z0-9][a-zA-Z0-9_.-]{1,}$
// Source: https://github.com/moby/moby/blob/master/volume/validate.go
bool VolumeMount::IsValidNamedVolumeName(const std::wstring& name)
{
    static const std::wregex namedVolumeRegex(LR"(^[a-zA-Z0-9][a-zA-Z0-9_.-]{1,}$)");
    return std::regex_match(name, namedVolumeRegex);
}

VolumeMount VolumeMount::Parse(const std::wstring& value)
{
    auto lastColon = value.rfind(':');
    if (lastColon == std::wstring::npos)
    {
        THROW_HR_WITH_USER_ERROR(E_INVALIDARG, Localization::WSLCCLI_VolumeInvalidSpec(value, Localization::WSLCCLI_VolumeFormatUsage()));
    }

    VolumeMount vm;
    auto splitColon = lastColon;
    const auto lastToken = value.substr(lastColon + 1);
    if (IsValidMode(lastToken))
    {
        vm.m_isReadOnlyMode = IsReadOnlyMode(lastToken);
        if (lastColon == 0)
        {
            THROW_HR_WITH_USER_ERROR(E_INVALIDARG, Localization::WSLCCLI_VolumeInvalidSpec(value, Localization::WSLCCLI_VolumeFormatUsage()));
        }

        splitColon = value.rfind(':', lastColon - 1);
        if (splitColon == std::wstring::npos)
        {
            THROW_HR_WITH_USER_ERROR(E_INVALIDARG, Localization::WSLCCLI_VolumeInvalidSpec(value, Localization::WSLCCLI_VolumeFormatUsage()));
        }

        vm.m_containerPath = WideToMultiByte(value.substr(splitColon + 1, lastColon - splitColon - 1));
    }
    else
    {
        vm.m_containerPath = WideToMultiByte(lastToken);
    }

    if (vm.m_containerPath.empty())
    {
        THROW_HR_WITH_USER_ERROR(
            E_INVALIDARG, Localization::WSLCCLI_VolumeContainerPathEmpty(value, Localization::WSLCCLI_VolumeFormatUsage()));
    }

    if (vm.m_containerPath[0] != '/')
    {
        THROW_HR_WITH_USER_ERROR(
            E_INVALIDARG, Localization::WSLCCLI_VolumeContainerPathNotAbsolute(value, Localization::WSLCCLI_VolumeFormatUsage()));
    }

    const auto rawHostPath = value.substr(0, splitColon);
    if (rawHostPath.empty())
    {
        THROW_HR_WITH_USER_ERROR(E_INVALIDARG, Localization::WSLCCLI_VolumeHostPathEmpty(value, Localization::WSLCCLI_VolumeFormatUsage()));
    }

    // This is where we need to check if the user is referencing a named volume.
    // This can be either an existing named volume or a new named volume that will be created.
    if (VolumeMount::IsValidNamedVolumeName(rawHostPath))
    {
        vm.m_isNamedVolume = true;
        vm.m_host = rawHostPath;
    }
    else
    {
        // Not a named volume, so it must be a path.
        // Use wil::GetFullPathNameW to resolve relative paths against the CWD.
        std::wstring resolvedHostPath;
        const auto hr = wil::GetFullPathNameW(rawHostPath.c_str(), resolvedHostPath);
        if (FAILED(hr))
        {
            THROW_HR_WITH_USER_ERROR(hr, Localization::WSLCCLI_VolumeHostPathInvalid(value, rawHostPath));
        }

        // GetFileAttributesW validates the resolved path syntax without requiring existence.
        // ERROR_INVALID_NAME indicates illegal characters in the path (e.g. ":" as a component).
        if (GetFileAttributesW(resolvedHostPath.c_str()) == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_INVALID_NAME)
        {
            THROW_HR_WITH_USER_ERROR(E_INVALIDARG, Localization::WSLCCLI_VolumeHostPathInvalid(value, rawHostPath));
        }

        vm.m_host = std::move(resolvedHostPath);
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
        THROW_HR_WITH_USER_ERROR(E_INVALIDARG, Localization::WSLCCLI_EnvKeyEmptyError());
    }

    if (std::any_of(key.begin(), key.end(), std::iswspace))
    {
        THROW_HR_WITH_USER_ERROR(E_INVALIDARG, Localization::WSLCCLI_EnvKeyWhitespaceError(key));
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

TmpfsMount TmpfsMount::Parse(const std::string& value)
{
    TmpfsMount result{};
    auto colonPos = value.find(':');
    if (colonPos == std::string::npos)
    {
        result.m_containerPath = value;
        return result;
    }

    result.m_containerPath = value.substr(0, colonPos);
    result.m_options = value.substr(colonPos + 1);
    return result;
}

CidFile::CidFile(const std::optional<std::wstring>& path)
{
    if (!path.has_value())
    {
        return;
    }

    m_path = *path;
    auto [file, openError] = wil::try_create_new_file(std::filesystem::path(*m_path).c_str(), GENERIC_WRITE);
    if (!file.is_valid())
    {
        if (openError == ERROR_FILE_EXISTS || openError == ERROR_ALREADY_EXISTS)
        {
            THROW_HR_WITH_USER_ERROR(HRESULT_FROM_WIN32(openError), Localization::WSLCCLI_CIDFileAlreadyExistsError(*m_path));
        }

        const auto errorMessage = wsl::shared::string::MultiByteToWide(std::system_category().message(openError));
        THROW_HR_WITH_USER_ERROR(HRESULT_FROM_WIN32(openError), Localization::MessageWslcFailedToOpenFile(*m_path, errorMessage));
    }

    m_file = std::move(file);
}

CidFile::~CidFile()
{
    if (m_committed || !m_path.has_value())
    {
        return;
    }

    m_file.reset();
    std::error_code ec;
    std::filesystem::remove(std::filesystem::path(*m_path), ec);
}

void CidFile::Commit(const std::string& containerId)
{
    if (m_file)
    {
        DWORD bytesWritten{};
        THROW_IF_WIN32_BOOL_FALSE(::WriteFile(m_file.get(), containerId.data(), static_cast<DWORD>(containerId.size()), &bytesWritten, nullptr));
    }

    m_committed = true;
}
} // namespace wsl::windows::wslc::models
