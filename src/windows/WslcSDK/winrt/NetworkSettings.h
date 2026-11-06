/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    NetworkSettings.h

Abstract:

    This file contains the definition of the WinRT wrapper for the WSLC SDK NetworkSettings class.

--*/

#pragma once
#include "Microsoft.WSL.Containers.NetworkSettings.g.h"
#include "Helpers.h"
#include <winrt/Windows.Networking.h>

namespace winrt::Microsoft::WSL::Containers::implementation {
struct NetworkSettings : NetworkSettingsT<NetworkSettings>
{
    NetworkSettings() = default;

    NetworkSettings(hstring const& name);
    hstring Name();
    void Name(hstring const& value);
    winrt::Microsoft::WSL::Containers::NetworkFlags Flags();
    void Flags(winrt::Microsoft::WSL::Containers::NetworkFlags const& value);
    winrt::Windows::Networking::HostName Subnet();
    void Subnet(winrt::Windows::Networking::HostName const& value);
    uint8_t SubnetPrefixLength();
    void SubnetPrefixLength(uint8_t value);
    winrt::Windows::Networking::HostName Gateway();
    void Gateway(winrt::Windows::Networking::HostName const& value);

    WslcNetworkSettings* ToStructPointer();

private:
    void EnsureNotApplied() const;

    std::string m_name;
    winrt::Microsoft::WSL::Containers::NetworkFlags m_flags{winrt::Microsoft::WSL::Containers::NetworkFlags::None};
    winrt::Windows::Networking::HostName m_subnet{nullptr};
    uint8_t m_subnetPrefixLength{};
    winrt::Windows::Networking::HostName m_gateway{nullptr};

    std::unique_ptr<WslcNetworkSettings> m_networkSettings;
};
} // namespace winrt::Microsoft::WSL::Containers::implementation

namespace winrt::Microsoft::WSL::Containers::factory_implementation {
struct NetworkSettings : NetworkSettingsT<NetworkSettings, implementation::NetworkSettings>
{
};
} // namespace winrt::Microsoft::WSL::Containers::factory_implementation

DEFINE_TYPE_HELPERS(NetworkSettings);
