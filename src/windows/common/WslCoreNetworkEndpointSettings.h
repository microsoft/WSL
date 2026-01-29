// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once
#include <algorithm>
#include <set>
#include <string>

#include <windows.h>
#include <mstcpip.h>
#include <ws2ipdef.h>
#include <netioapi.h>

#include "hcs.hpp"
#include "lxinitshared.h"
#include "Stringify.h"
#include "stringshared.h"
#include "WslCoreNetworkingSupport.h"
#include "hns_schema.h"

namespace wsl::core::networking {

constexpr auto AddEndpointRetryPeriod = std::chrono::milliseconds(100);
constexpr auto AddEndpointRetryTimeout = std::chrono::seconds(3);
constexpr auto AddEndpointRetryPredicate = [] {
    // Don't retry if ModifyComputeSystem fails with:
    //     HCN_E_ENDPOINT_NOT_FOUND - indicates that the underlying network object was deleted.
    //     HCN_E_ENDPOINT_ALREADY_ATTACHED - occurs when HNS was restarted before the endpoints were removed.
    //     VM_E_INVALID_STATE - occurs when the VM has been terminated.
    const auto result = wil::ResultFromCaughtException();
    return result != HCN_E_ENDPOINT_NOT_FOUND && result != HCN_E_ENDPOINT_ALREADY_ATTACHED && result != VM_E_INVALID_STATE;
};

struct EndpointIpAddress
{
    SOCKADDR_INET Address{};
    std::wstring AddressString{};
    unsigned char PrefixLength = 0;
    unsigned int PrefixOrigin = 0;
    unsigned int SuffixOrigin = 0;

    // The following field can be changed from a const iterator in SyncIpStateWithLinux - that's why it's marked mutable.
    mutable unsigned int PreferredLifetime = 0;

    EndpointIpAddress() = default;
    ~EndpointIpAddress() noexcept = default;

    EndpointIpAddress(EndpointIpAddress&&) = default;
    EndpointIpAddress& operator=(EndpointIpAddress&&) = default;
    EndpointIpAddress(const EndpointIpAddress&) = default;
    EndpointIpAddress& operator=(const EndpointIpAddress&) = default;

    explicit EndpointIpAddress(const MIB_UNICASTIPADDRESS_ROW& AddressRow) :
        Address(AddressRow.Address),
        AddressString(windows::common::string::SockAddrInetToWstring(AddressRow.Address)),
        PrefixLength(AddressRow.OnLinkPrefixLength),
        PrefixOrigin(AddressRow.PrefixOrigin),
        SuffixOrigin(AddressRow.SuffixOrigin),
        // We treat the preferred lifetime field as effective DAD state - 0 is not preferred, anything else is preferred.
        // We do this for convenience, as we can't directly set the DAD state of an address into the guest, but we
        // we can set an address's preferred lifetime (in Linux, at least).
        PreferredLifetime(AddressRow.DadState == IpDadStatePreferred ? 0xFFFFFFFF : 0)
    {
    }

    // operator== is deliberately not comparing PreferredLifetime (DAD state) for equality - only the address portion
    bool operator==(const EndpointIpAddress& rhs) const noexcept
    {
        return Address == rhs.Address && PrefixLength == rhs.PrefixLength;
    }

    bool operator<(const EndpointIpAddress& rhs) const noexcept
    {
        if (Address == rhs.Address)
        {
            return PrefixLength < rhs.PrefixLength;
        }
        return Address < rhs.Address;
    }

    void Clear() noexcept
    {
        Address = {};
        AddressString.clear();
        PrefixLength = 0;
        PrefixOrigin = 0;
        SuffixOrigin = 0;
    }

