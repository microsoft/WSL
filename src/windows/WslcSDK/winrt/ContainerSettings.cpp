/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ContainerSettings.cpp

Abstract:

    This file contains the implementation of the WinRT wrapper for the WSLC SDK ContainerSettings class.

--*/

#include "precomp.h"
#include "ContainerSettings.h"
#include "Microsoft.WSL.Containers.ContainerSettings.g.cpp"

namespace winrt::Microsoft::WSL::Containers::implementation {
ContainerSettings::ContainerSettings(hstring const& imageName)
{
    throw hresult_not_implemented();
}
hstring ContainerSettings::ImageName()
{
    throw hresult_not_implemented();
}
void ContainerSettings::ImageName(hstring const& value)
{
    throw hresult_not_implemented();
}
hstring ContainerSettings::Name()
{
    throw hresult_not_implemented();
}
void ContainerSettings::Name(hstring const& value)
{
    throw hresult_not_implemented();
}
winrt::Microsoft::WSL::Containers::ProcessSettings ContainerSettings::InitProcess()
{
    throw hresult_not_implemented();
}
void ContainerSettings::InitProcess(winrt::Microsoft::WSL::Containers::ProcessSettings const& value)
{
    throw hresult_not_implemented();
}
winrt::Windows::Foundation::IReference<winrt::Microsoft::WSL::Containers::ContainerNetworkingMode> ContainerSettings::NetworkingMode()
{
    throw hresult_not_implemented();
}
void ContainerSettings::NetworkingMode(winrt::Windows::Foundation::IReference<winrt::Microsoft::WSL::Containers::ContainerNetworkingMode> const& value)
{
    throw hresult_not_implemented();
}
hstring ContainerSettings::HostName()
{
    throw hresult_not_implemented();
}
void ContainerSettings::HostName(hstring const& value)
{
    throw hresult_not_implemented();
}
hstring ContainerSettings::DomainName()
{
    throw hresult_not_implemented();
}
void ContainerSettings::DomainName(hstring const& value)
{
    throw hresult_not_implemented();
}
winrt::Microsoft::WSL::Containers::ContainerFlags ContainerSettings::Flags()
{
    throw hresult_not_implemented();
}
void ContainerSettings::Flags(winrt::Microsoft::WSL::Containers::ContainerFlags const& value)
{
    throw hresult_not_implemented();
}
winrt::Windows::Foundation::Collections::IVector<winrt::Microsoft::WSL::Containers::ContainerPortMapping> ContainerSettings::PortMappings()
{
    throw hresult_not_implemented();
}
void ContainerSettings::PortMappings(winrt::Windows::Foundation::Collections::IVector<winrt::Microsoft::WSL::Containers::ContainerPortMapping> const& value)
{
    throw hresult_not_implemented();
}
winrt::Windows::Foundation::Collections::IVector<winrt::Microsoft::WSL::Containers::ContainerVolume> ContainerSettings::Volumes()
{
    throw hresult_not_implemented();
}
void ContainerSettings::Volumes(winrt::Windows::Foundation::Collections::IVector<winrt::Microsoft::WSL::Containers::ContainerVolume> const& value)
{
    throw hresult_not_implemented();
}
winrt::Windows::Foundation::Collections::IVector<winrt::Microsoft::WSL::Containers::ContainerNamedVolume> ContainerSettings::NamedVolumes()
{
    throw hresult_not_implemented();
}
void ContainerSettings::NamedVolumes(winrt::Windows::Foundation::Collections::IVector<winrt::Microsoft::WSL::Containers::ContainerNamedVolume> const& value)
{
    throw hresult_not_implemented();
}
} // namespace winrt::Microsoft::WSL::Containers::implementation
