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

VhdOptions::VhdOptions(hstring const& name, uint64_t sizeInBytes, VhdType const& type) :
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

hstring VhdOptions::Name()
{
    return winrt::to_hstring(m_name);
}

void VhdOptions::Name(hstring const& value)
{
    if (m_VhdOptions)
    {
        throw hresult_illegal_state_change();
    }

    if (value.empty())
    {
        throw hresult_invalid_argument();
    }

    m_name = winrt::to_string(value);
}

uint64_t VhdOptions::SizeInBytes()
{
    return m_sizeInBytes;
}

void VhdOptions::SizeInBytes(uint64_t value)
{
    if (m_VhdOptions)
    {
        throw hresult_illegal_state_change();
    }

    if (value == 0)
    {
        throw hresult_invalid_argument();
    }

    m_sizeInBytes = value;
}

VhdType VhdOptions::Type()
{
    return m_type;
}

void VhdOptions::Type(VhdType const& value)
{
    if (m_VhdOptions)
    {
        throw hresult_illegal_state_change();
    }

    if (value != VhdType::Dynamic)
    {
        throw hresult_not_implemented();
    }

    m_type = value;
}

void VhdOptions::SetOwner(uint32_t uid, uint32_t gid)
{
    throw hresult_not_implemented();
}

WslcVhdRequirements* VhdOptions::ToStructPointer()
{
    if (!m_VhdOptions)
    {
        m_VhdOptions = std::make_unique<WslcVhdRequirements>();
        m_VhdOptions->name = m_name.c_str();
        m_VhdOptions->sizeBytes = m_sizeInBytes;
        m_VhdOptions->type = static_cast<WslcVhdType>(m_type);
    }

    return m_VhdOptions.get();
}

} // namespace winrt::Microsoft::WSL::Containers::implementation
