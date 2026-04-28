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
VhdRequirements::VhdRequirements(hstring const& name, uint64_t sizeInBytes, winrt::Microsoft::WSL::Containers::VhdType const& type) :
    m_name(winrt::to_string(name))
{
    m_vhdRequirements.name = m_name.c_str();
    m_vhdRequirements.sizeBytes = sizeInBytes;
    m_vhdRequirements.type = static_cast<WslcVhdType>(type);
}

hstring VhdRequirements::Name()
{
    return winrt::to_hstring(m_name);
}

uint64_t VhdRequirements::SizeInBytes()
{
    return m_vhdRequirements.sizeBytes;
}

winrt::Microsoft::WSL::Containers::VhdType VhdRequirements::Type()
{
    return static_cast<winrt::Microsoft::WSL::Containers::VhdType>(m_vhdRequirements.type);
}

WslcVhdRequirements* VhdRequirements::ToStructPointer()
{
    return &m_vhdRequirements;
}
} // namespace winrt::Microsoft::WSL::Containers::implementation