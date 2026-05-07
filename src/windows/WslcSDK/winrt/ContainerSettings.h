/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ContainerSettings.h

Abstract:

    This file contains the definition of the WinRT wrapper for the WSLC SDK ContainerSettings class.

--*/

#pragma once
#include "Microsoft.WSL.Containers.ContainerSettings.g.h"

namespace winrt::Microsoft::WSL::Containers::implementation {
struct ContainerSettings : ContainerSettingsT<ContainerSettings>
{
    ContainerSettings() = default;

    ContainerSettings(hstring const& imageName);
    hstring ImageName();
    void ImageName(hstring const& value);
    hstring Name();
    void Name(hstring const& value);
    winrt::Microsoft::WSL::Containers::ProcessSettings InitProcess();
    void InitProcess(winrt::Microsoft::WSL::Containers::ProcessSettings const& value);
    winrt::Windows::Foundation::IReference<winrt::Microsoft::WSL::Containers::ContainerNetworkingMode> NetworkingMode();
    void NetworkingMode(winrt::Windows::Foundation::IReference<winrt::Microsoft::WSL::Containers::ContainerNetworkingMode> const& value);
    hstring HostName();
    void HostName(hstring const& value);
    hstring DomainName();
    void DomainName(hstring const& value);
    winrt::Microsoft::WSL::Containers::ContainerFlags Flags();
    void Flags(winrt::Microsoft::WSL::Containers::ContainerFlags const& value);
    winrt::Windows::Foundation::Collections::IVector<winrt::Microsoft::WSL::Containers::ContainerPortMapping> PortMappings();
    void PortMappings(winrt::Windows::Foundation::Collections::IVector<winrt::Microsoft::WSL::Containers::ContainerPortMapping> const& value);
    winrt::Windows::Foundation::Collections::IVector<winrt::Microsoft::WSL::Containers::ContainerVolume> Volumes();
    void Volumes(winrt::Windows::Foundation::Collections::IVector<winrt::Microsoft::WSL::Containers::ContainerVolume> const& value);
    winrt::Windows::Foundation::Collections::IVector<winrt::Microsoft::WSL::Containers::ContainerNamedVolume> NamedVolumes();
    void NamedVolumes(winrt::Windows::Foundation::Collections::IVector<winrt::Microsoft::WSL::Containers::ContainerNamedVolume> const& value);
};
} // namespace winrt::Microsoft::WSL::Containers::implementation
namespace winrt::Microsoft::WSL::Containers::factory_implementation {
struct ContainerSettings : ContainerSettingsT<ContainerSettings, implementation::ContainerSettings>
{
};
} // namespace winrt::Microsoft::WSL::Containers::factory_implementation
