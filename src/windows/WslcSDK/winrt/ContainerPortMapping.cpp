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
ContainerPortMapping::ContainerPortMapping(uint16_t windowsPort, uint16_t containerPort, winrt::Microsoft::WSL::Containers::PortProtocol const& protocol)
{
    throw hresult_not_implemented();
}
uint16_t ContainerPortMapping::WindowsPort()
{
    throw hresult_not_implemented();
}
void ContainerPortMapping::WindowsPort(uint16_t value)
{
    throw hresult_not_implemented();
}
uint16_t ContainerPortMapping::ContainerPort()
{
    throw hresult_not_implemented();
}
void ContainerPortMapping::ContainerPort(uint16_t value)
{
    throw hresult_not_implemented();
}
winrt::Microsoft::WSL::Containers::PortProtocol ContainerPortMapping::Protocol()
{
    throw hresult_not_implemented();
}
void ContainerPortMapping::Protocol(winrt::Microsoft::WSL::Containers::PortProtocol const& value)
{
    throw hresult_not_implemented();
}
winrt::Windows::Networking::HostName ContainerPortMapping::WindowsAddress()
{
    throw hresult_not_implemented();
}
void ContainerPortMapping::WindowsAddress(winrt::Windows::Networking::HostName const& value)
{
    throw hresult_not_implemented();
}
} // namespace winrt::Microsoft::WSL::Containers::implementation
