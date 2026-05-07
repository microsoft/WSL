/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    VhdRequirements.cpp

Abstract:

    This file contains the implementation of the WinRT wrapper for the WSLC SDK VhdRequirements class.

--*/

#include "precomp.h"
#include "VhdRequirements.h"
#include "Microsoft.WSL.Containers.VhdRequirements.g.cpp"

namespace winrt::Microsoft::WSL::Containers::implementation {
VhdRequirements::VhdRequirements(hstring const& name, uint64_t sizeInBytes, winrt::Microsoft::WSL::Containers::VhdType const& type)
{
    throw hresult_not_implemented();
}
hstring VhdRequirements::Name()
{
    throw hresult_not_implemented();
}
void VhdRequirements::Name(hstring const& value)
{
    throw hresult_not_implemented();
}
uint64_t VhdRequirements::SizeInBytes()
{
    throw hresult_not_implemented();
}
void VhdRequirements::SizeInBytes(uint64_t value)
{
    throw hresult_not_implemented();
}
winrt::Microsoft::WSL::Containers::VhdType VhdRequirements::Type()
{
    throw hresult_not_implemented();
}
void VhdRequirements::Type(winrt::Microsoft::WSL::Containers::VhdType const& value)
{
    throw hresult_not_implemented();
}
} // namespace winrt::Microsoft::WSL::Containers::implementation
