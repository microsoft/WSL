// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "WSLCProxy.h"
#include "WslCoreNetworkingSupport.h"
#include "stringshared.h"
#include <array>
#include <cstring>
#include <winhttp.h>

namespace wsl::windows::wslc {

namespace {

    constexpr std::array<std::pair<std::string_view, std::string_view>, 3> c_proxyNames{
        {{"http_proxy", "HTTP_PROXY"}, {"https_proxy", "HTTPS_PROXY"}, {"no_proxy", "NO_PROXY"}}};

    std::string_view EnvironmentName(std::string_view Entry)
    {
        return Entry.substr(0, Entry.find('='));
    }

    std::optional<std::string_view> FindProxyHost(std::string_view url)
    {
        const auto schemeEnd = url.find("://");
        if (schemeEnd == 0)
        {
            return std::nullopt;
        }

        const auto authorityOffset = schemeEnd == std::string_view::npos ? 0 : schemeEnd + 3;
        // Use HTTP only for parsing so schemeless and SOCKS proxy URLs keep their original form.
        constexpr std::wstring_view c_scheme = L"http://";
        const auto normalizedUrl = std::wstring{c_scheme} + wsl::shared::string::MultiByteToWide(std::string{url.substr(authorityOffset)});
        URL_COMPONENTS components{};
        components.dwStructSize = sizeof(components);
        components.dwHostNameLength = -1;
        if (!WinHttpCrackUrl(normalizedUrl.c_str(), 0, 0, &components) || components.dwHostNameLength == 0)
        {
            return std::nullopt;
        }

        const auto* hostBegin = components.lpszHostName;
        const auto* hostEnd = hostBegin + components.dwHostNameLength;
        const auto prefix = wsl::shared::string::WideToMultiByte(std::wstring{normalizedUrl.c_str() + c_scheme.size(), hostBegin});
        const auto host = wsl::shared::string::WideToMultiByte(std::wstring{hostBegin, hostEnd});
        return url.substr(authorityOffset + prefix.size(), host.size());
    }

} // namespace

bool IsProxyEnvironmentVariable(std::string_view Entry)
{
    const auto name = EnvironmentName(Entry);
    return Entry.find('=') != std::string_view::npos &&
           std::ranges::any_of(c_proxyNames, [&](const auto& names) { return name == names.first || name == names.second; });
}

void AddProxyEnvironmentDefaults(std::vector<std::string>& Environment, const std::vector<std::string>& Defaults)
{
    const auto explicitCount = Environment.size();
    for (const auto& entry : Defaults)
    {
        const auto name = EnvironmentName(entry);
        const auto aliases =
            std::ranges::find_if(c_proxyNames, [&](const auto& names) { return name == names.first || name == names.second; });
        if (aliases != c_proxyNames.end() &&
            std::ranges::any_of(Environment.begin(), Environment.begin() + explicitCount, [&](const auto& value) {
                const auto explicitName = EnvironmentName(value);
                return explicitName == aliases->first || explicitName == aliases->second;
            }))
        {
            continue;
        }

        if (std::ranges::none_of(Environment, [&](const auto& value) { return EnvironmentName(value) == name; }))
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
        if (name == "http_proxy" || name == "HTTP_PROXY" || name == "https_proxy" || name == "HTTPS_PROXY")
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

    const auto name = *host;
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
    const auto hostOffset = static_cast<size_t>(host->data() - Url.data());
    resolved.replace(hostOffset, host->size(), HostLoopback);
    return resolved;
}

bool IsLoopbackProxyUrl(const std::string& Url)
{
    const auto host = FindProxyHost(Url);
    if (!host)
    {
        return false;
    }

    const auto name = *host;
    if (wsl::shared::string::IsEqual(name, "localhost", true))
    {
        return true;
    }

    SOCKADDR_INET address{};
    if (name.front() == '[')
    {
        address.si_family = AF_INET6;
        const auto addressString = std::string{name.substr(1, name.size() - 2)};
        if (inet_pton(AF_INET6, addressString.c_str(), &address.Ipv6.sin6_addr) != 1)
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
    const auto addressString = std::string{name};
    return inet_pton(AF_INET, addressString.c_str(), &address.Ipv4.sin_addr) == 1 && wsl::core::networking::IsLoopbackIpAddress(address);
}

} // namespace wsl::windows::wslc
