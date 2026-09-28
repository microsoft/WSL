/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ContainerPortMapping.cpp

Abstract:

    This file contains the implementation of the WinRT wrapper for the WSLC SDK ContainerPortMapping class.

--*/

#include "precomp.h"
#include "ContainerPortMapping.h"
#include "Microsoft.WSL.Containers.ContainerPortMapping.g.cpp"

namespace winrt::Microsoft::WSL::Containers::implementation {

ContainerPortMapping::ContainerPortMapping(uint16_t windowsPort, uint16_t containerPort, winrt::Microsoft::WSL::Containers::PortProtocol const& protocol) :
    m_windowsPort(windowsPort), m_containerPort(containerPort), m_protocol(protocol)
{
}

uint16_t ContainerPortMapping::WindowsPort()
{
    return m_windowsPort;
}

void ContainerPortMapping::WindowsPort(uint16_t value)
{
    if (m_containerPortMapping)
    {
        throw hresult_illegal_state_change(L"Cannot change value after options have been applied");
    }

    m_windowsPort = value;
}

uint16_t ContainerPortMapping::ContainerPort()
{
    return m_containerPort;
}

void ContainerPortMapping::ContainerPort(uint16_t value)
{
    if (m_containerPortMapping)
    {
        throw hresult_illegal_state_change(L"Cannot change value after options have been applied");
    }

    m_containerPort = value;
}

winrt::Microsoft::WSL::Containers::PortProtocol ContainerPortMapping::Protocol()
{
    return m_protocol;
}

void ContainerPortMapping::Protocol(winrt::Microsoft::WSL::Containers::PortProtocol const& value)
{
    if (m_containerPortMapping)
    {
        throw hresult_illegal_state_change(L"Cannot change value after options have been applied");
    }

    m_protocol = value;
}

winrt::Windows::Networking::HostName ContainerPortMapping::WindowsAddress()
{
    return m_windowsAddress;
}

void ContainerPortMapping::WindowsAddress(winrt::Windows::Networking::HostName const& value)
{
    if (m_containerPortMapping)
    {
        throw hresult_illegal_state_change(L"Cannot change value after options have been applied");
    }

    if (value && value.Type() != winrt::Windows::Networking::HostNameType::Ipv4 && value.Type() != winrt::Windows::Networking::HostNameType::Ipv6)
    {
        throw hresult_invalid_argument(L"Only IP addresses are supported for port mapping");
    }

    m_windowsAddress = value;
}

WslcContainerPortMapping ContainerPortMapping::ToStruct()
{
    if (!m_containerPortMapping)
    {
        m_containerPortMapping = std::make_unique<WslcContainerPortMapping>();
        m_containerPortMapping->windowsPort = m_windowsPort;
        m_containerPortMapping->containerPort = m_containerPort;
        m_containerPortMapping->protocol = static_cast<WslcPortProtocol>(m_protocol);

        if (m_windowsAddress)
        {
            m_windowsAddressStorage = sockaddr_storage{};
            void* addrPtr = &m_windowsAddressStorage.value();

            auto rawName = winrt::to_string(m_windowsAddress.RawName());

            if (m_windowsAddress.Type() == winrt::Windows::Networking::HostNameType::Ipv4)
            {
                auto addr = static_cast<sockaddr_in*>(addrPtr);
                addr->sin_family = AF_INET;
                if (inet_pton(AF_INET, rawName.c_str(), &addr->sin_addr) != 1)
                {
                    throw winrt::hresult_invalid_argument(L"Invalid IPv4 address format");
                }
            }
            else if (m_windowsAddress.Type() == winrt::Windows::Networking::HostNameType::Ipv6)
            {
                auto addr = static_cast<sockaddr_in6*>(addrPtr);
                addr->sin6_family = AF_INET6;
                if (inet_pton(AF_INET6, rawName.c_str(), &addr->sin6_addr) != 1)
                {
                    throw winrt::hresult_invalid_argument(L"Invalid IPv6 address format");
                }
            }
            else
            {
                throw winrt::hresult_invalid_argument(L"Only IP addresses are supported for port mapping");
            }

            m_containerPortMapping->windowsAddress = &m_windowsAddressStorage.value();
        }
        else
        {
            m_containerPortMapping->windowsAddress = nullptr;
        }
    }

    return *m_containerPortMapping;
}

} // namespace winrt::Microsoft::WSL::Containers::implementation
