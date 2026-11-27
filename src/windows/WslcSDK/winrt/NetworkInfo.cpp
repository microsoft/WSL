/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    NetworkInfo.cpp

Abstract:

    This file contains the implementation of the WinRT wrapper for the WSLC SDK NetworkInfo class.

--*/

#include "precomp.h"
#include "NetworkInfo.h"
#include "Microsoft.WSL.Containers.NetworkInfo.g.cpp"

namespace {
template <typename TAddress>
winrt::Windows::Networking::HostName ToHostName(int family, const TAddress& address)
{
    wchar_t buffer[INET6_ADDRSTRLEN]{};
    THROW_LAST_ERROR_IF_NULL(InetNtopW(family, &address, buffer, ARRAYSIZE(buffer)));
    return winrt::Windows::Networking::HostName{buffer};
}

template <typename TAddress>
winrt::Windows::Networking::HostName ToSubnetHostName(int family, const TAddress& address, uint8_t prefixLength)
{
    return prefixLength == 0 ? nullptr : ToHostName(family, address);
}

winrt::Windows::Networking::HostName ToGatewayHostName(const IN_ADDR& address)
{
    return address.s_addr == 0 ? nullptr : ToHostName(AF_INET, address);
}

winrt::Windows::Networking::HostName ToGatewayHostName(const IN6_ADDR& address)
{
    return IN6_IS_ADDR_UNSPECIFIED(&address) ? nullptr : ToHostName(AF_INET6, address);
}
} // namespace

namespace winrt::Microsoft::WSL::Containers::implementation {
NetworkInfo::NetworkInfo(WslcNetworkInfo const& info)
{
    m_id = winrt::to_hstring(info.id);
    m_name = winrt::to_hstring(info.name);
    m_driver = winrt::to_hstring(info.driver);
    m_createdTimestamp = winrt::clock::from_time_t(static_cast<time_t>(info.createdUnixTime));
    m_flags = static_cast<winrt::Microsoft::WSL::Containers::NetworkFlags>(info.flags);
    m_ipv4Subnet = ToSubnetHostName(AF_INET, info.ipv4Subnet, info.ipv4PrefixLength);
    m_ipv4SubnetPrefixLength = info.ipv4PrefixLength;
    m_ipv4Gateway = ToGatewayHostName(info.ipv4Gateway);
    m_ipv6Subnet = ToSubnetHostName(AF_INET6, info.ipv6Subnet, info.ipv6PrefixLength);
    m_ipv6SubnetPrefixLength = info.ipv6PrefixLength;
    m_ipv6Gateway = ToGatewayHostName(info.ipv6Gateway);
}

hstring NetworkInfo::Id()
{
    return m_id;
}

hstring NetworkInfo::Name()
{
    return m_name;
}

hstring NetworkInfo::Driver()
{
    return m_driver;
}

winrt::Windows::Foundation::DateTime NetworkInfo::CreatedTimestamp()
{
    return m_createdTimestamp;
}

winrt::Microsoft::WSL::Containers::NetworkFlags NetworkInfo::Flags()
{
    return m_flags;
}

winrt::Windows::Networking::HostName NetworkInfo::IPv4Subnet()
{
    return m_ipv4Subnet;
}

uint8_t NetworkInfo::IPv4SubnetPrefixLength()
{
    return m_ipv4SubnetPrefixLength;
}

winrt::Windows::Networking::HostName NetworkInfo::IPv4Gateway()
{
    return m_ipv4Gateway;
}

winrt::Windows::Networking::HostName NetworkInfo::IPv6Subnet()
{
    return m_ipv6Subnet;
}

uint8_t NetworkInfo::IPv6SubnetPrefixLength()
{
    return m_ipv6SubnetPrefixLength;
}

winrt::Windows::Networking::HostName NetworkInfo::IPv6Gateway()
{
    return m_ipv6Gateway;
}
} // namespace winrt::Microsoft::WSL::Containers::implementation