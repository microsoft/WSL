// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include "stringshared.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace wsl::shared::wslc {

inline std::string ConfigureLocalhostHosts(std::string_view Contents)
{
    std::string result{"127.0.0.1\tlocalhost\n::1\tlocalhost ip6-localhost ip6-loopback\n"};
    std::istringstream input{std::string{Contents}};
    for (std::string line; std::getline(input, line);)
    {
        const auto comment = line.find('#');
        std::istringstream fields{line.substr(0, comment)};
        std::string address;
        if (!(fields >> address))
        {
            result += line + '\n';
            continue;
        }

        bool changed = false;
        std::vector<std::string> aliases;
        for (std::string alias; fields >> alias;)
        {
            const std::string_view name{alias};
            if (string::IsEqual(name, std::string_view{"localhost"}, true) || string::IsEqual(name, std::string_view{"ip6-localhost"}, true) ||
                string::IsEqual(name, std::string_view{"ip6-loopback"}, true))
            {
                changed = true;
            }
            else
            {
                aliases.emplace_back(std::move(alias));
            }
        }

        if (!changed)
        {
            result += line + '\n';
            continue;
        }

        if (!aliases.empty())
        {
            result += address;
            for (const auto& alias : aliases)
            {
                result += '\t' + alias;
            }
            if (comment != std::string::npos)
            {
                result += '\t';
            }
        }
        if (comment != std::string::npos)
        {
            result += line.substr(comment);
        }
        if (!aliases.empty() || comment != std::string::npos)
        {
            result += '\n';
        }
    }

    return result;
}

inline std::string ConfigureLocalhostNameService(std::string_view Contents)
{
    std::string result;
    bool hostsConfigured = false;
    std::istringstream input{std::string{Contents}};
    for (std::string line; std::getline(input, line);)
    {
        const auto colon = line.find(':');
        const auto database = std::string_view{line}.substr(0, colon);
        const auto start = database.find_first_not_of(" \t");
        const auto end = database.find_last_not_of(" \t");
        if (colon == std::string::npos || start == std::string_view::npos || database.substr(start, end - start + 1) != "hosts")
        {
            result += line + '\n';
            continue;
        }

        if (!hostsConfigured)
        {
            result += "hosts: files dns";
        }

        const auto comment = line.find('#', colon);
        if (comment != std::string::npos)
        {
            if (!hostsConfigured)
            {
                result += ' ';
            }
            result += line.substr(comment);
        }
        if (!hostsConfigured || comment != std::string::npos)
        {
            result += '\n';
        }
        hostsConfigured = true;
    }

    if (!hostsConfigured)
    {
        result += "hosts: files dns\n";
    }

    return result;
}

template <typename Configure>
inline void ConfigureLocalhostFile(const std::filesystem::path& Path, Configure&& ConfigureContents)
{
    const bool exists = std::filesystem::exists(Path);
    std::string contents;
    if (exists)
    {
        std::ifstream input;
        input.exceptions(std::ios::badbit | std::ios::failbit);
        input.open(Path, std::ios::binary | std::ios::ate);
        contents.resize(static_cast<size_t>(input.tellg()));
        input.seekg(0);
        input.read(contents.data(), static_cast<std::streamsize>(contents.size()));
        input.close();
    }

    const auto configured = ConfigureContents(contents);
    if (exists && configured == contents)
    {
        return;
    }

    std::ofstream output;
    output.exceptions(std::ios::badbit | std::ios::failbit);
    output.open(Path, std::ios::binary | std::ios::trunc);
    if (!exists)
    {
        std::filesystem::permissions(
            Path,
            std::filesystem::perms::owner_read | std::filesystem::perms::owner_write | std::filesystem::perms::group_read |
                std::filesystem::perms::others_read);
    }
    output.write(configured.data(), static_cast<std::streamsize>(configured.size()));
    output.close();
}

} // namespace wsl::shared::wslc
