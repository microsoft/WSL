/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ContainerNamedVolume.h

Abstract:

    This file contains the definition of the WinRT wrapper for the WSLC SDK ContainerNamedVolume class.

--*/

#pragma once
#include "Microsoft.WSL.Containers.ContainerNamedVolume.g.h"

namespace winrt::Microsoft::WSL::Containers::implementation {
struct ContainerNamedVolume : ContainerNamedVolumeT<ContainerNamedVolume>
{
    ContainerNamedVolume() = default;

    ContainerNamedVolume(hstring const& name, hstring const& containerPath, bool readOnly);
    hstring Name();
    void Name(hstring const& value);
    hstring ContainerPath();
    void ContainerPath(hstring const& value);
    bool ReadOnly();
    void ReadOnly(bool value);
};
} // namespace winrt::Microsoft::WSL::Containers::implementation
namespace winrt::Microsoft::WSL::Containers::factory_implementation {
struct ContainerNamedVolume : ContainerNamedVolumeT<ContainerNamedVolume, implementation::ContainerNamedVolume>
{
};
} // namespace winrt::Microsoft::WSL::Containers::factory_implementation
