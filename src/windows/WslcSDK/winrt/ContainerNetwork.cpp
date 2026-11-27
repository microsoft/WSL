/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ContainerNetwork.cpp

Abstract:

    This file contains the implementation of the WinRT wrapper for the WSLC SDK ContainerNetwork class.

--*/

#include "precomp.h"
#include "ContainerNetwork.h"
#include "Microsoft.WSL.Containers.ContainerNetwork.g.cpp"

namespace winrt::Microsoft::WSL::Containers::implementation {

ContainerNetwork::ContainerNetwork(hstring const& name) : m_name(winrt::to_string(name))
{
    if (name.empty())
    {
        throw hresult_invalid_argument(L"Network name cannot be empty");
    }
}

hstring ContainerNetwork::Name()
{
    return winrt::to_hstring(m_name);
}

void ContainerNetwork::Name(hstring const& value)
{
    if (m_containerNetwork)
    {
        throw hresult_illegal_state_change(L"Cannot change value after options have been applied");
    }

    if (value.empty())
    {
        throw hresult_invalid_argument(L"Network name cannot be empty");
    }

    m_name = winrt::to_string(value);
}

winrt::Windows::Foundation::Collections::IVector<hstring> ContainerNetwork::Aliases()
{
    return m_aliases;
}

void ContainerNetwork::Aliases(winrt::Windows::Foundation::Collections::IVector<hstring> const& value)
{
    if (m_containerNetwork)
    {
        throw hresult_illegal_state_change(L"Cannot change value after options have been applied");
    }

    if (!value)
    {
        throw hresult_error(E_POINTER, L"Value cannot be null");
    }

    m_aliases = value;
}

winrt::Windows::Networking::HostName ContainerNetwork::IPv4Address()
{
    return m_ipv4Address;
}

void ContainerNetwork::IPv4Address(winrt::Windows::Networking::HostName const& value)
{
    if (m_containerNetwork)
    {
        throw hresult_illegal_state_change(L"Cannot change value after options have been applied");
    }

    if (value && value.Type() != winrt::Windows::Networking::HostNameType::Ipv4)
    {
        throw hresult_invalid_argument(L"Value must be an IPv4 address");
    }

    m_ipv4Address = value;
}

winrt::Windows::Networking::HostName ContainerNetwork::IPv6Address()
{
    return m_ipv6Address;
}

void ContainerNetwork::IPv6Address(winrt::Windows::Networking::HostName const& value)
{
    if (m_containerNetwork)
    {
        throw hresult_illegal_state_change(L"Cannot change value after options have been applied");
    }

    if (value && value.Type() != winrt::Windows::Networking::HostNameType::Ipv6)
    {
        throw hresult_invalid_argument(L"Value must be an IPv6 address");
    }

    m_ipv6Address = value;
}

WslcContainerNetwork ContainerNetwork::ToStruct()
{
    if (!m_containerNetwork)
    {
        if (m_ipv4Address && inet_pton(AF_INET, winrt::to_string(m_ipv4Address.RawName()).c_str(), &m_ipv4AddressStruct) != 1)
        {
            throw hresult_invalid_argument(L"Invalid IPv4 address format");
        }

        if (m_ipv6Address && inet_pton(AF_INET6, winrt::to_string(m_ipv6Address.RawName()).c_str(), &m_ipv6AddressStruct) != 1)
        {
            throw hresult_invalid_argument(L"Invalid IPv6 address format");
        }

        m_aliasStrings.clear();
        m_aliasStrings.reserve(m_aliases.Size());
        for (auto const& alias : m_aliases)
        {
            m_aliasStrings.push_back(winrt::to_string(alias));
        }

        m_aliasPointers.clear();
        m_aliasPointers.reserve(m_aliasStrings.size());
        for (auto const& alias : m_aliasStrings)
        {
            m_aliasPointers.push_back(alias.c_str());
        }

        m_containerNetwork = std::make_unique<WslcContainerNetwork>();
        m_containerNetwork->name = m_name.c_str();
        m_containerNetwork->aliases = m_aliasPointers.empty() ? nullptr : m_aliasPointers.data();
        m_containerNetwork->aliasCount = static_cast<uint32_t>(m_aliasPointers.size());
        m_containerNetwork->ipv4Address = m_ipv4Address ? &m_ipv4AddressStruct : nullptr;
        m_containerNetwork->ipv6Address = m_ipv6Address ? &m_ipv6AddressStruct : nullptr;
    }

    return *m_containerNetwork;
}

} // namespace winrt::Microsoft::WSL::Containers::implementation
