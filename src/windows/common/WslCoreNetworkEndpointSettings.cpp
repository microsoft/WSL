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
        EndpointIpAddress{},
        route,
        EndpointRoute{},
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
        address.Address.Ipv4 = *reinterpret_cast<SOCKADDR_IN*>(firstIpv4Address->Address.lpSockaddr);
        address.AddressString = windows::common::string::SockAddrInetToWstring(address.Address);
        address.PrefixLength = firstIpv4Address->OnLinkPrefixLength;
    }

    // Find the first global-scope (non-link-local) IPv6 unicast address.
    EndpointIpAddress ipv6Address{};
    auto nextUnicastAddress = bestInterface->FirstUnicastAddress;
    while (nextUnicastAddress)
    {
        if (nextUnicastAddress->Address.lpSockaddr->sa_family == AF_INET6)
        {
            const auto& sin6 = *reinterpret_cast<SOCKADDR_IN6*>(nextUnicastAddress->Address.lpSockaddr);
            if (!IN6_IS_ADDR_LINKLOCAL(&sin6.sin6_addr) && !IN6_IS_ADDR_LOOPBACK(&sin6.sin6_addr))
            {
                ipv6Address.Address.Ipv6 = sin6;
                ipv6Address.AddressString = windows::common::string::SockAddrInetToWstring(ipv6Address.Address);
                ipv6Address.PrefixLength = nextUnicastAddress->OnLinkPrefixLength;
                break;
            }
        }
        nextUnicastAddress = nextUnicastAddress->Next;
    }

    // Helper to find the first gateway address of a given family.
    auto findGatewayAddress = [](PIP_ADAPTER_GATEWAY_ADDRESS list, ADDRESS_FAMILY family) -> PIP_ADAPTER_GATEWAY_ADDRESS {
        while (list && list->Address.lpSockaddr->sa_family != family)
        {
            list = list->Next;
        }
        return list;
    };

    // Build IPv4 default route.
    EndpointRoute route{};
    const auto v4Gateway = findGatewayAddress(bestInterface->FirstGatewayAddress, AF_INET);
    if (v4Gateway)
    {
        SOCKADDR_INET v4NextHop{};
        v4NextHop.Ipv4 = *reinterpret_cast<SOCKADDR_IN*>(v4Gateway->Address.lpSockaddr);
        route = EndpointRoute::DefaultRoute(AF_INET, v4NextHop);
    }
    else if (address.Address.si_family == AF_INET)
    {
        // Synthesize a gateway from the first host address in the subnet.
        SOCKADDR_INET gatewayAddr{};
        gatewayAddr.si_family = AF_INET;
        const uint32_t hostAddr = ntohl(address.Address.Ipv4.sin_addr.s_addr);
        const uint32_t mask = (address.PrefixLength == 0) ? 0u : ~((1u << (32u - address.PrefixLength)) - 1u);
        gatewayAddr.Ipv4.sin_addr.s_addr = htonl((hostAddr & mask) | 1u);
        route = EndpointRoute::DefaultRoute(AF_INET, gatewayAddr);
    }

    // Build IPv6 default route.
    EndpointRoute v6Route{};
    const auto v6Gateway = findGatewayAddress(bestInterface->FirstGatewayAddress, AF_INET6);
    if (v6Gateway)
    {
        SOCKADDR_INET v6NextHop{};
        v6NextHop.Ipv6 = *reinterpret_cast<SOCKADDR_IN6*>(v6Gateway->Address.lpSockaddr);
        v6Route = EndpointRoute::DefaultRoute(AF_INET6, v6NextHop);
    }

    return std::make_shared<NetworkSettings>(
        bestInterface->NetworkGuid,
        std::move(address),
        std::move(ipv6Address),
        std::move(route),
        std::move(v6Route),
        std::move(macAddress),
        bestInterface->IfIndex,
        bestInterface->IfType);
}

std::wstring wsl::core::networking::NetworkSettings::GetBestGatewayMacAddress(ADDRESS_FAMILY addressFamily) const
{
    auto gatewayAddress = GetBestGatewayAddress(addressFamily);
    if (gatewayAddress.si_family != addressFamily)
    {
        return {};
    }

    MIB_IPNET_ROW2 ipNetRow{};
    ipNetRow.Address = gatewayAddress;
    ipNetRow.InterfaceIndex = InterfaceIndex;

    const auto result = ResolveIpNetEntry2(&ipNetRow, nullptr);
    if (result != NO_ERROR)
    {
        LOG_HR_MSG(HRESULT_FROM_WIN32(result), "Failed to resolve gateway MAC address");
        return {};
    }

    if (ipNetRow.PhysicalAddressLength != 6)
    {
        return {};
    }

    return wsl::shared::string::FormatMacAddress(
        wsl::shared::string::MacAddress{
            ipNetRow.PhysicalAddress[0],
            ipNetRow.PhysicalAddress[1],
            ipNetRow.PhysicalAddress[2],
            ipNetRow.PhysicalAddress[3],
            ipNetRow.PhysicalAddress[4],
            ipNetRow.PhysicalAddress[5]},
        L'-');
}
