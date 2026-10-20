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
#include "Session.h"

using namespace winrt::Windows::Foundation;

namespace winrt::Microsoft::WSL::Containers::implementation {
SessionSettings::SessionSettings(hstring const& name, hstring const& storagePath) : m_name(name), m_storagePath(storagePath)
{
    if (name.empty())
    {
        throw winrt::hresult_invalid_argument(L"Session name cannot be empty");
    }

    if (storagePath.empty())
    {
        throw winrt::hresult_invalid_argument(L"Storage path cannot be empty");
    }
}

hstring SessionSettings::Name()
{
    return hstring(m_name);
}

void SessionSettings::Name(hstring const& value)
{
    if (m_sessionSettings)
    {
        throw hresult_illegal_state_change(L"Cannot change session name after session has been initialized");
    }

    if (value.empty())
    {
        throw winrt::hresult_invalid_argument(L"Session name cannot be empty");
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
        throw hresult_illegal_state_change(L"Cannot change storage path after session has been initialized");
    }

    if (value.empty())
    {
        throw winrt::hresult_invalid_argument(L"Storage path cannot be empty");
    }

    m_storagePath = value;
}

IReference<uint32_t> SessionSettings::CpuCount()
{
    return m_cpuCount;
}

void SessionSettings::CpuCount(IReference<uint32_t> const& value)
{
    if (m_sessionSettings)
    {
        throw hresult_illegal_state_change(L"Cannot change CPU count after session has been initialized");
    }

    if (value && value.Value() == 0)
    {
        throw hresult_invalid_argument(L"CPU count cannot be 0");
    }

    m_cpuCount = value;
}

IReference<uint32_t> SessionSettings::MemorySizeInMB()
{
    return m_memorySizeInMB;
}

void SessionSettings::MemorySizeInMB(IReference<uint32_t> const& value)
{
    if (m_sessionSettings)
    {
        throw hresult_illegal_state_change(L"Cannot change memory size after session has been initialized");
    }

    if (value && value.Value() == 0)
    {
        throw hresult_invalid_argument(L"Memory size cannot be 0");
    }

    m_memorySizeInMB = value;
}

IReference<TimeSpan> SessionSettings::Timeout()
{
    return m_timeout;
}

void SessionSettings::Timeout(IReference<TimeSpan> const& value)
{
    if (m_sessionSettings)
    {
        throw hresult_illegal_state_change(L"Cannot change timeout after session has been initialized");
    }

    if (value)
    {
        if (value.Value() == TimeSpan::zero())
        {
            throw hresult_invalid_argument(L"Timeout cannot be 0");
        }

        // The C API takes the timeout in milliseconds as a uint32_t, so we need to validate that the value is within range.
        auto timeoutMS = std::chrono::duration_cast<std::chrono::milliseconds>(value.Value()).count();
        if (timeoutMS > std::numeric_limits<uint32_t>::max())
        {
            throw hresult_invalid_argument(L"Timeout exceeds the allowed limit");
        }

        if (timeoutMS < 0)
        {
            throw hresult_invalid_argument(L"Timeout cannot be negative");
        }
    }

    m_timeout = value;
}

winrt::Microsoft::WSL::Containers::VhdOptions SessionSettings::VhdRequirements()
{
    return m_vhdRequirements;
}

void SessionSettings::VhdRequirements(winrt::Microsoft::WSL::Containers::VhdOptions const& value)
{
    if (m_sessionSettings)
    {
        throw hresult_illegal_state_change(L"Cannot change VHD requirements after session has been initialized");
    }

    if (!value)
    {
        throw winrt::hresult_error(E_POINTER, L"VHD requirements cannot be null");
    }

    m_vhdRequirements = value;
}

bool SessionSettings::EnableGpu()
{
    return WI_IsFlagSet(m_featureFlags, WSLC_SESSION_FEATURE_FLAG_ENABLE_GPU);
}

void SessionSettings::EnableGpu(bool value)
{
    if (m_sessionSettings)
    {
        throw hresult_illegal_state_change(L"Cannot change GPU setting after session has been initialized");
    }

    WI_UpdateFlag(m_featureFlags, WSLC_SESSION_FEATURE_FLAG_ENABLE_GPU, value);
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

    if (m_memorySizeInMB)
    {
        winrt::check_hresult(WslcSetSessionSettingsMemory(m_sessionSettings.get(), m_memorySizeInMB.Value()));
    }

    if (m_timeout)
    {
        auto timeoutMS = std::chrono::duration_cast<std::chrono::milliseconds>(m_timeout.Value()).count();
        winrt::check_hresult(WslcSetSessionSettingsTimeout(m_sessionSettings.get(), static_cast<uint32_t>(timeoutMS)));
    }

    if (m_vhdRequirements)
    {
        winrt::check_hresult(WslcSetSessionSettingsVhd(m_sessionSettings.get(), GetStructPointer(m_vhdRequirements)));
    }

    winrt::check_hresult(WslcSetSessionSettingsFeatureFlags(m_sessionSettings.get(), m_featureFlags));

    return m_sessionSettings.get();
}

} // namespace winrt::Microsoft::WSL::Containers::implementation
