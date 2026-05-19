/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ContainerVolume.h

Abstract:

    This file contains the definition of the WinRT wrapper for the WSLC SDK ContainerVolume class.

--*/

#pragma once
#include "Microsoft.WSL.Containers.ContainerVolume.g.h"

namespace winrt::Microsoft::WSL::Containers::implementation {
struct ContainerVolume : ContainerVolumeT<ContainerVolume>
{
    ContainerVolume() = default;

    ContainerVolume(hstring const& windowsPath, hstring const& containerPath, bool readOnly);
    hstring WindowsPath();
    void WindowsPath(hstring const& value);
    hstring ContainerPath();
    void ContainerPath(hstring const& value);
    bool ReadOnly();
    void ReadOnly(bool value);
};
} // namespace winrt::Microsoft::WSL::Containers::implementation
namespace winrt::Microsoft::WSL::Containers::factory_implementation {
struct ContainerVolume : ContainerVolumeT<ContainerVolume, implementation::ContainerVolume>
{
};
} // namespace winrt::Microsoft::WSL::Containers::factory_implementation
