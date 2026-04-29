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

VhdRequirements::VhdRequirements(hstring const& name, uint64_t sizeInBytes, VhdType const& type) :
    m_name(winrt::to_string(name)), m_sizeInBytes(sizeInBytes), m_type(type)
{
    if (name.empty() || sizeInBytes == 0)
    {
        throw hresult_invalid_argument();
    }

    if (type != VhdType::Dynamic)
    {
        throw hresult_not_implemented();
    }
}

hstring VhdRequirements::Name()
{
    return winrt::to_hstring(m_name);
}

void VhdRequirements::Name(hstring const& value)
{
    if (m_vhdRequirements)
    {
        throw hresult_illegal_state_change();
    }

    if (value.empty())
    {
        throw hresult_invalid_argument();
    }

    m_name = winrt::to_string(value);
}

uint64_t VhdRequirements::SizeInBytes()
{
    return m_sizeInBytes;
}

void VhdRequirements::SizeInBytes(uint64_t value)
{
    if (m_vhdRequirements)
    {
        throw hresult_illegal_state_change();
    }

    if (value == 0)
    {
        throw hresult_invalid_argument();
    }

    m_sizeInBytes = value;
}

VhdType VhdRequirements::Type()
{
    return m_type;
}

void VhdRequirements::Type(VhdType const& value)
{
    if (m_vhdRequirements)
    {
        throw hresult_illegal_state_change();
    }

    if (value != VhdType::Dynamic)
    {
        throw hresult_not_implemented();
    }

    m_type = value;
}

WslcVhdRequirements* VhdRequirements::ToStructPointer()
{
    if (!m_vhdRequirements)
    {
        m_vhdRequirements = std::make_unique<WslcVhdRequirements>();
        m_vhdRequirements->name = m_name.c_str();
        m_vhdRequirements->sizeBytes = m_sizeInBytes;
        m_vhdRequirements->type = static_cast<WslcVhdType>(m_type);
    }

    return m_vhdRequirements.get();
}

} // namespace winrt::Microsoft::WSL::Containers::implementation