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

ContainerVolume::ContainerVolume(hstring const& windowsPath, hstring const& containerPath, bool readOnly) :
    m_windowsPath(windowsPath), m_containerPath(winrt::to_string(containerPath)), m_readOnly(readOnly)
{
    if (windowsPath.empty())
    {
        throw hresult_invalid_argument(L"Windows path cannot be empty");
    }

    if (containerPath.empty())
    {
        throw hresult_invalid_argument(L"Container path cannot be empty");
    }
}

hstring ContainerVolume::WindowsPath()
{
    return hstring(m_windowsPath);
}

void ContainerVolume::WindowsPath(hstring const& value)
{
    if (m_containerVolume)
    {
        throw hresult_illegal_state_change(L"Cannot change value after options have been applied");
    }

    if (value.empty())
    {
        throw hresult_invalid_argument(L"Windows path cannot be empty");
    }

    m_windowsPath = value;
}

hstring ContainerVolume::ContainerPath()
{
    return winrt::to_hstring(m_containerPath);
}

void ContainerVolume::ContainerPath(hstring const& value)
{
    if (m_containerVolume)
    {
        throw hresult_illegal_state_change(L"Cannot change value after options have been applied");
    }

    if (value.empty())
    {
        throw hresult_invalid_argument(L"Container path cannot be empty");
    }

    m_containerPath = winrt::to_string(value);
}

bool ContainerVolume::ReadOnly()
{
    return m_readOnly;
}

void ContainerVolume::ReadOnly(bool value)
{
    if (m_containerVolume)
    {
        throw hresult_illegal_state_change(L"Cannot change value after options have been applied");
    }

    m_readOnly = value;
}

WslcContainerVolume ContainerVolume::ToStruct()
{
    if (!m_containerVolume)
    {
        m_containerVolume = std::make_unique<WslcContainerVolume>();
        m_containerVolume->windowsPath = m_windowsPath.c_str();
        m_containerVolume->containerPath = m_containerPath.c_str();
        m_containerVolume->readOnly = m_readOnly;
    }

    return *m_containerVolume;
}

} // namespace winrt::Microsoft::WSL::Containers::implementation
