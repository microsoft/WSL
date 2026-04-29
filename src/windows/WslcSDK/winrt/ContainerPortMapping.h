/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ContainerPortMapping.h

Abstract:

    This file contains the definition of the WinRT wrapper for the WSLC SDK ContainerPortMapping class.

--*/

#pragma once
#include "Microsoft.WSL.Containers.ContainerPortMapping.g.h"
#include "Helpers.h"

namespace winrt::Microsoft::WSL::Containers::implementation {
struct ContainerPortMapping : ContainerPortMappingT<ContainerPortMapping>
{
    ContainerPortMapping() = default;

    ContainerPortMapping(uint16_t windowsPort, uint16_t containerPort, winrt::Microsoft::WSL::Containers::PortProtocol const& protocol);
    uint16_t WindowsPort();
    void WindowsPort(uint16_t value);
    uint16_t ContainerPort();
    void ContainerPort(uint16_t value);
    winrt::Microsoft::WSL::Containers::PortProtocol Protocol();
    void Protocol(winrt::Microsoft::WSL::Containers::PortProtocol const& value);
    winrt::Windows::Networking::HostName WindowsAddress();
    void WindowsAddress(winrt::Windows::Networking::HostName const& value);

    WslcContainerPortMapping ToStruct();

private:
    uint16_t m_windowsPort{};
    uint16_t m_containerPort{};
    winrt::Microsoft::WSL::Containers::PortProtocol m_protocol{ winrt::Microsoft::WSL::Containers::PortProtocol::TCP };
    winrt::Windows::Networking::HostName m_windowsAddress{ nullptr };

    std::unique_ptr<WslcContainerPortMapping> m_containerPortMapping;
};
} // namespace winrt::Microsoft::WSL::Containers::implementation

namespace winrt::Microsoft::WSL::Containers::factory_implementation {
struct ContainerPortMapping : ContainerPortMappingT<ContainerPortMapping, implementation::ContainerPortMapping>
{
};
} // namespace winrt::Microsoft::WSL::Containers::factory_implementation

DEFINE_TYPE_HELPERS(ContainerPortMapping);
