/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ContainerNetwork.h

Abstract:

    This file contains the definition of the WinRT wrapper for the WSLC SDK ContainerNetwork class.

--*/

#pragma once
#include "Microsoft.WSL.Containers.ContainerNetwork.g.h"
#include "Helpers.h"

namespace winrt::Microsoft::WSL::Containers::implementation {
struct ContainerNetwork : ContainerNetworkT<ContainerNetwork>
{
    ContainerNetwork() = default;

    ContainerNetwork(hstring const& name);
    hstring Name();
    void Name(hstring const& value);
    winrt::Windows::Foundation::Collections::IVector<hstring> Aliases();
    void Aliases(winrt::Windows::Foundation::Collections::IVector<hstring> const& value);
    winrt::Windows::Networking::HostName IPv4Address();
    void IPv4Address(winrt::Windows::Networking::HostName const& value);
    winrt::Windows::Networking::HostName IPv6Address();
    void IPv6Address(winrt::Windows::Networking::HostName const& value);

    WslcContainerNetwork ToStruct();

private:
    std::string m_name;
    winrt::Windows::Foundation::Collections::IVector<hstring> m_aliases{winrt::single_threaded_vector<hstring>()};
    winrt::Windows::Networking::HostName m_ipv4Address{nullptr};
    winrt::Windows::Networking::HostName m_ipv6Address{nullptr};

    std::unique_ptr<WslcContainerNetwork> m_containerNetwork;
    std::vector<std::string> m_aliasStrings;
    std::vector<PCSTR> m_aliasPointers;
    IN_ADDR m_ipv4AddressStruct{};
    IN6_ADDR m_ipv6AddressStruct{};
};
} // namespace winrt::Microsoft::WSL::Containers::implementation

namespace winrt::Microsoft::WSL::Containers::factory_implementation {
struct ContainerNetwork : ContainerNetworkT<ContainerNetwork, implementation::ContainerNetwork>
{
};
} // namespace winrt::Microsoft::WSL::Containers::factory_implementation

DEFINE_TYPE_HELPERS(ContainerNetwork);
