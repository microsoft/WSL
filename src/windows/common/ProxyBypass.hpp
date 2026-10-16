// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include <algorithm>
#include <array>
#include <cassert>
#include <cctype>
#include <format>
#include <ranges>
#include <string>
#include <string_view>

namespace wsl::windows::common::proxy {

// Convert a valid IPv4 prefix wildcard to CIDR for consumers that do not recognize wildcard syntax,
// e.g., when adapting Windows proxy bypass entries for NO_PROXY.
// Input must be trimmed; the caller is responsible for IPv4 validity. Other entries are returned unchanged.
inline std::string Ipv4WildcardToCidr(std::string_view value)
{
    assert(
        value.empty() ||
        (!std::isspace(static_cast<unsigned char>(value.front())) && !std::isspace(static_cast<unsigned char>(value.back()))));

    std::array<std::string_view, 4> parts{};
    size_t count = 0;
    size_t wildcardPos = parts.size();
    for (const auto range : std::views::split(value, '.'))
    {
        // IPv4 has at most four parts.
        if (count == parts.size())
        {
            return std::string{value};
        }

        const std::string_view part{range.begin(), range.end()};
        // Only wildcards may follow the first wildcard, so 127.*.1.3 cannot be converted.
        if (wildcardPos != parts.size() && part != "*")
        {
            return std::string{value};
        }

        if (part == "*")
        {
            // Record the first wildcard, e.g., position 1 in 127.*.*.
            if (wildcardPos == parts.size())
            {
                wildcardPos = count;
            }
        }
        else if (part.empty() || !std::ranges::all_of(part, [](const char character) {
                     return std::isdigit(static_cast<unsigned char>(character));
                 }))
        {
            // Preserve non-IP prefixes such as 127.example.*.
            return std::string{value};
        }

        parts[count++] = part;
    }

    // Leave '*' and entries without a wildcard unchanged.
    if (wildcardPos == 0 || wildcardPos == parts.size())
    {
        return std::string{value};
    }

    // Replace the wildcard suffix and any omitted octets with zeros: 127.* becomes 127.0.0.0.
    for (size_t index = wildcardPos; index < parts.size(); ++index)
    {
        parts[index] = "0";
    }

    // Each fixed leading octet contributes eight network prefix bits.
    return std::format("{}.{}.{}.{}/{}", parts[0], parts[1], parts[2], parts[3], wildcardPos * 8);
}

} // namespace wsl::windows::common::proxy
