/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    SessionSettings.h

Abstract:

    This file contains the definition of the WinRT wrapper for the WSLC SDK SessionSettings class.

--*/

#pragma once
#include "Microsoft.WSL.Containers.SessionSettings.g.h"
#include "VhdRequirements.h"
#include "Helpers.h"
#include "Defaults.h"

namespace winrt::Microsoft::WSL::Containers::implementation {
struct SessionSettings : SessionSettingsT<SessionSettings>
{
    SessionSettings() = default;

    SessionSettings(hstring const& name, hstring const& storagePath);
    hstring Name();
    hstring StoragePath();
    uint32_t CpuCount();
    void CpuCount(uint32_t value);
    uint32_t MemoryMb();
    void MemoryMb(uint32_t value);
    uint32_t TimeoutMS();
    void TimeoutMS(uint32_t value);
    winrt::Microsoft::WSL::Containers::VhdRequirements VhdRequirements();
    void VhdRequirements(winrt::Microsoft::WSL::Containers::VhdRequirements const& value);
    winrt::Microsoft::WSL::Containers::SessionFeatureFlags FeatureFlags();
    void FeatureFlags(winrt::Microsoft::WSL::Containers::SessionFeatureFlags const& value);

    WslcSessionSettings* ToStructPointer();

private:
    WslcSessionSettings m_sessionSettings{};
    std::wstring m_name;
    std::wstring m_storagePath;
    uint32_t m_cpuCount{s_DefaultCPUCount};
    uint32_t m_memoryMb{s_DefaultMemoryMB};
    uint32_t m_timeoutMS{s_DefaultBootTimeout};
    winrt::Microsoft::WSL::Containers::VhdRequirements m_vhdRequirements{nullptr};
    winrt::Microsoft::WSL::Containers::SessionFeatureFlags m_featureFlags{winrt::Microsoft::WSL::Containers::SessionFeatureFlags::None};
};
} // namespace winrt::Microsoft::WSL::Containers::implementation

namespace winrt::Microsoft::WSL::Containers::factory_implementation {
struct SessionSettings : SessionSettingsT<SessionSettings, implementation::SessionSettings>
{
};
} // namespace winrt::Microsoft::WSL::Containers::factory_implementation

DEFINE_TYPE_HELPERS(SessionSettings);