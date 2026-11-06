/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    NetworkSettings.cpp

Abstract:

    This file contains the implementation of the WinRT wrapper for the WSLC SDK NetworkSettings class.

--*/

#include "precomp.h"
#include "NetworkSettings.h"
#include "Microsoft.WSL.Containers.NetworkSettings.g.cpp"

namespace winrt::Microsoft::WSL::Containers::implementation {

namespace {

    void ValidateIpAddress(winrt::Windows::Networking::HostName const& value)
    {
        if (value && value.Type() != winrt::Windows::Networking::HostNameType::Ipv4 &&
            value.Type() != winrt::Windows::Networking::HostNameType::Ipv6)
        {
            throw hresult_invalid_argument(L"Only IP addresses are supported for network settings");
        }
    }

    void ParseIpAddress(winrt::Windows::Networking::HostName const& value, IN_ADDR& ipv4Address, IN6_ADDR& ipv6Address)
    {
        auto rawName = winrt::to_string(value.RawName());
        if (value.Type() == winrt::Windows::Networking::HostNameType::Ipv4)
        {
            if (inet_pton(AF_INET, rawName.c_str(), &ipv4Address) != 1)
            {
                throw hresult_invalid_argument(L"Invalid IPv4 address format");
            }

            return;
        }

        if (inet_pton(AF_INET6, rawName.c_str(), &ipv6Address) != 1)
        {
            throw hresult_invalid_argument(L"Invalid IPv6 address format");
        }
    }

} // namespace

NetworkSettings::NetworkSettings(hstring const& name) : m_name(winrt::to_string(name))
{
    if (m_name.empty())
    {
        throw hresult_invalid_argument(L"Name cannot be empty");
    }
}

void NetworkSettings::EnsureNotApplied() const
{
    if (m_networkSettings)
    {
        throw hresult_illegal_state_change(L"Cannot change value after options have been applied");
    }
}

hstring NetworkSettings::Name()
{
    return winrt::to_hstring(m_name);
}

void NetworkSettings::Name(hstring const& value)
{
    EnsureNotApplied();

    if (value.empty())
    {
        throw hresult_invalid_argument(L"Name cannot be empty");
    }

    m_name = winrt::to_string(value);
}

winrt::Microsoft::WSL::Containers::NetworkFlags NetworkSettings::Flags()
{
    return m_flags;
}

void NetworkSettings::Flags(winrt::Microsoft::WSL::Containers::NetworkFlags const& value)
{
    EnsureNotApplied();
    m_flags = value;
}

winrt::Windows::Networking::HostName NetworkSettings::Subnet()
{
    return m_subnet;
}

void NetworkSettings::Subnet(winrt::Windows::Networking::HostName const& value)
{
    EnsureNotApplied();
    ValidateIpAddress(value);
    m_subnet = value;
}

uint8_t NetworkSettings::SubnetPrefixLength()
{
    return m_subnetPrefixLength;
}

void NetworkSettings::SubnetPrefixLength(uint8_t value)
{
    EnsureNotApplied();
    m_subnetPrefixLength = value;
}

winrt::Windows::Networking::HostName NetworkSettings::Gateway()
{
    return m_gateway;
}

void NetworkSettings::Gateway(winrt::Windows::Networking::HostName const& value)
{
    EnsureNotApplied();
    ValidateIpAddress(value);
    m_gateway = value;
}

WslcNetworkSettings* NetworkSettings::ToStructPointer()
{
    if (m_networkSettings)
    {
        return m_networkSettings.get();
    }

    if (static_cast<bool>(m_subnet) != (m_subnetPrefixLength != 0))
    {
        throw hresult_invalid_argument(L"Subnet and SubnetPrefixLength must be set together");
    }

    if (m_gateway && !m_subnet)
    {
        throw hresult_invalid_argument(L"Gateway requires a subnet");
    }

    if (m_gateway && m_gateway.Type() != m_subnet.Type())
    {
        throw hresult_invalid_argument(L"Gateway and subnet must use the same address family");
    }

    auto settings = std::make_unique<WslcNetworkSettings>();
    settings->name = m_name.c_str();
    settings->flags = static_cast<WslcNetworkFlags>(m_flags);

    if (m_subnet)
    {
        ParseIpAddress(m_subnet, settings->ipv4Subnet, settings->ipv6Subnet);
        if (m_subnet.Type() == winrt::Windows::Networking::HostNameType::Ipv4)
        {
            settings->ipv4PrefixLength = m_subnetPrefixLength;
        }
        else
        {
            settings->ipv6PrefixLength = m_subnetPrefixLength;
        }
    }

    if (m_gateway)
    {
        ParseIpAddress(m_gateway, settings->ipv4Gateway, settings->ipv6Gateway);
    }

    m_networkSettings = std::move(settings);
    return m_networkSettings.get();
}

} // namespace winrt::Microsoft::WSL::Containers::implementation
