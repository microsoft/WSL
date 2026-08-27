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

VhdOptions::VhdOptions(hstring const& name, uint64_t size, VhdType const& type) :
    m_name(winrt::to_string(name)), m_size(size), m_type(type)
{
    if (size == 0)
    {
        throw hresult_invalid_argument(L"VHD size cannot be zero");
    }
}

hstring VhdOptions::Name()
{
    return winrt::to_hstring(m_name);
}

void VhdOptions::Name(hstring const& value)
{
    if (m_vhdOptions)
    {
        throw hresult_illegal_state_change(L"Cannot change value after options have been applied");
    }

    m_name = winrt::to_string(value);
}

uint64_t VhdOptions::Size()
{
    return m_size;
}

void VhdOptions::Size(uint64_t value)
{
    if (m_vhdOptions)
    {
        throw hresult_illegal_state_change(L"Cannot change value after options have been applied");
    }

    if (value == 0)
    {
        throw hresult_invalid_argument(L"VHD size cannot be zero");
    }

    m_size = value;
}

VhdType VhdOptions::Type()
{
    return m_type;
}

void VhdOptions::Type(VhdType const& value)
{
    if (m_vhdOptions)
    {
        throw hresult_illegal_state_change(L"Cannot change value after options have been applied");
    }

    m_type = value;
}

winrt::Windows::Foundation::IReference<winrt::Microsoft::WSL::Containers::VhdOwner> VhdOptions::Owner()
{
    return m_owner;
}

void VhdOptions::Owner(winrt::Windows::Foundation::IReference<winrt::Microsoft::WSL::Containers::VhdOwner> const& value)
{
    if (m_vhdOptions)
    {
        throw hresult_illegal_state_change(L"Cannot change value after options have been applied");
    }

    m_owner = value;
}

WslcVhdRequirements* VhdOptions::ToStructPointer()
{
    if (!m_vhdOptions)
    {
        m_vhdOptions = std::make_unique<WslcVhdRequirements>();
        m_vhdOptions->name = m_name.c_str();
        m_vhdOptions->sizeBytes = m_size;
        m_vhdOptions->type = static_cast<WslcVhdType>(m_type);

        if (m_owner != nullptr)
        {
            auto owner = m_owner.Value();
            m_vhdOptions->uid = owner.Uid;
            m_vhdOptions->gid = owner.Gid;
            WI_SetFlag(m_vhdOptions->flags, WSLC_VHD_REQ_FLAG_OWNER);
        }
    }

    return m_vhdOptions.get();
}

} // namespace winrt::Microsoft::WSL::Containers::implementation
