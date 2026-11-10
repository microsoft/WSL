/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    NetworkInfo.h

Abstract:

    This file contains the definition of the WinRT wrapper for the WSLC SDK NetworkInfo class.

--*/

#pragma once
#include "Microsoft.WSL.Containers.NetworkInfo.g.h"
#include "Helpers.h"
#include <winrt/Windows.Networking.h>

namespace winrt::Microsoft::WSL::Containers::implementation {
struct NetworkInfo : NetworkInfoT<NetworkInfo>
{
    NetworkInfo() = default;
    NetworkInfo(WslcNetworkInfo const& info);

    hstring Id();
    hstring Name();
    hstring Driver();
    winrt::Windows::Foundation::DateTime CreatedTimestamp();
    winrt::Microsoft::WSL::Containers::NetworkFlags Flags();
    winrt::Windows::Networking::HostName IPv4Subnet();
    uint8_t IPv4SubnetPrefixLength();
    winrt::Windows::Networking::HostName IPv4Gateway();
    winrt::Windows::Networking::HostName IPv6Subnet();
    uint8_t IPv6SubnetPrefixLength();
    winrt::Windows::Networking::HostName IPv6Gateway();

private:
    hstring m_id{};
    hstring m_name{};
    hstring m_driver{};
    winrt::Windows::Foundation::DateTime m_createdTimestamp{};
    winrt::Microsoft::WSL::Containers::NetworkFlags m_flags{};
    winrt::Windows::Networking::HostName m_ipv4Subnet{nullptr};
    uint8_t m_ipv4SubnetPrefixLength{};
    winrt::Windows::Networking::HostName m_ipv4Gateway{nullptr};
    winrt::Windows::Networking::HostName m_ipv6Subnet{nullptr};
    uint8_t m_ipv6SubnetPrefixLength{};
    winrt::Windows::Networking::HostName m_ipv6Gateway{nullptr};
};
} // namespace winrt::Microsoft::WSL::Containers::implementation

DEFINE_TYPE_HELPERS(NetworkInfo);
