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

static inline bool IsSpace(char c)
{
    return std::isspace(static_cast<unsigned char>(c));
};

std::optional<std::string> EnvironmentVariable::Parse(std::string entry)
{
    if(entry.empty() || std::all_of(entry.begin(), entry.end(), IsSpace))
    {
        return std::nullopt;
    }

    std::string key;
    std::optional<std::string> value;

    auto delimiterPos = entry.find('=');
    if (delimiterPos == std::string::npos)
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
        const auto envValue = std::getenv(key.c_str());
        if (envValue == nullptr)
        {
            return std::nullopt;
        }

        value = std::string(envValue);
    }

    return std::format("{}={}", key, value.value());
}

std::vector<std::string> EnvironmentVariable::ParseFile(std::string filePath)
{
    if (!std::filesystem::exists(filePath))
    {
        throw std::invalid_argument(std::format("Environment file '{}' does not exist", filePath));
    }

    // Read the file line by line
    std::vector<std::string> envVars;
    std::ifstream file(filePath);
    std::string line;
    while (std::getline(file, line))
    {
        // Remove leading whitespace
        line.erase(line.begin(), std::find_if(line.begin(), line.end(), [](unsigned char ch) {
            return !std::isspace(ch);
        }));

        // Skip empty lines and comments
        if (line.empty() || line[0] == '#')
        {
            continue;
        }

        auto envVar = Parse(line);
        if (envVar.has_value())
        {
            envVars.push_back(std::move(envVar.value()));
        }
    }

    return envVars;
}
} // namespace wsl::windows::wslc::models
