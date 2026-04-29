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
        throw hresult_illegal_state_change();
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
        throw hresult_illegal_state_change();
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
        throw hresult_illegal_state_change();
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
        throw hresult_illegal_state_change();
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
            throw hresult_not_implemented();
        }
        else
        {
            m_containerPortMapping->windowsAddress = nullptr;
        }
    }

    return *m_containerPortMapping;
}

} // namespace winrt::Microsoft::WSL::Containers::implementation
