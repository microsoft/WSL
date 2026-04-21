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

namespace winrt::Microsoft::WSL::Containers::implementation {
struct SessionSettings : SessionSettingsT<SessionSettings>
{
    SessionSettings() = default;

    SessionSettings(hstring const& name, hstring const& storagePath);
    hstring Name();
    void Name(hstring const& value);
    hstring StoragePath();
    void StoragePath(hstring const& value);
    winrt::Windows::Foundation::IReference<uint32_t> CpuCount();
    void CpuCount(winrt::Windows::Foundation::IReference<uint32_t> const& value);
    winrt::Windows::Foundation::IReference<uint32_t> MemoryMB();
    void MemoryMB(winrt::Windows::Foundation::IReference<uint32_t> const& value);
    winrt::Windows::Foundation::IReference<uint32_t> TimeoutMS();
    void TimeoutMS(winrt::Windows::Foundation::IReference<uint32_t> const& value);
    winrt::Microsoft::WSL::Containers::VhdRequirements VhdRequirements();
    void VhdRequirements(winrt::Microsoft::WSL::Containers::VhdRequirements const& value);
    winrt::Microsoft::WSL::Containers::SessionFeatureFlags FeatureFlags();
    void FeatureFlags(winrt::Microsoft::WSL::Containers::SessionFeatureFlags const& value);
    winrt::Microsoft::WSL::Containers::SessionTerminationHandler TerminationHandler();
    void TerminationHandler(winrt::Microsoft::WSL::Containers::SessionTerminationHandler const& value);

    WslcSessionSettings* ToStructPointer();

private:
    std::wstring m_name;
    std::wstring m_storagePath;
    winrt::Windows::Foundation::IReference<uint32_t> m_cpuCount{ nullptr };
    winrt::Windows::Foundation::IReference<uint32_t> m_memoryMB{ nullptr };
    winrt::Windows::Foundation::IReference<uint32_t> m_timeoutMS{ nullptr };
    winrt::Microsoft::WSL::Containers::VhdRequirements m_vhdRequirements{ nullptr };
    winrt::Microsoft::WSL::Containers::SessionFeatureFlags m_featureFlags{ winrt::Microsoft::WSL::Containers::SessionFeatureFlags::None };
    winrt::Microsoft::WSL::Containers::SessionTerminationHandler m_terminationHandler{ nullptr };

    std::unique_ptr<WslcSessionSettings> m_sessionSettings;
};
} // namespace winrt::Microsoft::WSL::Containers::implementation

namespace winrt::Microsoft::WSL::Containers::factory_implementation {
struct SessionSettings : SessionSettingsT<SessionSettings, implementation::SessionSettings>
{
};
} // namespace winrt::Microsoft::WSL::Containers::factory_implementation

DEFINE_TYPE_HELPERS(SessionSettings);