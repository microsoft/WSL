/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ContainerVolume.cpp

Abstract:

    This file contains the implementation of the WinRT wrapper for the WSLC SDK ContainerVolume class.

--*/

#include "precomp.h"
#include "ContainerVolume.h"
#include "Microsoft.WSL.Containers.ContainerVolume.g.cpp"

namespace winrt::Microsoft::WSL::Containers::implementation {
ContainerVolume::ContainerVolume(hstring const& windowsPath, hstring const& containerPath, bool readOnly)
{
    throw hresult_not_implemented();
}
hstring ContainerVolume::WindowsPath()
{
    throw hresult_not_implemented();
}
void ContainerVolume::WindowsPath(hstring const& value)
{
    throw hresult_not_implemented();
}
hstring ContainerVolume::ContainerPath()
{
    throw hresult_not_implemented();
}
void ContainerVolume::ContainerPath(hstring const& value)
{
    throw hresult_not_implemented();
}
bool ContainerVolume::ReadOnly()
{
    throw hresult_not_implemented();
}
void ContainerVolume::ReadOnly(bool value)
{
    throw hresult_not_implemented();
}
} // namespace winrt::Microsoft::WSL::Containers::implementation