    std::wstring GetPrefix() const
    {
        SOCKADDR_INET address{Address};
        unsigned char* addressPointer{nullptr};

        if (Address.si_family == AF_INET)
        {
            addressPointer = reinterpret_cast<unsigned char*>(&address.Ipv4.sin_addr);
        }
        else if (Address.si_family == AF_INET6)
        {
            addressPointer = address.Ipv6.sin6_addr.u.Byte;
        }
        else
        {
            return L"";
        }

        constexpr int c_numBitsPerByte = 8;
        for (int i = 0, currPrefixLength = PrefixLength; i < INET_ADDR_LENGTH(Address.si_family); i++, currPrefixLength -= c_numBitsPerByte)
        {
            if (currPrefixLength < c_numBitsPerByte)
            {
                const int bitShiftAmt = c_numBitsPerByte - std::max(currPrefixLength, 0);
                addressPointer[i] &= (0xFF >> bitShiftAmt) << bitShiftAmt;
            }
        }

        const auto addressString = windows::common::string::SockAddrInetToWstring(address);
        WI_ASSERT(!addressString.empty());
        if (addressString.empty())
        {
            // just return an empty string if we have a bad address
            return addressString;
        }

        return std::format(L"{}/{}", addressString, PrefixLength);
    }

    std::wstring GetIpv4BroadcastMask() const
    {
        // start with all bits set, then shift off the prefix
        ULONG prefixMask{0xffffffff};
        prefixMask <<= PrefixLength;
        prefixMask >>= PrefixLength;

        SOCKADDR_INET address{Address};
        // flip to host-order, then apply the mask
        ULONG hostOrder = ntohl(address.Ipv4.sin_addr.S_un.S_addr);
        hostOrder |= prefixMask;
        address.Ipv4.sin_addr.S_un.S_addr = htonl(hostOrder);

        return windows::common::string::SockAddrInetToWstring(address);
    }

    bool IsPreferred() const noexcept
    {
        return PreferredLifetime > 0;
    }

    bool IsLinkLocal() const
    {
        return (Address.si_family == AF_INET && IN4_IS_ADDR_LINKLOCAL(&Address.Ipv4.sin_addr)) ||
               (Address.si_family == AF_INET6 && IN6_IS_ADDR_LINKLOCAL(&Address.Ipv6.sin6_addr));
    }
};

struct EndpointRoute
{
    ADDRESS_FAMILY Family = AF_INET;
    IP_ADDRESS_PREFIX DestinationPrefix{};
    std::wstring DestinationPrefixString{};
    SOCKADDR_INET NextHop{};
    std::wstring NextHopString{};
    unsigned char SitePrefixLength = 0;
    unsigned int Metric = 0;
    bool IsAutoGeneratedPrefixRoute = false;

    EndpointRoute() = default;
    ~EndpointRoute() noexcept = default;

    EndpointRoute(EndpointRoute&&) = default;
    EndpointRoute& operator=(EndpointRoute&&) = default;
    EndpointRoute(const EndpointRoute&) = default;
    EndpointRoute& operator=(const EndpointRoute&) = default;

    EndpointRoute(const MIB_IPFORWARD_ROW2& RouteRow) :
        Family(RouteRow.NextHop.si_family),
        DestinationPrefix(RouteRow.DestinationPrefix),
        DestinationPrefixString(windows::common::string::SockAddrInetToWstring(RouteRow.DestinationPrefix.Prefix)),
        NextHop(RouteRow.NextHop),
        NextHopString(windows::common::string::SockAddrInetToWstring(RouteRow.NextHop)),
        SitePrefixLength(RouteRow.SitePrefixLength),
        Metric(RouteRow.Metric)
    {
    }

    unsigned char GetMaxPrefixLength() const
    {
        return (Family == AF_INET) ? 32 : 128;
    }

    std::wstring GetFullDestinationPrefix() const
    {
        return std::format(L"{}/{}", DestinationPrefixString, static_cast<unsigned int>(DestinationPrefix.PrefixLength));
    }

    bool IsNextHopOnlink() const noexcept
    {
        return (Family == AF_INET && NextHopString == LX_INIT_UNSPECIFIED_ADDRESS) ||
               (Family == AF_INET6 && NextHopString == LX_INIT_UNSPECIFIED_V6_ADDRESS);
    }

