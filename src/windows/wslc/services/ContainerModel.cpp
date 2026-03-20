/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ContainerModel.cpp

Abstract:

    This file contains the ContainerModel implementation
--*/

#include "precomp.h"
#include "ContainerModel.h"
#include <cctype>

namespace wsl::windows::wslc::models {

using namespace wsl::shared::string;

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
    return vm;
}

static inline bool IsSpace(wchar_t ch)
{
    return std::iswspace(ch) != 0;
}

std::optional<std::wstring> EnvironmentVariable::Parse(std::wstring entry)
{
    if (entry.empty() || std::all_of(entry.begin(), entry.end(), IsSpace))
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
        THROW_HR_WITH_USER_ERROR(E_INVALIDARG, "Environment variable key cannot be empty");
    }

    if (std::any_of(key.begin(), key.end(), IsSpace))
    {
        THROW_HR_WITH_USER_ERROR(E_INVALIDARG, std::format(L"Environment variable key '{}' cannot contain whitespace", key));
    }

    if (!value.has_value())
    {
        const DWORD valueLength = GetEnvironmentVariableW(key.c_str(), nullptr, 0);
        if (valueLength > 0)
        {
            std::wstring envValue(valueLength, L'\0');
            GetEnvironmentVariableW(key.c_str(), envValue.data(), valueLength);
            if (!envValue.empty() && envValue.back() == L'\0')
            {
                envValue.pop_back();
            }

            value = envValue;
        }
        else
        {
            return std::nullopt;
        }
    }

    return std::format(L"{}={}", key, value.value());
}

std::vector<std::wstring> EnvironmentVariable::ParseFile(std::wstring filePath)
{
    if (!std::filesystem::exists(filePath))
    {
        THROW_HR_WITH_USER_ERROR(E_INVALIDARG, std::format(L"Environment file '{}' does not exist", filePath));
    }

    // Read the file line by line
    std::vector<std::wstring> envVars;
    std::ifstream file(filePath);
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
