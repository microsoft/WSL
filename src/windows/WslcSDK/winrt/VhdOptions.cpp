/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    VhdOptions.cpp

Abstract:

    This file contains the implementation of the WinRT wrapper for the WSLC SDK VhdOptions class.

--*/

#include "precomp.h"
#include "VhdOptions.h"
#include "Microsoft.WSL.Containers.VhdOptions.g.cpp"

namespace winrt::Microsoft::WSL::Containers::implementation {
VhdOptions::VhdOptions(hstring const& name, uint64_t sizeInBytes, winrt::Microsoft::WSL::Containers::VhdType const& type)
{
    throw hresult_not_implemented();
}
hstring VhdOptions::Name()
{
    throw hresult_not_implemented();
}
void VhdOptions::Name(hstring const& value)
{
    throw hresult_not_implemented();
}
uint64_t VhdOptions::SizeInBytes()
{
    throw hresult_not_implemented();
}
void VhdOptions::SizeInBytes(uint64_t value)
{
    throw hresult_not_implemented();
}
winrt::Microsoft::WSL::Containers::VhdType VhdOptions::Type()
{
    throw hresult_not_implemented();
}
void VhdOptions::Type(winrt::Microsoft::WSL::Containers::VhdType const& value)
{
    throw hresult_not_implemented();
}
void VhdOptions::SetOwner(uint32_t uid, uint32_t gid)
{
    throw hresult_not_implemented();
}
} // namespace winrt::Microsoft::WSL::Containers::implementation
