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

namespace winrt::Microsoft::WSL::Containers::implementation {
SessionSettings::SessionSettings(hstring const& name, hstring const& storagePath) :
    m_name(name),
    m_storagePath(storagePath)
{
}
hstring SessionSettings::Name()
{
    return hstring(m_name);
}
void SessionSettings::Name(hstring const& value)
{
    if (m_sessionSettings)
    {
        throw hresult_illegal_state_change();
    }

    m_name = value;
}
hstring SessionSettings::StoragePath()
{
    return hstring(m_storagePath);
}
void SessionSettings::StoragePath(hstring const& value)
{
    if (m_sessionSettings)
    {
        throw hresult_illegal_state_change();
    }

    m_storagePath = value;
}
winrt::Windows::Foundation::IReference<uint32_t> SessionSettings::CpuCount()
{
    return m_cpuCount;
}
void SessionSettings::CpuCount(winrt::Windows::Foundation::IReference<uint32_t> const& value)
{
    if (m_sessionSettings)
    {
        throw hresult_illegal_state_change();
    }

    if (value && value.Value() == 0)
    {
        throw hresult_invalid_argument();
    }

    m_cpuCount = value;
}
winrt::Windows::Foundation::IReference<uint32_t> SessionSettings::MemoryMB()
{
    return m_memoryMB;
}
void SessionSettings::MemoryMB(winrt::Windows::Foundation::IReference<uint32_t> const& value)
{
    if (m_sessionSettings)
    {
        throw hresult_illegal_state_change();
    }

    if (value && value.Value() == 0)
    {
        throw hresult_invalid_argument();
    }

    m_memoryMB = value;
}
winrt::Windows::Foundation::IReference<uint32_t> SessionSettings::TimeoutMS()
{
    return m_timeoutMS;
}
void SessionSettings::TimeoutMS(winrt::Windows::Foundation::IReference<uint32_t> const& value)
{
    if (m_sessionSettings)
    {
        throw hresult_illegal_state_change();
    }

    if (value && value.Value() == 0)
    {
        throw hresult_invalid_argument();
    }

    m_timeoutMS = value;
}
winrt::Microsoft::WSL::Containers::VhdRequirements SessionSettings::VhdRequirements()
{
    return m_vhdRequirements;
}
void SessionSettings::VhdRequirements(winrt::Microsoft::WSL::Containers::VhdRequirements const& value)
{
    if (m_sessionSettings)
    {
        throw hresult_illegal_state_change();
    }

    m_vhdRequirements = value;
}
winrt::Microsoft::WSL::Containers::SessionFeatureFlags SessionSettings::FeatureFlags()
{
    return m_featureFlags;
}
void SessionSettings::FeatureFlags(winrt::Microsoft::WSL::Containers::SessionFeatureFlags const& value)
{
    if (m_sessionSettings)
    {
        throw hresult_illegal_state_change();
    }

    m_featureFlags = value;
}

winrt::Microsoft::WSL::Containers::SessionTerminationHandler SessionSettings::TerminationHandler()
{
    return m_terminationHandler;
}
void SessionSettings::TerminationHandler(winrt::Microsoft::WSL::Containers::SessionTerminationHandler const& value)
{
    if (m_sessionSettings)
    {
        throw hresult_illegal_state_change();
    }

    m_terminationHandler = value;
}

WslcSessionSettings* SessionSettings::ToStructPointer()
{
    if (m_sessionSettings)
    {
        return m_sessionSettings.get();
    }

    m_sessionSettings = std::make_unique<WslcSessionSettings>();
    winrt::check_hresult(WslcInitSessionSettings(m_name.c_str(), m_storagePath.c_str(), m_sessionSettings.get()));

    if (m_cpuCount)
    {
        winrt::check_hresult(WslcSetSessionSettingsCpuCount(m_sessionSettings.get(), m_cpuCount.Value()));
    }

    if (m_memoryMB)
    {
        winrt::check_hresult(WslcSetSessionSettingsMemory(m_sessionSettings.get(), m_memoryMB.Value()));
    }

    if (m_timeoutMS)
    {
        winrt::check_hresult(WslcSetSessionSettingsTimeout(m_sessionSettings.get(), m_timeoutMS.Value()));
    }

    if (m_vhdRequirements)
    {
        winrt::check_hresult(WslcSetSessionSettingsVhd(m_sessionSettings.get(), GetStructPointer(m_vhdRequirements)));
    }

    winrt::check_hresult(WslcSetSessionSettingsFeatureFlags(m_sessionSettings.get(), static_cast<WslcSessionFeatureFlags>(m_featureFlags)));

    if (m_terminationHandler)
    {
        throw hresult_not_implemented();
    }

    return m_sessionSettings.get();
}
} // namespace winrt::Microsoft::WSL::Containers::implementation