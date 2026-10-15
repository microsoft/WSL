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
inline std::string Ipv4WildcardToCidr(std::string_view value)
{
    assert(
        value.empty() ||
        (!std::isspace(static_cast<unsigned char>(value.front())) && !std::isspace(static_cast<unsigned char>(value.back()))));

    std::array<std::string_view, 4> parts;
    parts.fill("*");
    size_t count = 0;
    size_t wildcardPos = parts.size();
    for (const auto range : std::views::split(value, '.'))
    {
        if (count == parts.size())
        {
            return std::string{value};
        }

        const std::string_view part{range.begin(), range.end()};
        if (part == "*")
        {
            if (wildcardPos == parts.size())
            {
                wildcardPos = count;
            }
        }
        else if (wildcardPos != parts.size() || part.empty() || !std::ranges::all_of(part, [](const char character) {
                     return std::isdigit(static_cast<unsigned char>(character));
                 }))
        {
            return std::string{value};
        }

        parts[count++] = part;
    }

    if (wildcardPos == 0 || wildcardPos == parts.size())
    {
        return std::string{value};
    }

    for (size_t index = wildcardPos; index < parts.size(); ++index)
    {
        parts[index] = "0";
    }

    return std::format("{}.{}.{}.{}/{}", parts[0], parts[1], parts[2], parts[3], wildcardPos * 8);
}

} // namespace wsl::windows::common::proxy