    bool IsDefault() const noexcept
    {
        return (Family == AF_INET && DestinationPrefixString == LX_INIT_UNSPECIFIED_ADDRESS) ||
               (Family == AF_INET6 && DestinationPrefixString == LX_INIT_UNSPECIFIED_V6_ADDRESS);
    }

    bool IsUnicastAddressRoute() const noexcept
    {
        return (Family == AF_INET && DestinationPrefix.PrefixLength == 32) || (Family == AF_INET6 && DestinationPrefix.PrefixLength == 128);
    }

    std::wstring ToString() const
    {
        return std::format(L"{}=>{} [metric {}]", GetFullDestinationPrefix(), NextHopString, Metric);
    }

    bool operator==(const EndpointRoute& rhs) const noexcept
    {
        return Family == rhs.Family && DestinationPrefix.PrefixLength == rhs.DestinationPrefix.PrefixLength &&
               DestinationPrefix.Prefix == rhs.DestinationPrefix.Prefix && NextHop == rhs.NextHop &&
               SitePrefixLength == rhs.SitePrefixLength && Metric == rhs.Metric;
    }

    bool operator!=(const EndpointRoute& other) const
    {
        return !(*this == other);
    }

    // sort by family, then by next-hop (on-link routes first), then by prefix, then by metric
    bool operator<(const EndpointRoute& rhs) const noexcept
    {
        if (Family == rhs.Family)
        {
            if (NextHop == rhs.NextHop)
            {
                if (DestinationPrefix.Prefix == rhs.DestinationPrefix.Prefix)
                {
                    if (DestinationPrefix.PrefixLength == rhs.DestinationPrefix.PrefixLength)
                    {
                        if (Metric == rhs.Metric)
                        {
                            return SitePrefixLength < rhs.SitePrefixLength;
                        }
                        return Metric < rhs.Metric;
                    }
                    return DestinationPrefix.PrefixLength < rhs.DestinationPrefix.PrefixLength;
                }
                return DestinationPrefix.Prefix < rhs.DestinationPrefix.Prefix;
            }
            return NextHop < rhs.NextHop;
        }
        return Family < rhs.Family;
    }
};

struct NetworkSettings
{
    NetworkSettings() = default;

    NetworkSettings(const GUID& interfaceGuid, EndpointIpAddress preferredIpAddress, EndpointRoute gateway, std::wstring macAddress, uint32_t interfaceIndex, uint32_t mediaType) :
        InterfaceGuid(interfaceGuid),
        PreferredIpAddress(std::move(preferredIpAddress)),
        MacAddress(std::move(macAddress)),
        InterfaceIndex(interfaceIndex),
        InterfaceType(mediaType)
    {
        Routes.emplace(std::move(gateway));
    }

    GUID InterfaceGuid{};
    EndpointIpAddress PreferredIpAddress{};
    std::set<EndpointIpAddress> IpAddresses{}; // Does not include PreferredIpAddress.
    std::set<EndpointRoute> Routes{};
    std::wstring MacAddress;
    IF_INDEX InterfaceIndex = 0;
    IFTYPE InterfaceType = 0;
    ULONG IPv4InterfaceMtu = 0;
    ULONG IPv6InterfaceMtu = 0;
    // some interfaces will only have an IPv4 or IPv6 interface
    std::optional<ULONG> IPv4InterfaceMetric = 0;
    std::optional<ULONG> IPv6InterfaceMetric = 0;
    bool IsHidden = false;
    bool IsConnected = false;
    bool IsMetered = false;
    bool DisableIpv4DefaultRoutes = false;
    bool DisableIpv6DefaultRoutes = false;
    bool PendingUpdateToReconnectForMetered = false;
    bool PendingIPInterfaceUpdate = false;

    auto operator<=>(const NetworkSettings&) const = default;

    std::wstring GetBestGatewayAddressString() const
    {
        // Best is currently defined as simply the first IPv4 gateway.
        for (const auto& route : Routes)
        {
            if (route.Family == AF_INET && route.DestinationPrefix.PrefixLength == 0 && route.DestinationPrefixString == LX_INIT_UNSPECIFIED_ADDRESS)
            {
                return route.NextHopString;
            }
        }

        return {};
    }

