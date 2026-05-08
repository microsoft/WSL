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

ContainerNamedVolume::ContainerNamedVolume(hstring const& name, hstring const& containerPath, bool readOnly) :
    m_name(winrt::to_string(name)), m_containerPath(winrt::to_string(containerPath)), m_readOnly(readOnly)
{
    if (name.empty() || containerPath.empty())
    {
        throw hresult_invalid_argument();
    }
}

hstring ContainerNamedVolume::Name()
{
    return winrt::to_hstring(m_name);
}

void ContainerNamedVolume::Name(hstring const& value)
{
    if (m_containerNamedVolume)
    {
        throw hresult_illegal_state_change();
    }

    if (value.empty())
    {
        throw hresult_invalid_argument();
    }

    m_name = winrt::to_string(value);
}

hstring ContainerNamedVolume::ContainerPath()
{
    return winrt::to_hstring(m_containerPath);
}

void ContainerNamedVolume::ContainerPath(hstring const& value)
{
    if (m_containerNamedVolume)
    {
        throw hresult_illegal_state_change();
    }

    if (value.empty())
    {
        throw hresult_invalid_argument();
    }

    m_containerPath = winrt::to_string(value);
}

bool ContainerNamedVolume::ReadOnly()
{
    return m_readOnly;
}

void ContainerNamedVolume::ReadOnly(bool value)
{
    if (m_containerNamedVolume)
    {
        throw hresult_illegal_state_change();
    }

    m_readOnly = value;
}

WslcContainerNamedVolume ContainerNamedVolume::ToStruct()
{
    if (!m_containerNamedVolume)
    {
        m_containerNamedVolume = std::make_unique<WslcContainerNamedVolume>();
        m_containerNamedVolume->name = m_name.c_str();
        m_containerNamedVolume->containerPath = m_containerPath.c_str();
        m_containerNamedVolume->readOnly = m_readOnly;
    }

    return *m_containerNamedVolume;
}

} // namespace winrt::Microsoft::WSL::Containers::implementation
