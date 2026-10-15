// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include <array>
#include <charconv>
#include <format>
#include <string>
#include <string_view>

namespace wsl::windows::common::proxy {

// Convert Windows IPv4 prefix wildcards to CIDR for NO_PROXY consumers that support IP ranges.
// Preserve other entries, including domain patterns and Windows-specific macros.
inline std::string NormalizeBypass(std::string_view bypass)
{
    const auto original = bypass;
    const auto first = bypass.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos)
    {
        return std::string{original};
    }

    bypass = bypass.substr(first, bypass.find_last_not_of(" \t\r\n") - first + 1);
    std::array<unsigned int, 4> octets{};
    size_t fixedOctets = 0;
    size_t totalOctets = 0;
    bool hasWildcard = false;
    while (!bypass.empty())
    {
        if (totalOctets++ == octets.size())
        {
            return std::string{original};
        }

        const auto separator = bypass.find('.');
        const auto part = bypass.substr(0, separator);
        if (part == "*")
        {
            hasWildcard = true;
        }
        else
        {
            if (hasWildcard || part.empty() || part.size() > 3 || (part.size() > 1 && part.front() == '0'))
            {
                return std::string{original};
            }

            unsigned int octet = 0;
            const auto [end, error] = std::from_chars(part.data(), part.data() + part.size(), octet);
            if (error != std::errc{} || end != part.data() + part.size() || octet > 255)
            {
                return std::string{original};
            }

            octets[fixedOctets++] = octet;
        }

        if (separator == std::string_view::npos)
        {
            break;
        }

        bypass.remove_prefix(separator + 1);
        if (bypass.empty())
        {
            return std::string{original};
        }
    }

    if (!hasWildcard || fixedOctets == 0)
    {
        return std::string{original};
    }

    return std::format("{}.{}.{}.{}/{}", octets[0], octets[1], octets[2], octets[3], fixedOctets * 8);
}

} // namespace wsl::windows::common::proxy
