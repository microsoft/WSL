// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include <array>
#include <charconv>
#include <format>
#include <ranges>
#include <string>
#include <string_view>

namespace wsl::windows::common::proxy {

// Convert IPv4 prefix wildcards to CIDR for consumers that accept CIDR ranges but do not recognize IP wildcard syntax
// (e.g., adapting Windows proxy bypass entries for NO_PROXY).
// Preserve other entries, including domain patterns and Windows-specific macros.
inline std::string Ipv4WildcardToCidr(std::string_view value)
{
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos)
    {
        return std::string{value};
    }

    const auto pattern = value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
    const auto wildcard = pattern.find(".*");
    if (wildcard == std::string_view::npos || wildcard == 0)
    {
        return std::string{value};
    }

    const auto suffix = pattern.substr(wildcard);
    if (suffix != ".*" && suffix != ".*.*" && suffix != ".*.*.*")
    {
        return std::string{value};
    }

    std::array<unsigned int, 4> octets{};
    size_t fixedOctets = 0;
    for (const auto range : std::views::split(pattern.substr(0, wildcard), '.'))
    {
        const std::string_view part{range.begin(), range.end()};
        if (fixedOctets == 3 || part.empty() || part.size() > 3 || (part.size() > 1 && part.front() == '0'))
        {
            return std::string{value};
        }

        const auto [end, error] = std::from_chars(part.data(), part.data() + part.size(), octets[fixedOctets]);
        if (error != std::errc{} || end != part.data() + part.size() || octets[fixedOctets] > 255)
        {
            return std::string{value};
        }

        ++fixedOctets;
    }

    if (fixedOctets + suffix.size() / 2 > octets.size())
    {
        return std::string{value};
    }

    return std::format("{}.{}.{}.{}/{}", octets[0], octets[1], octets[2], octets[3], fixedOctets * 8);
}

} // namespace wsl::windows::common::proxy