    SOCKADDR_INET GetBestGatewayAddress() const
    {
        // Best is currently defined as simply the first IPv4 gateway.
        for (const auto& route : Routes)
        {
            if (route.Family == AF_INET && route.DestinationPrefix.PrefixLength == 0 && route.DestinationPrefixString == LX_INIT_UNSPECIFIED_ADDRESS)
            {
                return route.NextHop;
            }
        }

        return {};
    }

    std::wstring IpAddressesString() const
    {
        return std::accumulate(std::begin(IpAddresses), std::end(IpAddresses), std::wstring{}, [](const std::wstring& prev, const auto& addr) {
            return addr.AddressString + (prev.empty() ? L"" : L"," + prev);
        });
    }

    std::wstring RoutesString() const
    {
        return std::accumulate(std::begin(Routes), std::end(Routes), std::wstring{}, [](const std::wstring& prev, const EndpointRoute& route) {
            return route.ToString() + (prev.empty() ? L"" : L"," + prev);
        });
    }

    // will return ULONG_MAX if there's no configured MTU
    ULONG GetEffectiveMtu() const noexcept
    {
        return std::min(IPv4InterfaceMtu > 0 ? IPv4InterfaceMtu : ULONG_MAX, IPv6InterfaceMtu > 0 ? IPv6InterfaceMtu : ULONG_MAX);
    }

    // will return zero if there's no configured metric
    ULONG GetMinimumMetric() const noexcept
    {
        if (!IPv4InterfaceMetric.has_value() && !IPv6InterfaceMetric.has_value())
        {
            return 0;
        }
        if (!IPv4InterfaceMetric.has_value())
        {
            return IPv6InterfaceMetric.value();
        }
        if (!IPv6InterfaceMetric.has_value())
        {
            return IPv4InterfaceMetric.value();
        }
        return std::min(IPv4InterfaceMetric.value(), IPv6InterfaceMetric.value());
    }
};

std::shared_ptr<NetworkSettings> GetEndpointSettings(const wsl::shared::hns::HNSEndpoint& properties);
std::shared_ptr<NetworkSettings> GetHostEndpointSettings();

#define TRACE_NETWORKSETTINGS_OBJECT(settings) \
    TraceLoggingValue((settings)->InterfaceGuid, "interfaceGuid"), TraceLoggingValue((settings)->InterfaceIndex, "interfaceIndex"), \
        TraceLoggingValue((settings)->InterfaceType, "interfaceType"), \
        TraceLoggingValue((settings)->IsConnected, "isConnected"), TraceLoggingValue((settings)->IsMetered, "isMetered"), \
        TraceLoggingValue((settings)->GetBestGatewayAddressString().c_str(), "bestGatewayAddress"), \
        TraceLoggingValue((settings)->PreferredIpAddress.AddressString.c_str(), "preferredIpAddress"), \
        TraceLoggingValue((settings)->PreferredIpAddress.PrefixLength, "preferredIpAddressPrefixLength"), \
        TraceLoggingValue((settings)->IpAddressesString().c_str(), "ipAddresses"), \
        TraceLoggingValue((settings)->RoutesString().c_str(), "routes"), \
        TraceLoggingValue((settings)->MacAddress.c_str(), "macAddress"), \
        TraceLoggingValue((settings)->IPv4InterfaceMtu, "IPv4InterfaceMtu"), \
        TraceLoggingValue((settings)->IPv6InterfaceMtu, "IPv6InterfaceMtu"), \
        TraceLoggingValue((settings)->IPv4InterfaceMetric.value_or(0xffffffff), "IPv4InterfaceMetric"), \
        TraceLoggingValue((settings)->IPv6InterfaceMetric.value_or(0xffffffff), "IPv6InterfaceMetric"), \
        TraceLoggingValue((settings)->PendingIPInterfaceUpdate, "PendingIPInterfaceUpdate"), \
        TraceLoggingValue((settings)->PendingUpdateToReconnectForMetered, "PendingUpdateToReconnectForMetered")

} // namespace wsl::core::networking
