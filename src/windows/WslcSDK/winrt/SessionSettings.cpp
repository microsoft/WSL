/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    SessionSettings.cpp

Abstract:

    This file contains the implementation of the WinRT wrapper for the WSLC SDK SessionSettings class.

--*/

#include "precomp.h"
#include "SessionSettings.h"
#include "Microsoft.WSL.Containers.SessionSettings.g.cpp"

namespace WSLC = winrt::Microsoft::WSL::Containers;

namespace winrt::Microsoft::WSL::Containers {
implementation::SessionSettings::SessionSettings(hstring const& name, hstring const& storagePath) :
    m_name(name), m_storagePath(storagePath)
{
    winrt::check_hresult(WslcInitSessionSettings(m_name.c_str(), m_storagePath.c_str(), &m_sessionSettings));
}

hstring implementation::SessionSettings::Name()
{
    return hstring{m_name};
}

hstring implementation::SessionSettings::StoragePath()
{
    return hstring{m_storagePath};
}

uint32_t implementation::SessionSettings::CpuCount()
{
    return m_cpuCount;
}

void implementation::SessionSettings::CpuCount(uint32_t value)
{
    m_cpuCount = value;
    winrt::check_hresult(WslcSetSessionSettingsCpuCount(&m_sessionSettings, m_cpuCount));
}

uint32_t implementation::SessionSettings::MemoryMb()
{
    return m_memoryMb;
}

void implementation::SessionSettings::MemoryMb(uint32_t value)
{
    m_memoryMb = value;
    winrt::check_hresult(WslcSetSessionSettingsMemory(&m_sessionSettings, m_memoryMb));
}

uint32_t implementation::SessionSettings::TimeoutMS()
{
    return m_timeoutMS;
}

void implementation::SessionSettings::TimeoutMS(uint32_t value)
{
    m_timeoutMS = value;
    winrt::check_hresult(WslcSetSessionSettingsTimeout(&m_sessionSettings, m_timeoutMS));
}

WSLC::VhdRequirements implementation::SessionSettings::VhdRequirements()
{
    return m_vhdRequirements;
}

void implementation::SessionSettings::VhdRequirements(WSLC::VhdRequirements const& value)
{
    m_vhdRequirements = value;
    auto vhdRequirements =
        m_vhdRequirements ? winrt::get_self<implementation::VhdRequirements>(m_vhdRequirements)->ToStructPointer() : nullptr;
    winrt::check_hresult(WslcSetSessionSettingsVhd(&m_sessionSettings, vhdRequirements));
}

WSLC::SessionFeatureFlags implementation::SessionSettings::FeatureFlags()
{
    return m_featureFlags;
}

void implementation::SessionSettings::FeatureFlags(WSLC::SessionFeatureFlags const& value)
{
    m_featureFlags = value;
    winrt::check_hresult(WslcSetSessionSettingsFeatureFlags(&m_sessionSettings, static_cast<WslcSessionFeatureFlags>(m_featureFlags)));
}

WslcSessionSettings* implementation::SessionSettings::ToStructPointer()
{
    return &m_sessionSettings;
}
} // namespace winrt::Microsoft::WSL::Containers
