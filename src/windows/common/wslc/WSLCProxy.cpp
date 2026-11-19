// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "WSLCProxy.h"
#include "WslCoreNetworkingSupport.h"
#include "stringshared.h"
#include <array>
#include <cstring>
#include <string_view>
#include <winhttp.h>

namespace wsl::windows::wslc {

namespace {

    constexpr std::array<std::pair<std::string_view, std::string_view>, 3> c_proxyNames{
        {{"http_proxy", "HTTP_PROXY"}, {"https_proxy", "HTTPS_PROXY"}, {"no_proxy", "NO_PROXY"}}};

    std::string_view EnvironmentName(std::string_view Entry)
    {
        return Entry.substr(0, Entry.find('='));
    }

    std::optional<std::wstring_view> FindProxyHost(std::wstring_view url)
    {
        const auto schemeEnd = url.find(L"://");
        if (schemeEnd == 0)
        {
            return std::nullopt;
        }

        const auto authorityOffset = schemeEnd == std::wstring_view::npos ? 0 : schemeEnd + 3;
        // Use HTTP only for parsing so schemeless and SOCKS proxy URLs keep their original form.
        constexpr std::wstring_view c_scheme = L"http://";
        const auto normalizedUrl = std::wstring{c_scheme} + std::wstring{url.substr(authorityOffset)};
        URL_COMPONENTS components{};
        components.dwStructSize = sizeof(components);
        components.dwHostNameLength = -1;
        if (!WinHttpCrackUrl(normalizedUrl.c_str(), 0, 0, &components) || components.dwHostNameLength == 0)
        {
            return std::nullopt;
        }

        const auto* hostBegin = components.lpszHostName;
        const auto hostOffset = static_cast<size_t>(hostBegin - normalizedUrl.data()) - c_scheme.size() + authorityOffset;
        return url.substr(hostOffset, components.dwHostNameLength);
    }

} // namespace

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

std::string ResolveProxyUrl(const std::string& Url, const std::string& HostLoopback)
{
    if (HostLoopback.empty() || Url.empty())
    {
        return Url;
    }

    const auto byteCount = gsl::narrow<int>(Url.size());
    std::wstring wideUrl(Url.size(), L'\0');
    const auto wideCount = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, Url.data(), byteCount, wideUrl.data(), byteCount);
    if (wideCount == 0)
    {
        THROW_LAST_ERROR_IF(GetLastError() != ERROR_NO_UNICODE_TRANSLATION);
        return Url;
    }

    wideUrl.resize(wideCount);
    const auto host = FindProxyHost(wideUrl);
    if (!host)
    {
        return Url;
    }

    const auto name = *host;
    if (!wsl::shared::string::IsEqual(name, L"localhost", true) && name != L"127.0.0.1")
    {
        return Url;
    }

    const auto scheme = std::wstring_view{wideUrl}.substr(0, wideUrl.find(L"://"));
    if (wsl::shared::string::IsEqual(scheme, L"https", true))
    {
        return Url;
    }

    const auto hostOffset = static_cast<size_t>(host->data() - wideUrl.data());
    wideUrl.replace(hostOffset, host->size(), wsl::shared::string::MultiByteToWide(HostLoopback));
    return wsl::windows::common::string::WideToMultiByte(wideUrl);
}

bool IsLoopbackProxyUrl(const std::string& Url)
{
    const auto wideUrl = wsl::shared::string::MultiByteToWide(Url);
    const auto host = FindProxyHost(wideUrl);
    if (!host)
    {
        return false;
    }

    const auto name = *host;
    if (wsl::shared::string::IsEqual(name, L"localhost", true))
    {
        return true;
    }

    SOCKADDR_INET address{};
    if (name.front() == L'[')
    {
        address.si_family = AF_INET6;
        const std::wstring addressString{name.substr(1, name.size() - 2)};
        if (InetPtonW(AF_INET6, addressString.c_str(), &address.Ipv6.sin6_addr) != 1)
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
    const std::wstring addressString{name};
    return InetPtonW(AF_INET, addressString.c_str(), &address.Ipv4.sin_addr) == 1 && wsl::core::networking::IsLoopbackIpAddress(address);
}

} // namespace wsl::windows::wslc
