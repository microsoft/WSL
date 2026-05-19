/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    SessionSettings.h

Abstract:

    This file contains the definition of the WinRT wrapper for the WSLC SDK SessionSettings class.

--*/

#pragma once
#include "Microsoft.WSL.Containers.SessionSettings.g.h"

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
    winrt::Microsoft::WSL::Containers::VhdOptions VhdRequirements();
    void VhdRequirements(winrt::Microsoft::WSL::Containers::VhdOptions const& value);
    winrt::Microsoft::WSL::Containers::SessionFeatureFlags FeatureFlags();
    void FeatureFlags(winrt::Microsoft::WSL::Containers::SessionFeatureFlags const& value);
};
} // namespace winrt::Microsoft::WSL::Containers::implementation
namespace winrt::Microsoft::WSL::Containers::factory_implementation {
struct SessionSettings : SessionSettingsT<SessionSettings, implementation::SessionSettings>
{
};
} // namespace winrt::Microsoft::WSL::Containers::factory_implementation
