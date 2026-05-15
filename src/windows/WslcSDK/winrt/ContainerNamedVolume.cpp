/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ContainerNamedVolume.cpp

Abstract:

    This file contains the implementation of the WinRT wrapper for the WSLC SDK ContainerNamedVolume class.

--*/

#include "precomp.h"
#include "ContainerNamedVolume.h"
#include "Microsoft.WSL.Containers.ContainerNamedVolume.g.cpp"

namespace winrt::Microsoft::WSL::Containers::implementation {
ContainerNamedVolume::ContainerNamedVolume(hstring const& name, hstring const& containerPath, bool readOnly)
{
    throw hresult_not_implemented();
}
hstring ContainerNamedVolume::Name()
{
    throw hresult_not_implemented();
}
void ContainerNamedVolume::Name(hstring const& value)
{
    throw hresult_not_implemented();
}
hstring ContainerNamedVolume::ContainerPath()
{
    throw hresult_not_implemented();
}
void ContainerNamedVolume::ContainerPath(hstring const& value)
{
    throw hresult_not_implemented();
}
bool ContainerNamedVolume::ReadOnly()
{
    throw hresult_not_implemented();
}
void ContainerNamedVolume::ReadOnly(bool value)
{
    throw hresult_not_implemented();
}
} // namespace winrt::Microsoft::WSL::Containers::implementation
