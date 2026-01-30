// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "hns_schema.h"
#include "WslCoreNetworkEndpointSettings.h"
#include "WslCoreHostDnsInfo.h"

using namespace wsl::shared;

std::shared_ptr<wsl::core::networking::NetworkSettings> wsl::core::networking::GetEndpointSettings(const hns::HNSEndpoint& properties)
{
    EndpointIpAddress address{};
    address.Address = windows::common::string::StringToSockAddrInet(properties.IPAddress);
    address.AddressString = properties.IPAddress;
    address.PrefixLength = properties.PrefixLength;

    EndpointRoute route{};
    route.DestinationPrefix.PrefixLength = 0;
    IN4ADDR_SETANY(&route.DestinationPrefix.Prefix.Ipv4);
    route.DestinationPrefixString = LX_INIT_UNSPECIFIED_ADDRESS;
    route.NextHop = windows::common::string::StringToSockAddrInet(properties.GatewayAddress);
    route.NextHopString = properties.GatewayAddress;

    return std::make_shared<wsl::core::networking::NetworkSettings>(
        properties.InterfaceConstraint.InterfaceGuid,
        address,
        route,
        properties.MacAddress,
        properties.InterfaceConstraint.InterfaceIndex,
        properties.InterfaceConstraint.InterfaceMediaType);
}

std::shared_ptr<wsl::core::networking::NetworkSettings> wsl::core::networking::GetHostEndpointSettings()
{
    auto addresses = AdapterAddresses::GetCurrent();
    auto bestIndex = GetBestInterface();
    auto bestInterfacePtr =
        std::find_if(addresses.cbegin(), addresses.cend(), [&](const auto& address) { return address->IfIndex == bestIndex; });
    if (bestInterfacePtr == addresses.end())
    {
        return std::make_shared<NetworkSettings>();
    }

    const auto& bestInterface = *bestInterfacePtr;

    std::wstring macAddress = wsl::shared::string::FormatMacAddress(
        wsl::shared::string::MacAddress{
            bestInterface->PhysicalAddress[0],
            bestInterface->PhysicalAddress[1],
            bestInterface->PhysicalAddress[2],
            bestInterface->PhysicalAddress[3],
            bestInterface->PhysicalAddress[4],
            bestInterface->PhysicalAddress[5]},
        L'-');

    EndpointIpAddress address{};
    auto firstIpv4Address = bestInterface->FirstUnicastAddress;
    while (firstIpv4Address && firstIpv4Address->Address.lpSockaddr->sa_family != AF_INET)
    {
        firstIpv4Address = firstIpv4Address->Next;
    }
    if (firstIpv4Address)
    {
        address.Address = *reinterpret_cast<SOCKADDR_INET*>(firstIpv4Address->Address.lpSockaddr);
        address.AddressString = windows::common::string::SockAddrInetToWstring(address.Address);
        address.PrefixLength = firstIpv4Address->OnLinkPrefixLength;
    }

    EndpointRoute route{};
    PIP_ADAPTER_GATEWAY_ADDRESS nextGatewayAddress = bestInterface->FirstGatewayAddress;
    while (nextGatewayAddress && nextGatewayAddress->Address.lpSockaddr->sa_family != AF_INET)
    {
        nextGatewayAddress = nextGatewayAddress->Next;
    }
    if (nextGatewayAddress)
    {
        route.DestinationPrefix.PrefixLength = 0;
        IN4ADDR_SETANY(&route.DestinationPrefix.Prefix.Ipv4);
        route.DestinationPrefixString = LX_INIT_UNSPECIFIED_ADDRESS;
        route.NextHop = *reinterpret_cast<SOCKADDR_INET*>(nextGatewayAddress->Address.lpSockaddr);
        route.NextHopString = windows::common::string::SockAddrInetToWstring(route.NextHop);
    }
    else if (address.Address.si_family == AF_INET)
    {
        IN_ADDR default_route{};
        default_route.s_addr = htonl((ntohl(address.Address.Ipv4.sin_addr.s_addr) & ~((1 << (32 - address.PrefixLength)) - 1)) | 1);
        route.DestinationPrefix.PrefixLength = 0;
        IN4ADDR_SETANY(&route.DestinationPrefix.Prefix.Ipv4);
        route.DestinationPrefixString = LX_INIT_UNSPECIFIED_ADDRESS;
        IN4ADDR_SETSOCKADDR(&route.NextHop.Ipv4, &default_route, 0);
        route.NextHopString = windows::common::string::SockAddrInetToWstring(route.NextHop);
    }

    return std::make_shared<NetworkSettings>(
        bestInterface->NetworkGuid, address, route, macAddress, bestInterface->IfIndex, bestInterface->IfType);
}
