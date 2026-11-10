// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "WSLCProxy.h"
#include "WslCoreNetworkingSupport.h"
#include "stringshared.h"
#include <array>
#include <charconv>
#include <cstring>

namespace wsl::windows::wslc {

namespace {

    constexpr std::array<std::pair<std::string_view, std::string_view>, 3> c_proxyNames{
        {{"http_proxy", "HTTP_PROXY"}, {"https_proxy", "HTTPS_PROXY"}, {"no_proxy", "NO_PROXY"}}};

    std::string_view EnvironmentName(std::string_view Entry)
    {
        return Entry.substr(0, Entry.find('='));
    }

    std::optional<std::pair<size_t, size_t>> FindProxyHost(std::string_view url)
    {
        const auto schemeEnd = url.find("://");
        if (schemeEnd == 0)
        {
            return std::nullopt;
        }

        auto begin = schemeEnd == std::string_view::npos ? 0 : schemeEnd + 3;
        auto authorityEnd = url.find_first_of("/?#", begin);
        if (authorityEnd == std::string_view::npos)
        {
            authorityEnd = url.size();
        }

        const auto at = url.substr(begin, authorityEnd - begin).rfind('@');
        if (at != std::string_view::npos)
        {
            begin += at + 1;
        }

        size_t end{};
        if (begin < authorityEnd && url[begin] == '[')
        {
            end = url.find(']', begin);
            if (end == std::string_view::npos || end >= authorityEnd)
            {
                return std::nullopt;
            }

            end++;
        }
        else
        {
            end = std::min(url.find(':', begin), authorityEnd);
        }

        if (end <= begin)
        {
            return std::nullopt;
        }

        if (end < authorityEnd)
        {
            if (url[end] != ':')
            {
                return std::nullopt;
            }

            uint16_t port{};
            const auto portBegin = url.data() + end + 1;
            const auto portEnd = url.data() + authorityEnd;
            const auto result = std::from_chars(portBegin, portEnd, port);
            if (result.ec != std::errc{} || result.ptr != portEnd || port == 0)
            {
                return std::nullopt;
            }
        }

        return std::make_pair(begin, end - begin);
    }

} // namespace

bool IsProxyEnvironmentVariable(std::string_view Entry)
{
    return Entry.find('=') != std::string_view::npos && std::ranges::any_of(c_proxyNames, [&](const auto& names) {
               return wsl::shared::string::IsEqual(EnvironmentName(Entry), names.first, true);
           });
}

void AddProxyEnvironmentDefaults(std::vector<std::string>& Environment, const std::vector<std::string>& Defaults)
{
    // Either explicit casing, including an empty value, suppresses both default casings.
    const auto explicitEnvironment = Environment;
    for (const auto& entry : Defaults)
    {
        if (std::ranges::none_of(explicitEnvironment, [&](const auto& value) {
                return wsl::shared::string::IsEqual(EnvironmentName(value), EnvironmentName(entry), true);
            }))
        {
            Environment.emplace_back(entry);
        }
    }
}

void ResolveProxyEnvironmentLoopback(std::vector<std::string>& Environment, const std::string& HostLoopback)
{
    for (auto& entry : Environment)
    {
        const auto name = EnvironmentName(entry);
        if (wsl::shared::string::IsEqual(name, "http_proxy", true) || wsl::shared::string::IsEqual(name, "https_proxy", true))
        {
            const auto offset = entry.find('=') + 1;
            entry = entry.substr(0, offset) + ResolveProxyUrl(entry.substr(offset), HostLoopback);
        }
    }
}

std::string ResolveProxyUrl(const std::string& Url, const std::string& HostLoopback)
{
    const auto host = FindProxyHost(Url);
    if (HostLoopback.empty() || !host)
    {
        return Url;
    }

    const auto name = std::string_view{Url}.substr(host->first, host->second);
    if (!wsl::shared::string::IsEqual(name, "localhost", true) && name != "127.0.0.1")
    {
        return Url;
    }

    const auto scheme = std::string_view{Url}.substr(0, Url.find("://"));
    if (wsl::shared::string::IsEqual(scheme, "https", true))
    {
        return Url;
    }

    auto resolved = Url;
    resolved.replace(host->first, host->second, HostLoopback);
    return resolved;
}

bool IsLoopbackProxyUrl(const std::string& Url)
{
    const auto host = FindProxyHost(Url);
    if (!host)
    {
        return false;
    }

    const auto name = Url.substr(host->first, host->second);
    if (wsl::shared::string::IsEqual(name, "localhost", true))
    {
        return true;
    }

    SOCKADDR_INET address{};
    if (name.front() == '[')
    {
        address.si_family = AF_INET6;
        if (inet_pton(AF_INET6, name.substr(1, name.size() - 2).c_str(), &address.Ipv6.sin6_addr) != 1)
        {
            return false;
        }
        if (IN6_IS_ADDR_V4MAPPED(&address.Ipv6.sin6_addr))
        {
            IN_ADDR ipv4{};
            memcpy(&ipv4, IN6_GET_ADDR_V4MAPPED(&address.Ipv6.sin6_addr), sizeof(ipv4));
            return IN4_IS_ADDR_LOOPBACK(&ipv4);
        }
        return wsl::core::networking::IsLoopbackIpAddress(address);
    }

    address.si_family = AF_INET;
    return inet_pton(AF_INET, name.c_str(), &address.Ipv4.sin_addr) == 1 && wsl::core::networking::IsLoopbackIpAddress(address);
}

} // namespace wsl::windows::wslc
