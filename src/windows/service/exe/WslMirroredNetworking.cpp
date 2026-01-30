/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WslMirroredNetworking.cpp

Abstract:

    This file contains WSL mirrored networking function definitions.

--*/

#include "precomp.h"
#include "WslMirroredNetworking.h"
#include "WslCoreMessageQueue.h"
#include "Stringify.h"
#include "WslCoreNetworkingSupport.h"
#include "WslCoreNetworkEndpointSettings.h"
#include "WslCoreHostDnsInfo.h"
#include "hcs.hpp"
#include "hns_schema.h"

static constexpr auto c_loopbackDeviceName = TEXT(LX_INIT_LOOPBACK_DEVICE_NAME);
static constexpr auto c_initialMirroredGoalStateWaitTimeoutMs = 5 * 1000;

using namespace wsl::windows::common;
using namespace wsl::shared;
using wsl::core::networking::EndpointIpAddress;
using wsl::core::networking::EndpointRoute;

namespace {
inline const auto HnsModifyRequestTypeToString(const hns::ModifyRequestType requestType)
{
    return JsonEnumToString<hns::ModifyRequestType>(requestType);
}
} // namespace

_Requires_lock_held_(m_networkLock)
void wsl::core::networking::WslMirroredNetworkManager::ProcessConnectivityChange()
{
    const std::set<GUID, wsl::windows::common::helpers::GuidLess> initialConnectedInterfaces{std::move(m_hostConnectedInterfaces)};
    m_hostConnectedInterfaces.clear();

    const auto coInit = wil::CoInitializeEx();
    const wil::com_ptr<INetworkListManager> networkListManager = wil::CoCreateInstance<NetworkListManager, INetworkListManager>();

    wil::com_ptr<IEnumNetworks> networksEnumerator;
    THROW_IF_FAILED(networkListManager->GetNetworks(NLM_ENUM_NETWORK_CONNECTED, &networksEnumerator));

    for (;;)
    {
        ULONG fetched{};
        wil::com_ptr<INetwork> networkInstance;
        auto hr = networksEnumerator->Next(1, &networkInstance, &fetched);
        THROW_IF_FAILED(hr);
        if (hr == S_FALSE || fetched == 0)
        {
            break;
        }

        // each NLM network could have multiple interfaces - walk through each
        // if we fail trying to access an individual interface, continue the loop for the other interfaces

        wil::com_ptr<IEnumNetworkConnections> enumNetworkConnections;
        hr = networkInstance->GetNetworkConnections(&enumNetworkConnections);
        if (FAILED(hr))
        {
            WSL_LOG(
                "WslMirroredNetworkManager::ProcessConnectivityChange - ignoring interface after processing "
                "INetworkConnection::GetAdapterId",
                TraceLoggingValue(hr, "hr"));
            continue;
        }

        for (;;)
        {
            ULONG fetchedNetworkConnections{};
            wil::com_ptr<INetworkConnection> networkConnection;
            hr = enumNetworkConnections->Next(1, &networkConnection, &fetchedNetworkConnections);
            if (FAILED(hr) || hr == S_FALSE || fetchedNetworkConnections == 0)
            {
                break;
            }

            GUID interfaceGuid{};
            hr = networkConnection->GetAdapterId(&interfaceGuid);
            if (FAILED(hr))
            {
                WSL_LOG(
                    "WslMirroredNetworkManager::ProcessConnectivityChange - ignoring interface INetworkConnection::GetAdapterId "
                    "failed",
                    TraceLoggingValue(hr, "hr"));
                continue;
            }

            NLM_CONNECTIVITY connectivity{};
            hr = networkConnection->GetConnectivity(&connectivity);
            if (FAILED(hr) || connectivity == NLM_CONNECTIVITY_DISCONNECTED)
            {
                WSL_LOG(
                    "WslMirroredNetworkManager::ProcessConnectivityChange - ignoring interface after processing "
                    "INetworkConnection::GetConnectivity",
                    TraceLoggingValue(wsl::shared::string::GuidToString<wchar_t>(interfaceGuid).c_str(), "interfaceGuid"),
                    TraceLoggingValue(connectivity == NLM_CONNECTIVITY_DISCONNECTED, "is_NLM_CONNECTIVITY_DISCONNECTED"),
                    TraceLoggingValue(hr, "hr"));
                continue;
            }

            m_hostConnectedInterfaces.insert(interfaceGuid);
        }
    }

    if (initialConnectedInterfaces != m_hostConnectedInterfaces)
    {
        WSL_LOG(
            "WslMirroredNetworkManager::ProcessConnectivityChange - reset goal state",
            TraceLoggingValue(initialConnectedInterfaces.size(), "previous_interfaces_size"),
            TraceLoggingValue(m_hostConnectedInterfaces.size(), "updated_interfaces_size"));

        m_inMirroredGoalState.ResetEvent();
        m_connectivityTelemetry.UpdateTimer();

        std::wstring guids;
        for (const auto& connectedInterface : initialConnectedInterfaces)
        {
            guids.append(wsl::shared::string::GuidToString<wchar_t>(connectedInterface) + L",");
        }

        WSL_LOG(
            "WslMirroredNetworkManager::ProcessConnectivityChange [previous]",
            TraceLoggingValue(guids.c_str(), "connectedInterfaces"));

        guids.clear();
        for (const auto& connectedInterface : m_hostConnectedInterfaces)
        {
            guids.append(wsl::shared::string::GuidToString<wchar_t>(connectedInterface) + L",");
        }

        WSL_LOG(
            "WslMirroredNetworkManager::ProcessConnectivityChange [updated]",
            TraceLoggingValue(guids.c_str(), "connectedInterfaces"));
    }
}

_Requires_lock_held_(m_networkLock)
void wsl::core::networking::WslMirroredNetworkManager::ProcessIpAddressChange()
{
    wsl::core::networking::unique_address_table addressTable{};
    THROW_IF_WIN32_ERROR(GetUnicastIpAddressTable(AF_UNSPEC, &addressTable));

    for (auto& endpoint : m_networkEndpoints)
    {
        const auto initialAddresses{std::move(endpoint.Network->IpAddresses)};
        endpoint.Network->IpAddresses.clear();

        // if the interface isn't connected, ensure we always track zero addresses
        if (!endpoint.Network->IsConnected)
        {
            continue;
        }

        for (const auto& address : wil::make_range(addressTable.get()->Table, addressTable.get()->NumEntries))
        {
            if (address.InterfaceIndex != endpoint.Network->InterfaceIndex)
            {
                continue;
            }

            const auto endpointAddress = EndpointIpAddress(address);
            if (endpointAddress.IsPreferred())
            {
                endpoint.Network->IpAddresses.insert(endpointAddress);
            }
        }

        if (initialAddresses != endpoint.Network->IpAddresses)
        {
            WSL_LOG(
                "WslMirroredNetworkManager::ProcessIpAddressChange - reset goal state",
                TraceLoggingValue(endpoint.EndpointId, "endpointId"),
                TraceLoggingValue(endpoint.InterfaceGuid, "interfaceGuid"),
                TraceLoggingValue(initialAddresses.size(), "previous_addresses_size"),
                TraceLoggingValue(endpoint.Network->IpAddresses.size(), "updated_addresses_size"));

            m_inMirroredGoalState.ResetEvent();
            m_connectivityTelemetry.UpdateTimer();

            for (const auto& address : initialAddresses)
            {
                WSL_LOG(
                    "WslMirroredNetworkManager::ProcessIpAddressChange [previous]",
                    TraceLoggingValue(endpoint.EndpointId, "endpointId"),
                    TraceLoggingValue(endpoint.InterfaceGuid, "interfaceGuid"),
                    TraceLoggingValue(address.AddressString.c_str(), "address"),
                    TraceLoggingValue(address.PrefixLength, "prefixLength"));
            }
            for (const auto& address : endpoint.Network->IpAddresses)
            {
                WSL_LOG(
                    "WslMirroredNetworkManager::ProcessIpAddressChange [updated]",
                    TraceLoggingValue(endpoint.EndpointId, "endpointId"),
                    TraceLoggingValue(endpoint.InterfaceGuid, "interfaceGuid"),
                    TraceLoggingValue(address.AddressString.c_str(), "address"),
                    TraceLoggingValue(address.PrefixLength, "prefixLength"));
            }
        }
    }
}

_Requires_lock_held_(m_networkLock)
void wsl::core::networking::WslMirroredNetworkManager::ProcessRouteChange()
{
    wsl::core::networking::unique_address_table addressTable{};
    wsl::core::networking::unique_forward_table routeTable{};
    THROW_IF_WIN32_ERROR(GetIpForwardTable2(AF_UNSPEC, &routeTable));

    for (auto& endpoint : m_networkEndpoints)
    {
        const auto initialRoutes = endpoint.Network->Routes;
        endpoint.Network->Routes.clear();

        // if the interface isn't connected, ensure we always track zero routes
        // Windows can have routes assigned on disconnected interfaces, Linux cannot
        if (!endpoint.Network->IsConnected)
        {
            continue;
        }

        // Gather endpoint address prefixes and raw address
        std::unordered_set<std::wstring> addressPrefixes{};
        std::unordered_set<std::wstring> addresses{};
        std::unordered_set<std::wstring> ipv4broadcastAddresses{};

        for (const auto& endpointAddress : endpoint.Network->IpAddresses)
        {
            addresses.insert(endpointAddress.AddressString);

            auto addressPrefix = endpointAddress.GetPrefix();
            WI_ASSERT(!addressPrefix.empty());
            if (!addressPrefix.empty())
            {
                addressPrefixes.insert(std::move(addressPrefix));
            }

            if (endpointAddress.Address.si_family == AF_INET)
            {
                auto v4BroadcastMaskAddress = endpointAddress.GetIpv4BroadcastMask();
                WI_ASSERT(!v4BroadcastMaskAddress.empty());
                if (!v4BroadcastMaskAddress.empty())
                {
                    ipv4broadcastAddresses.emplace(std::move(v4BroadcastMaskAddress));
                }
            }
        }

        for (const auto& route : wil::make_range(routeTable.get()->Table, routeTable.get()->NumEntries))
        {
            if (route.InterfaceIndex == endpoint.Network->InterfaceIndex)
            {
                auto endpointRoute = EndpointRoute(route);

                endpointRoute.IsAutoGeneratedPrefixRoute =
                    endpointRoute.IsNextHopOnlink() && addressPrefixes.contains(endpointRoute.GetFullDestinationPrefix());

                // Ignore host IPv4 routes, e.g. 192.168.5.2/32 -> 0.0.0.0
                if (addresses.contains(endpointRoute.DestinationPrefixString))
                {
                    continue;
                }

                // ignore host routes for deprecated addresses
                // the address will not be in the 'addresses' variable above since it's deprecated
                // e.g. a route 2001:0:d5b:9458:1ceb:518b:7c94:609e/128, but the matching local IP address is deprecated
                bool shouldIgnoreUnicastAddressRoute = false;
                if (endpointRoute.IsUnicastAddressRoute())
                {
                    if (!addressTable)
                    {
                        THROW_IF_WIN32_ERROR(GetUnicastIpAddressTable(AF_UNSPEC, &addressTable));
                    }
                    // find the address matching this destination prefix
                    for (const auto& address : wil::make_range(addressTable.get()->Table, addressTable.get()->NumEntries))
                    {
                        if (address.InterfaceIndex != endpoint.Network->InterfaceIndex)
                        {
                            continue;
                        }

                        const auto endpointAddress = EndpointIpAddress(address);
                        if (endpointAddress.Address == route.DestinationPrefix.Prefix)
                        {
                            if (!endpointAddress.IsPreferred())
                            {
                                shouldIgnoreUnicastAddressRoute = true;
                                break;
                            }
                        }
                    }
                }
                if (shouldIgnoreUnicastAddressRoute)
                {
                    continue;
                }

                if (endpointRoute.DestinationPrefix.Prefix.si_family == AF_INET)
                {
                    if (endpoint.Network->DisableIpv4DefaultRoutes && endpointRoute.IsDefault())
                    {
                        continue;
                    }

                    const auto addressType =
                        Ipv4AddressType(reinterpret_cast<const UCHAR*>(&endpointRoute.DestinationPrefix.Prefix.Ipv4.sin_addr));
                    if (addressType != NlatUnspecified && addressType != NlatUnicast)
                    {
                        // ignore broadcast and multicast routes - Linux doesn't seem to create those like Windows
                        continue;
                    }

                    if (ipv4broadcastAddresses.contains(endpointRoute.DestinationPrefixString))
                    {
                        continue;
                    }
                }
                else if (endpointRoute.DestinationPrefix.Prefix.si_family == AF_INET6)
                {
                    if (endpoint.Network->DisableIpv6DefaultRoutes && endpointRoute.IsDefault())
                    {
                        continue;
                    }

                    const auto addressType =
                        Ipv6AddressType(reinterpret_cast<const UCHAR*>(&endpointRoute.DestinationPrefix.Prefix.Ipv6.sin6_addr));
                    if (addressType != NlatUnspecified && addressType != NlatUnicast)
                    {
                        // ignore broadcast and multicast routes - Linux doesn't seem to create those like Windows
                        continue;
                    }
                }

                // update the route metric for Linux - which to be equivalent to Windows must be the sum of the interface metric and route metric
                endpointRoute.Metric += (endpointRoute.Family == AF_INET) ? endpoint.Network->IPv4InterfaceMetric.value_or(0)
                                                                          : endpoint.Network->IPv6InterfaceMetric.value_or(0);
                if (endpointRoute.Metric > UINT16_MAX)
                {
                    endpointRoute.Metric = UINT16_MAX;
                }
                endpoint.Network->Routes.insert(endpointRoute);
            }
        }

        // Linux requires that there's an onlink route for any route with a NextHop address that's not all-zeros (on-link)
        // "normal" network deployments with Windows creates an address prefix route that includes that next hop
        // but some deployments, like some VPNs, do not include a prefix route that includes the nexthop
        // While that works in Windows (all nexthop addresses in a route *must* be on-link), it won't work in Linux
        // thus we must guarantee an onlink route for all routes with a non-zero nexthop
        std::vector<EndpointRoute> newRoutes;
        for (const auto& route : endpoint.Network->Routes)
        {
            if (!route.IsNextHopOnlink())
            {
                EndpointRoute newRoute;
                newRoute.Family = route.Family;
                newRoute.Metric = route.Metric;
                newRoute.SitePrefixLength = route.GetMaxPrefixLength();

                // update the destination prefix to the nexthop address /32 (for ipv4) or /128 (for ipv6)
                newRoute.DestinationPrefix.Prefix = route.NextHop;
                newRoute.DestinationPrefix.PrefixLength = route.GetMaxPrefixLength();
                newRoute.DestinationPrefixString = windows::common::string::SockAddrInetToWstring(newRoute.DestinationPrefix.Prefix);

                // update the destination prefix to be all zeros (on-link)
                ZeroMemory(&newRoute.NextHop, sizeof newRoute.NextHop);
                newRoute.NextHop.si_family = route.NextHop.si_family;
                newRoute.NextHopString = windows::common::string::SockAddrInetToWstring(newRoute.NextHop);

                // force a copy so the route strings are re-calculated in the new EndpointRoute object
                newRoutes.emplace_back(std::move(newRoute));
            }
        }
        for (const auto& route : newRoutes)
        {
            endpoint.Network->Routes.insert(route);
        }

        if (initialRoutes != endpoint.Network->Routes)
        {
            WSL_LOG(
                "WslMirroredNetworkManager::ProcessRouteChange - reset goal state",
                TraceLoggingValue(endpoint.EndpointId, "endpointId"),
                TraceLoggingValue(endpoint.InterfaceGuid, "interfaceGuid"),
                TraceLoggingValue(initialRoutes.size(), "previous_routes_size"),
                TraceLoggingValue(endpoint.Network->Routes.size(), "updated_routes_size"));

            m_inMirroredGoalState.ResetEvent();
            m_connectivityTelemetry.UpdateTimer();

            for (const auto& route : initialRoutes)
            {
                WSL_LOG(
                    "WslMirroredNetworkManager::ProcessRouteChange [previous]",
                    TraceLoggingValue(endpoint.EndpointId, "endpointId"),
                    TraceLoggingValue(endpoint.InterfaceGuid, "interfaceGuid"),
                    TraceLoggingValue(route.Metric, "metric"),
                    TraceLoggingValue(route.NextHopString.c_str(), "nextHop"),
                    TraceLoggingValue(route.DestinationPrefixString.c_str(), "destinationPrefix"),
                    TraceLoggingValue(route.DestinationPrefix.PrefixLength, "destinationPrefixLength"));
            }
            for (const auto& route : endpoint.Network->Routes)
            {
                WSL_LOG(
                    "WslMirroredNetworkManager::ProcessRouteChange [updated]",
                    TraceLoggingValue(endpoint.EndpointId, "endpointId"),
                    TraceLoggingValue(endpoint.InterfaceGuid, "interfaceGuid"),
                    TraceLoggingValue(route.Metric, "metric"),
                    TraceLoggingValue(route.NextHopString.c_str(), "nextHop"),
                    TraceLoggingValue(route.DestinationPrefixString.c_str(), "destinationPrefix"),
                    TraceLoggingValue(route.DestinationPrefix.PrefixLength, "destinationPrefixLength"));
            }
        }
    }
}

_Requires_lock_held_(m_networkLock)
void wsl::core::networking::WslMirroredNetworkManager::ProcessDNSChange()
{
    const auto initialDnsInfo = m_dnsInfo;

    if (m_vmConfig.EnableDnsTunneling)
    {
        m_dnsInfo = wsl::core::networking::HostDnsInfo::GetDnsTunnelingSettings(m_dnsTunnelingIpAddress);
    }
    else
    {
        m_hostDnsInfo.UpdateNetworkInformation();
        m_dnsInfo = m_hostDnsInfo.GetDnsSettings(
            wsl::core::networking::DnsSettingsFlags::IncludeVpn | wsl::core::networking::DnsSettingsFlags::IncludeIpv6Servers |
            wsl::core::networking::DnsSettingsFlags::IncludeAllSuffixes);
    }

    if (initialDnsInfo != m_dnsInfo)
    {
        WSL_LOG("WslMirroredNetworkManager::ProcessDNSChange - reset goal state");
        m_inMirroredGoalState.ResetEvent();
        m_connectivityTelemetry.UpdateTimer();

        WSL_LOG(
            "WslMirroredNetworkManager::ProcessDNSChange [previous]",
            TraceLoggingValue(wsl::shared::string::Join(initialDnsInfo.Domains, ',').c_str(), "domainList"),
            TraceLoggingValue(wsl::shared::string::Join(initialDnsInfo.Servers, ',').c_str(), "dnsServerList"));

        WSL_LOG(
            "WslMirroredNetworkManager::ProcessDNSChange [updated]",
            TraceLoggingValue(wsl::shared::string::Join(m_dnsInfo.Domains, ',').c_str(), "domainList"),
            TraceLoggingValue(wsl::shared::string::Join(m_dnsInfo.Servers, ',').c_str(), "dnsServerList"));
    }
}

_Requires_lock_held_(m_networkLock)
void wsl::core::networking::WslMirroredNetworkManager::ProcessInterfaceChange()
{
    wsl::core::networking::unique_interface_table interfaceTable{};
    THROW_IF_WIN32_ERROR(::GetIpInterfaceTable(AF_UNSPEC, &interfaceTable));

    for (auto& endpoint : m_networkEndpoints)
    {
        const auto originalIPv4DisableDefaultRoutes = endpoint.Network->DisableIpv4DefaultRoutes;
        const auto originalIPv6DisableDefaultRoutes = endpoint.Network->DisableIpv6DefaultRoutes;
        const auto originallyConnected = endpoint.Network->IsConnected;
        const auto originalMinimumMtu = endpoint.Network->GetEffectiveMtu();
        const auto originalMinimumMetric = endpoint.Network->GetMinimumMetric();

        endpoint.Network->IsConnected = false;

        auto interfaceFoundCount = 0;
        for (const auto& ipInterface : wil::make_range(interfaceTable.get()->Table, interfaceTable.get()->NumEntries))
        {
            if (ipInterface.InterfaceIndex != endpoint.Network->InterfaceIndex ||
                (ipInterface.Family != AF_INET && ipInterface.Family != AF_INET6))
            {
                continue;
            }

            // Endpoint is marked as connected if either IPv4 or IPv6 interface is connected
            endpoint.Network->IsConnected = endpoint.Network->IsConnected || !!ipInterface.Connected;

            if (ipInterface.Family == AF_INET)
            {
                endpoint.Network->IPv4InterfaceMtu = ipInterface.NlMtu;
                endpoint.Network->IPv4InterfaceMetric = ipInterface.Metric;
                endpoint.Network->DisableIpv4DefaultRoutes = ipInterface.DisableDefaultRoutes;
            }
            else
            {
                endpoint.Network->IPv6InterfaceMtu = ipInterface.NlMtu;
                endpoint.Network->IPv6InterfaceMetric = ipInterface.Metric;
                endpoint.Network->DisableIpv6DefaultRoutes = ipInterface.DisableDefaultRoutes;
            }

            ++interfaceFoundCount;
            if (interfaceFoundCount > 1)
            {
                // we already found both v4 and v6
                break;
            }
        }

        const auto disableDefaultRoutesUpdated = originalIPv4DisableDefaultRoutes != endpoint.Network->DisableIpv4DefaultRoutes ||
                                                 originalIPv6DisableDefaultRoutes != endpoint.Network->DisableIpv6DefaultRoutes;
        const auto connectedStateUpdated = originallyConnected != endpoint.Network->IsConnected;
        const auto minimumMtu = endpoint.Network->GetEffectiveMtu();
        const auto mtuUpdated = originalMinimumMtu != minimumMtu;
        const auto minimumMetric = endpoint.Network->GetMinimumMetric();
        const auto metricUpdate = originalMinimumMetric != minimumMetric;

        endpoint.Network->PendingIPInterfaceUpdate |= connectedStateUpdated || mtuUpdated || metricUpdate;

        if (disableDefaultRoutesUpdated || connectedStateUpdated || mtuUpdated || metricUpdate)
        {
            // we want to trace when disableDefaultRoutesUpdated, but that won't trigger resetting the goal-state
            // if disableDefaultRoutesUpdated affects routes, then ProcessRouteChange will reset the goal-state accordingly
            // but we do want to trace when disableDefaultRoutes get updated - to greatly help debugging
            if (connectedStateUpdated || mtuUpdated || metricUpdate)
            {
                WSL_LOG("WslMirroredNetworkManager::ProcessInterfaceChange - reset goal state");
                m_inMirroredGoalState.ResetEvent();
                m_connectivityTelemetry.UpdateTimer();
            }

            WSL_LOG(
                "WslMirroredNetworkManager::ProcessInterfaceChange [previous]",
                TraceLoggingValue(endpoint.EndpointId, "endpointId"),
                TraceLoggingValue(endpoint.InterfaceGuid, "interfaceGuid"),
                TraceLoggingValue(originallyConnected, "isConnected"),
                TraceLoggingValue(originalMinimumMtu, "EffectiveMtu"),
                TraceLoggingValue(originalMinimumMetric, "MinimumMetric"),
                TraceLoggingValue(originalIPv4DisableDefaultRoutes, "disableIpv4DefaultRoutes"),
                TraceLoggingValue(originalIPv6DisableDefaultRoutes, "disableIpv6DefaultRoutes"));

            WSL_LOG(
                "WslMirroredNetworkManager::ProcessInterfaceChange [updated]",
                TraceLoggingValue(endpoint.EndpointId, "endpointId"),
                TraceLoggingValue(endpoint.InterfaceGuid, "interfaceGuid"),
                TraceLoggingValue(endpoint.Network->IsConnected, "isConnected"),
                TraceLoggingValue(endpoint.Network->IPv4InterfaceMtu, "ipv4InterfaceMtu"),
                TraceLoggingValue(endpoint.Network->IPv6InterfaceMtu, "ipv6InterfaceMtu"),
                TraceLoggingValue(endpoint.Network->IPv4InterfaceMetric.value_or(0xffffffff), "IPv4InterfaceMetric"),
                TraceLoggingValue(endpoint.Network->IPv6InterfaceMetric.value_or(0xffffffff), "IPv6InterfaceMetric"),
                TraceLoggingValue(endpoint.Network->DisableIpv4DefaultRoutes, "disableIpv4DefaultRoutes"),
                TraceLoggingValue(endpoint.Network->DisableIpv6DefaultRoutes, "disableIpv6DefaultRoutes"));
        }
    }
}

wsl::core::networking::WslMirroredNetworkManager::WslMirroredNetworkManager(
    HCS_SYSTEM hcsSystem,
    const Config& config,
    GnsMessageCallbackWithCallbackResult&& GnsMessageCallbackWithCallbackResult,
    AddNetworkEndpointCallback&& addNetworkEndpointCallback,
    const std::pair<uint16_t, uint16_t>& ephemeralPortRange) :
    m_callbackForGnsMessage(std::move(GnsMessageCallbackWithCallbackResult)),
    m_addNetworkEndpointCallback(std::move(addNetworkEndpointCallback)),
    m_hcsSystem{hcsSystem},
    m_vmConfig{config},
    m_ephemeralPortRange(ephemeralPortRange),
    m_state(State::Starting)
{
    // ensure the MTA apartment stays alive for the lifetime of this object in this process
    // we do not want to risk COM unloading / reloading when we need to make our WinRT API calls
    // which by default will be in the MTA
    LOG_IF_FAILED(CoIncrementMTAUsage(&m_mtaCookie));

    // locking in the c'tor in case any of the below callbacks fire before this object is fully constructed
    const auto lock = m_networkLock.lock_exclusive();

    // keep the WinRT DLL loaded for the lifetime of this instance. we instantiate it repeatedly,
    // and today we are loading and unloading 7 dll's over and over again - each time we call it.
    // this also circumvents many performance optimizations we made with our WinRT API
    const auto roInit = wil::RoInitialize();
    m_networkInformationStatics = wil::GetActivationFactory<ABI::Windows::Networking::Connectivity::INetworkInformationStatics>(
        RuntimeClass_Windows_Networking_Connectivity_NetworkInformation);

    m_netListManager = wil::CoCreateInstance<NetworkListManager, INetworkListManager>();
    // create an event sink for NLM Network change notifications, then register (Advise) with NLM
    m_netListManagerEventSink = wil::com_ptr<INetworkEvents>(Microsoft::WRL::Make<PublicNLMSink>(this));
    // INetworkListManager is actually an inproc COM API - it just calls private COM APIs which are hosted in a service
    m_netListManagerAdviseHandler.AdviseInProcObject<INetworkEvents>(m_netListManager, m_netListManagerEventSink.get());

    // Subscribe for network change notifications. This is done before
    // obtaining the initial list of networks to connect to, in order to
    // avoid a race condition between the initial enumeration and any network
    // changes that may be occurring at the same time. The subscription will
    // receive network change events, but will not be able to react to them
    // the lock is released.
    m_hcnCallback = windows::common::hcs::RegisterServiceCallback(HcnCallback, this);

    // Create the timer used to retry the HNS service connection.
    m_retryHcnServiceConnectionTimer.reset(CreateThreadpoolTimer(HcnServiceConnectionTimerCallback, this, nullptr));
    THROW_IF_NULL_ALLOC(m_retryHcnServiceConnectionTimer);

    // Create the timer used to retry syncing pending IP state with Linux.
    m_retryLinuxIpStateSyncTimer.reset(CreateThreadpoolTimer(RetryLinuxIpStateSyncTimerCallback, this, nullptr));
    THROW_IF_NULL_ALLOC(m_retryLinuxIpStateSyncTimer);

    m_debounceUpdateAllEndpointsDefaultTimer.reset(CreateThreadpoolTimer(DebounceUpdateAllEndpointsDefaultTimerFired, this, nullptr));
    THROW_IF_NULL_ALLOC(m_debounceUpdateAllEndpointsDefaultTimer);

    m_debounceCreateEndpointFailureTimer.reset(CreateThreadpoolTimer(DebounceCreateEndpointFailureTimerFired, this, nullptr));
    THROW_IF_NULL_ALLOC(m_debounceCreateEndpointFailureTimer);

    // Populate the initial list of networks. The list will then be kept
    // up to date by the above subscription notifications.
    for (const auto& networkId : EnumerateMirroredNetworks())
    {
        // Must call back through MirroredNetworking to create a new Endpoint
        // note that the callback will not block - it just queues the work in MirroredNetworking
        LOG_IF_FAILED(AddNetwork(networkId));
    }

    // once HNS has started creating networks, start our telemetry timer
    if (config.EnableTelemetry && !WslTraceLoggingShouldDisableTelemetry())
    {
        m_connectivityTelemetry.StartTimer([&](NLM_CONNECTIVITY hostConnectivity, uint32_t telemetryCounter) {
            TelemetryConnectionCallback(hostConnectivity, telemetryCounter);
        });
    }

    if (config.DnsTunnelingIpAddress.has_value())
    {
        m_dnsTunnelingIpAddress = wsl::windows::common::string::IntegerIpv4ToWstring(config.DnsTunnelingIpAddress.value());
    }

    m_state = State::Started;
}

wsl::core::networking::WslMirroredNetworkManager::~WslMirroredNetworkManager() noexcept
{
    Stop();
}

wsl::core::networking::WslMirroredNetworkManager::HnsStatus wsl::core::networking::WslMirroredNetworkManager::Stop() noexcept
{
    HnsStatus returnStatus{};
    try
    {
        // scope to the lock to flip the bit that we are stopping
        {
            const auto lock = m_networkLock.lock_exclusive();
            m_state = State::Stopped;
            returnStatus = m_latestHnsStatus;
        }

        // must set state first so all other threads won't make forward progress
        // since we are about to stop all timers and callbacks
        // which must be stopped not holding our lock

        // Next stop the telemetry timer which could queue work to linux (through m_gnsCallbackQueue)
        m_connectivityTelemetry.Reset();

        // Next stop the timer which could reset the hcnCallback
        m_retryHcnServiceConnectionTimer.reset();

        // Next stop the Hcn callback, which could add/remove networks
        m_hcnCallback.reset();

        m_debounceUpdateAllEndpointsDefaultTimer.reset();

        m_debounceCreateEndpointFailureTimer.reset();

        // Stop the linux ip state sync timer
        m_retryLinuxIpStateSyncTimer.reset();

        // canceling the callback queue only after stopping all sources that could queue a callback
        m_gnsCallbackQueue.cancel();
        m_hnsQueue.cancel();

        // all of the above must be done outside holding a lock to avoid deadlocks
        const auto lock = m_networkLock.lock_exclusive();
        m_networkEndpoints.clear();
    }
    CATCH_LOG()

    return returnStatus;
}

void wsl::core::networking::WslMirroredNetworkManager::DebounceUpdateAllEndpointsDefaultTimerFired(
    _Inout_ PTP_CALLBACK_INSTANCE, _Inout_opt_ PVOID Context, _Inout_ PTP_TIMER)
try
{
    auto* const instance = static_cast<WslMirroredNetworkManager*>(Context);

    const auto lock = instance->m_networkLock.lock_exclusive();
    instance->m_IsDebounceUpdateAllEndpointsDefaultTimerSet = false;
    if (instance->m_state == State::Stopped)
    {
        return;
    }

    instance->UpdateAllEndpointsImpl(UpdateEndpointFlag::Default, "DebounceUpdateAllEndpointsDefaultTimerFired");
}
CATCH_LOG()

void wsl::core::networking::WslMirroredNetworkManager::DebounceCreateEndpointFailureTimerFired(
    _Inout_ PTP_CALLBACK_INSTANCE, _Inout_opt_ PVOID Context, _Inout_ PTP_TIMER)
try
{
    auto* const instance = static_cast<WslMirroredNetworkManager*>(Context);

    const auto lock = instance->m_networkLock.lock_exclusive();
    if (instance->m_state == State::Stopped)
    {
        return;
    }

    if (!instance->m_failedEndpointProperties.empty())
    {
        // AddEndpointImpl will update m_failedEndpointProperties if any re-attempts to add the endpoint fail
        // thus we must first move everything out
        auto failedEndpointProperties = std::move(instance->m_failedEndpointProperties);
        instance->m_failedEndpointProperties.clear();
        for (auto& endpointProperties : failedEndpointProperties)
        {
            instance->AddEndpointImpl(std::move(endpointProperties));
        }
    }
}
CATCH_LOG()

_Requires_lock_held_(m_networkLock)
std::vector<GUID> wsl::core::networking::WslMirroredNetworkManager::EnumerateMirroredNetworks() const noexcept
try
{
    WI_ASSERT(m_state == State::Started || m_state == State::Starting);

    return EnumerateMirroredNetworksAndHyperVFirewall(m_vmConfig.FirewallConfig.Enabled());
}
catch (...)
{
    LOG_CAUGHT_EXCEPTION();
    return {};
}

_Requires_lock_held_(m_networkLock)
_Check_return_ HRESULT wsl::core::networking::WslMirroredNetworkManager::AddNetwork(const GUID& networkId) noexcept
try
{
    WSL_LOG("WslMirroredNetworkManager::AddNetwork", TraceLoggingValue(networkId, "networkId"));

    // Inform the parent class to create a new endpoint object which we can then connect into the container
    m_hnsQueue.submit([this, networkId] { m_addNetworkEndpointCallback(networkId); });

    return S_OK;
}
CATCH_RETURN()

_Requires_lock_held_(m_networkLock)
_Check_return_ HRESULT wsl::core::networking::WslMirroredNetworkManager::RemoveNetwork(const GUID& networkId) noexcept
try
{
    WSL_LOG("WslMirroredNetworkManager::RemoveNetwork", TraceLoggingValue(networkId, "networkId"));

    const auto foundEndpoint =
        std::ranges::find_if(m_networkEndpoints, [&](const auto& endpoint) { return endpoint.NetworkId == networkId; });
    if (foundEndpoint == std::end(m_networkEndpoints))
    {
        WSL_LOG("WslMirroredNetworkManager::RemoveNetwork - Network not found", TraceLoggingValue(networkId, "networkId"));
        return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    }

    // RemoveEndpoint will remove this endpoint from m_networkEndpoints
    return RemoveEndpoint(foundEndpoint->EndpointId);
}
CATCH_RETURN()

void __stdcall wsl::core::networking::WslMirroredNetworkManager::RetryLinuxIpStateSyncTimerCallback(
    _Inout_ PTP_CALLBACK_INSTANCE, _Inout_opt_ PVOID Context, _Inout_ PTP_TIMER) noexcept
{
    auto* const manager = static_cast<WslMirroredNetworkManager*>(Context);
    const auto lock = manager->m_networkLock.lock_exclusive();
    if (manager->m_state == State::Stopped)
    {
        return;
    }

    manager->UpdateAllEndpointsImpl(UpdateEndpointFlag::Default, "RetryLinuxIpStateSyncTimerCallback");
}

_Requires_lock_held_(m_networkLock)
_Check_return_ HRESULT wsl::core::networking::WslMirroredNetworkManager::SendAddressRequestToGns(
    const NetworkEndpoint& endpoint, const TrackedIpAddress& address, hns::ModifyRequestType requestType) noexcept
try
{
    hns::ModifyGuestEndpointSettingRequest<hns::IPAddress> modifyRequest;
    modifyRequest.ResourceType = hns::GuestEndpointResourceType::IPAddress;
    modifyRequest.RequestType = requestType;
    modifyRequest.targetDeviceName = wsl::shared::string::GuidToString<wchar_t>(endpoint.InterfaceGuid);
    modifyRequest.Settings = address.ConvertToHnsSettingsMsg();

    WSL_LOG(
        "WslMirroredNetworkManager::SendAddressRequestToGns",
        TraceLoggingValue("ModifyGuestDeviceSettingRequest - set address [queued]", "GnsMessage"),
        TraceLoggingValue(HnsModifyRequestTypeToString(requestType).c_str(), "requestType"),
        TraceLoggingValue(endpoint.InterfaceGuid, "InterfaceGuid"),
        TraceLoggingValue(address.Address.AddressString.c_str(), "ipAddress"),
        TraceLoggingValue(address.Address.PrefixLength, "prefixLength"),
        TraceLoggingValue(address.Address.IsPreferred(), "isPreferred"));

    int linuxResultCode{};
    // can safely capture by ref since we are waiting
    const auto hr = m_gnsCallbackQueue.submit_and_wait([&] {
        return m_callbackForGnsMessage(LxGnsMessageDeviceSettingRequest, ToJsonW(modifyRequest), GnsCallbackFlags::Wait, &linuxResultCode);
    });

    WSL_LOG(
        "WslMirroredNetworkManager::SendAddressRequestToGns",
        TraceLoggingValue("ModifyGuestDeviceSettingRequest - set address [completed]", "GnsMessage"),
        TraceLoggingHResult(hr, "hr"),
        TraceLoggingValue(linuxResultCode, "linuxResultCode"));

    address.SyncRetryCount = (address.SyncRetryCount > 0) ? address.SyncRetryCount - 1 : 0;
    return hr;
}
CATCH_RETURN()

_Requires_lock_held_(m_networkLock)
_Check_return_ HRESULT wsl::core::networking::WslMirroredNetworkManager::SendRouteRequestToGns(
    const NetworkEndpoint& endpoint, const TrackedRoute& route, hns::ModifyRequestType requestType) noexcept
try
{
    hns::ModifyGuestEndpointSettingRequest<hns::Route> modifyRequest;
    modifyRequest.ResourceType = hns::GuestEndpointResourceType::Route;
    modifyRequest.RequestType = requestType;
    modifyRequest.targetDeviceName = wsl::shared::string::GuidToString<wchar_t>(endpoint.InterfaceGuid);
    modifyRequest.Settings = route.ConvertToHnsSettingsMsg();

    WSL_LOG(
        "WslMirroredNetworkManager::SendRouteRequestToGns",
        TraceLoggingValue("ModifyGuestDeviceSettingRequest : set route [queued]", "GnsMessage"),
        TraceLoggingValue(HnsModifyRequestTypeToString(requestType).c_str(), "requestType"),
        TraceLoggingValue(endpoint.InterfaceGuid, "InterfaceGuid"),
        TraceLoggingValue(route.Route.DestinationPrefixString.c_str(), "destinationPrefix"),
        TraceLoggingValue(route.Route.DestinationPrefix.PrefixLength, "prefixLength"),
        TraceLoggingValue(route.Route.NextHopString.c_str(), "nextHop"),
        TraceLoggingValue(route.Route.Metric, "metric"));

    int linuxResultCode{};
    // can safely capture by ref since we are waiting
    const auto hr = m_gnsCallbackQueue.submit_and_wait([&] {
        return m_callbackForGnsMessage(LxGnsMessageDeviceSettingRequest, ToJsonW(modifyRequest), GnsCallbackFlags::Wait, &linuxResultCode);
    });

    WSL_LOG(
        "WslMirroredNetworkManager::SendRouteRequestToGns",
        TraceLoggingValue("ModifyGuestDeviceSettingRequest : set route [completed]", "GnsMessage"),
        TraceLoggingHResult(hr, "hr"),
        TraceLoggingValue(linuxResultCode, "linuxResultCode"));

    route.SyncRetryCount = (route.SyncRetryCount > 0) ? route.SyncRetryCount - 1 : 0;
    return hr;
}
CATCH_RETURN()

_Requires_lock_held_(m_networkLock)
_Check_return_ HRESULT wsl::core::networking::WslMirroredNetworkManager::SendLoopbackRequestToGns(
    const NetworkEndpoint& endpoint, const TrackedIpAddress& address, hns::OperationType operation) noexcept
try
{
    hns::LoopbackRoutesRequest loopbackRequest;
    loopbackRequest.operation = operation;
    loopbackRequest.targetDeviceName = wsl::shared::string::GuidToString<wchar_t>(endpoint.InterfaceGuid);
    loopbackRequest.family = address.Address.Address.si_family;
    loopbackRequest.ipAddress = address.Address.AddressString;

    WSL_LOG(
        "WslMirroredNetworkManager::SendLoopbackRequestToGns",
        TraceLoggingValue("LoopbackRoutesRequest [queued]", "GnsMessage"),
        TraceLoggingValue(JsonEnumToString(operation).c_str(), "requestType"),
        TraceLoggingValue(endpoint.InterfaceGuid, "InterfaceGuid"),
        TraceLoggingValue(address.Address.AddressString.c_str(), "ipAddress"));

    int linuxResultCode{};
    // can safely capture by ref since we are waiting
    const auto hr = m_gnsCallbackQueue.submit_and_wait([&]() {
        return m_callbackForGnsMessage(LxGnsMessageLoopbackRoutesRequest, ToJsonW(loopbackRequest), GnsCallbackFlags::Wait, &linuxResultCode);
    });

    WSL_LOG(
        "WslMirroredNetworkManager::SendLoopbackRequestToGns",
        TraceLoggingValue("LoopbackRoutesRequest [completed]", "GnsMessage"),
        TraceLoggingHResult(hr, "hr"),
        TraceLoggingValue(linuxResultCode, "linuxResultCode"));

    return hr;
}
CATCH_RETURN()

_Requires_lock_held_(m_networkLock)
_Check_return_ HRESULT wsl::core::networking::WslMirroredNetworkManager::SendDnsRequestToGns(
    const NetworkEndpoint& endpoint, const DnsInfo& dnsInfo, hns::ModifyRequestType requestType) noexcept
try
{
    hns::ModifyGuestEndpointSettingRequest<hns::DNS> modifyRequest;
    modifyRequest.ResourceType = hns::GuestEndpointResourceType::DNS;
    modifyRequest.RequestType = requestType;
    modifyRequest.targetDeviceName = wsl::shared::string::GuidToString<wchar_t>(endpoint.InterfaceGuid);
    modifyRequest.Settings = BuildDnsNotification(dnsInfo);

    WSL_LOG(
        "WslMirroredNetworkManager::SendDnsRequestToGns",
        TraceLoggingValue("ModifyGuestDeviceSettingRequest : set DNS [queued]", "GnsMessage"),
        TraceLoggingValue(HnsModifyRequestTypeToString(requestType).c_str(), "requestType"),
        TraceLoggingValue(endpoint.InterfaceGuid, "InterfaceGuid"),
        TraceLoggingValue(m_vmConfig.EnableDnsTunneling, "m_vmConfig.EnableDnsTunneling"),
        TraceLoggingValue(wsl::shared::string::Join(dnsInfo.Servers, ',').c_str(), "server list"),
        TraceLoggingValue(wsl::shared::string::Join(dnsInfo.Domains, ',').c_str(), "suffix list"));

    int linuxResultCode{};
    // can safely capture by ref since we are waiting
    const auto hr = m_gnsCallbackQueue.submit_and_wait([&] {
        return m_callbackForGnsMessage(LxGnsMessageDeviceSettingRequest, ToJsonW(modifyRequest), GnsCallbackFlags::Wait, &linuxResultCode);
    });

    WSL_LOG(
        "WslMirroredNetworkManager::SendDnsRequestToGns",
        TraceLoggingValue("ModifyGuestDeviceSettingRequest : set DNS [completed]", "GnsMessage"),
        TraceLoggingHResult(hr, "hr"),
        TraceLoggingValue(linuxResultCode, "linuxResultCode"));

    return hr;
}
CATCH_RETURN()

_Requires_lock_held_(m_networkLock)
_Check_return_ HRESULT wsl::core::networking::WslMirroredNetworkManager::SendInterfaceRequestToGns(const NetworkEndpoint& endpoint) noexcept
try
{
    const auto interfaceConnected = endpoint.Network->IsConnected;
    const auto interfaceMtu = endpoint.Network->GetEffectiveMtu();
    const auto interfaceMetric = endpoint.Network->GetMinimumMetric();

    hns::ModifyGuestEndpointSettingRequest<hns::NetworkInterface> modifyRequest;
    modifyRequest.Settings.Connected = interfaceConnected;
    modifyRequest.Settings.NlMtu = interfaceMtu;
    modifyRequest.Settings.Metric = interfaceMetric;
    modifyRequest.ResourceType = hns::GuestEndpointResourceType::Interface;
    modifyRequest.targetDeviceName = wsl::shared::string::GuidToString<wchar_t>(endpoint.InterfaceGuid);

    WSL_LOG(
        "WslMirroredNetworkManager::SendInterfaceRequestToGns",
        TraceLoggingValue("ModifyGuestDeviceSettingRequest : update interface properties [queued]", "GnsMessage"),
        TraceLoggingValue(endpoint.InterfaceGuid, "InterfaceGuid"),
        TraceLoggingValue(interfaceConnected, "connected"),
        TraceLoggingValue(interfaceMtu, "mtu"),
        TraceLoggingValue(interfaceMetric, "metric"));

    int linuxResultCode{};
    // can safely capture by ref since we are waiting
    const auto hr = m_gnsCallbackQueue.submit_and_wait([&] {
        return m_callbackForGnsMessage(LxGnsMessageModifyGuestDeviceSettingRequest, ToJsonW(modifyRequest), GnsCallbackFlags::Wait, &linuxResultCode);
    });

    WSL_LOG(
        "WslMirroredNetworkManager::SendInterfaceRequestToGns",
        TraceLoggingValue("ModifyGuestDeviceSettingRequest : update interface properties [completed]", "GnsMessage"),
        TraceLoggingHResult(hr, "hr"),
        TraceLoggingValue(linuxResultCode, "linuxResultCode"));

    return hr;
}
CATCH_RETURN()

_Requires_lock_held_(m_networkLock)
_Check_return_ bool wsl::core::networking::WslMirroredNetworkManager::SyncIpStateWithLinux(NetworkEndpoint& endpoint)
{
    using hns::GuestEndpointResourceType;
    using hns::IPAddress;
    using hns::Route;
    using TrackedIpStateSyncStatus::PendingAdd;
    using TrackedIpStateSyncStatus::PendingRemoval;
    using TrackedIpStateSyncStatus::PendingUpdate;
    using TrackedIpStateSyncStatus::Synced;

    bool syncSuccessful = true;

    if (!endpoint.StateTracking->InitialSyncComplete)
    {
        // Tell GNS that we're ready to start pushing addresses and routes to Linux on this interface.
        hns::InitialIpConfigurationNotification notification{};
        notification.targetDeviceName = wsl::shared::string::GuidToString<wchar_t>(endpoint.InterfaceGuid);
        WI_SetAllFlags(
            notification.flags,
            (hns::InitialIpConfigurationNotificationFlags::SkipPrimaryRoutingTableUpdate |
             hns::InitialIpConfigurationNotificationFlags::SkipLoopbackRouteReset));

        WSL_LOG(
            "WslMirroredNetworkManager::SyncIpStateWithLinux",
            TraceLoggingValue("InitialIpConfigurationNotification [queued]", "GnsMessage"),
            TraceLoggingValue(endpoint.InterfaceGuid, "InterfaceGuid"));

        int linuxResultCode{};
        // can safely capture by ref since we are waiting
        const auto hr = m_gnsCallbackQueue.submit_and_wait([&] {
            return m_callbackForGnsMessage(
                LxGnsMessageInitialIpConfigurationNotification, ToJsonW(notification), GnsCallbackFlags::Wait, &linuxResultCode);
        });

        WSL_LOG(
            "WslMirroredNetworkManager::SyncIpStateWithLinux",
            TraceLoggingValue("InitialIpConfigurationNotification [completed]", "GnsMessage"),
            TraceLoggingHResult(hr, "hr"),
            TraceLoggingValue(linuxResultCode, "linuxResultCode"));
    }

    const auto makingIpInterfaceUpdate = endpoint.Network->PendingIPInterfaceUpdate;
    // Linux may delete routes behind us when making interface, address, and route changes
    // will track when to refresh v4 and v6 routes to ensure routes are still present after changes
    // a few customers have seen this when we update temporary v6 addresses, for example
    auto refreshAllRoutes = false;

    // First: update Linux with any interface updates
    // If IsHidden is set, then also indicate to Linux that the interface should be disconnected
    if (endpoint.Network->PendingIPInterfaceUpdate || endpoint.Network->IsHidden)
    {
        const auto originalConnectValue = endpoint.Network->IsConnected;
        if (endpoint.Network->IsHidden)
        {
            endpoint.Network->IsConnected = false;
        }

        if (FAILED(SendInterfaceRequestToGns(endpoint)))
        {
            WSL_LOG(
                "WslMirroredNetworkManager::SyncIpStateWithLinux",
                TraceLoggingValue("Failed to update Interface properties", "message"),
                TraceLoggingValue(endpoint.Network->IsConnected, "connected"),
                TraceLoggingValue(endpoint.Network->GetEffectiveMtu(), "mtu"),
                TraceLoggingValue(endpoint.Network->GetMinimumMetric(), "metric"),
                TraceLoggingValue(endpoint.InterfaceGuid, "InterfaceGuid"));

            syncSuccessful = false;
            // interfaces are in an unknown state - push route updates in case Linux deleted routes behind us
            refreshAllRoutes = true;
        }
        else
        {
            endpoint.Network->PendingIPInterfaceUpdate = false;
        }

        endpoint.Network->IsConnected = originalConnectValue;
        if (originalConnectValue)
        {
            // interface potentially just moved from disconnected -> connected
            // push route updates in case Linux deleted routes behind us
            refreshAllRoutes = true;
        }
    }

    // Second: update Linux with any addresses to remove
    auto addressIt = endpoint.StateTracking->IpAddresses.begin();
    while (addressIt != endpoint.StateTracking->IpAddresses.end())
    {
        auto address = addressIt++;

        // if the interface is hidden, we need to remove addresses
        // 'continue' to keep the address
        if (!endpoint.Network->IsHidden)
        {
            if (endpoint.Network->IpAddresses.contains(address->Address))
            {
                if (address->SyncStatus == PendingRemoval)
                {
                    // This address was slated for removal but still exists on the host.  It should be kept around instead.
                    // We'll send an update just in case.
                    address->SyncStatus = PendingUpdate;
                    address->SyncRetryCount = TrackedIpAddress::MaxSyncRetryCount;
                }
                else if (makingIpInterfaceUpdate)
                {
                    // if we pushed an interface update, ensure our addresses are up to date
                    address->SyncStatus = PendingUpdate;
                    address->SyncRetryCount = TrackedIpAddress::MaxSyncRetryCount;
                }

                continue;
            }
        }

        // We found an address that should be removed from the guest.

        if (address->SyncStatus != PendingRemoval)
        {
            // We've never attempted to remove this address before, so reset the sync retry count.
            address->SyncRetryCount = TrackedIpAddress::MaxSyncRetryCount;
        }
        address->SyncStatus = PendingRemoval;

        if (m_vmConfig.EnableHostAddressLoopback)
        {
            if (FAILED(SendLoopbackRequestToGns(endpoint, *address, hns::OperationType::Remove)))
            {
                WSL_LOG(
                    "WslMirroredNetworkManager::SyncIpStateWithLinux",
                    TraceLoggingValue("Failed to remove loopback routes for local address", "message"),
                    TraceLoggingValue(endpoint.InterfaceGuid, "InterfaceGuid"),
                    TraceLoggingValue(endpoint.Network->IsConnected, "isConnected"),
                    TraceLoggingValue(address->Address.AddressString.c_str(), "address"),
                    TraceLoggingValue(address->Address.PrefixLength, "prefixLength"));
            }
        }

        if (FAILED(SendAddressRequestToGns(endpoint, *address, hns::ModifyRequestType::Remove)))
        {
            if (address->SyncRetryCount == 0)
            {
                WSL_LOG(
                    "WslMirroredNetworkManager::SyncIpStateWithLinux",
                    TraceLoggingValue(
                        "Reached maximum retries to remove an address - we will no longer schedule the retry timer", "message"),
                    TraceLoggingValue(endpoint.InterfaceGuid, "InterfaceGuid"),
                    TraceLoggingValue(endpoint.Network->IsConnected, "isConnected"),
                    TraceLoggingValue(address->Address.AddressString.c_str(), "address"),
                    TraceLoggingValue(address->Address.PrefixLength, "prefixLength"));
            }
            else
            {
                syncSuccessful = false;
            }
        }
        else
        {
            WSL_LOG(
                "WslMirroredNetworkManager::SyncIpStateWithLinux",
                TraceLoggingValue("Address synced (removed)", "message"),
                TraceLoggingValue(endpoint.InterfaceGuid, "InterfaceGuid"),
                TraceLoggingValue(endpoint.Network->IsConnected, "isConnected"),
                TraceLoggingValue(address->Address.AddressString.c_str(), "address"),
                TraceLoggingValue(address->Address.PrefixLength, "prefixLength"));
            endpoint.StateTracking->IpAddresses.erase(address);
        }
        // push route updates in case Linux deleted routes behind us after removing addresses
        refreshAllRoutes = true;
    }

    // Third: update Linux with any routes to remove
    auto routeIt = endpoint.StateTracking->Routes.begin();
    while (routeIt != endpoint.StateTracking->Routes.end())
    {
        auto route = routeIt++;

        // if the interface is hidden, we need to remove routes
        // 'continue' to keep the route
        if (!endpoint.Network->IsHidden)
        {
            if (endpoint.Network->Routes.contains(route->Route))
            {
                if (route->SyncStatus == PendingRemoval)
                {
                    // This route was slated for removal but still exists on the host.  It should be kept around instead.
                    // We'll send an update just in case.
                    route->SyncStatus = PendingUpdate;
                    route->SyncRetryCount = TrackedRoute::MaxSyncRetryCount;
                }
                else if (makingIpInterfaceUpdate)
                {
                    // if we pushed an interface update, ensure our routes are up to date
                    route->SyncStatus = PendingUpdate;
                    route->SyncRetryCount = TrackedRoute::MaxSyncRetryCount;
                }

                continue;
            }
        }

        // We found a route that should be removed from the guest.

        if (route->SyncStatus != PendingRemoval)
        {
            // We've never attempted to remove this route before, so reset the sync retry count.
            route->SyncRetryCount = TrackedRoute::MaxSyncRetryCount;
        }
        route->SyncStatus = PendingRemoval;

        if (FAILED(SendRouteRequestToGns(endpoint, *route, hns::ModifyRequestType::Remove)))
        {
            if (route->SyncRetryCount == 0)
            {
                WSL_LOG(
                    "WslMirroredNetworkManager::SyncIpStateWithLinux",
                    TraceLoggingValue(
                        "Reached maximum retries to remove a route - we will no longer schedule the retry timer", "message"),
                    TraceLoggingValue(endpoint.InterfaceGuid, "InterfaceGuid"),
                    TraceLoggingValue(endpoint.Network->IsConnected, "isConnected"),
                    TraceLoggingValue(route->Route.DestinationPrefixString.c_str(), "destinationPrefix"),
                    TraceLoggingValue(route->Route.DestinationPrefix.PrefixLength, "prefixLength"),
                    TraceLoggingValue(route->Route.NextHopString.c_str(), "nextHop"),
                    TraceLoggingValue(route->Route.Metric, "metric"));
            }
            else
            {
                syncSuccessful = false;
            }
        }
        else
        {
            WSL_LOG(
                "WslMirroredNetworkManager::SyncIpStateWithLinux",
                TraceLoggingValue("Route synced (removed) succeeded", "message"),
                TraceLoggingValue(endpoint.InterfaceGuid, "InterfaceGuid"),
                TraceLoggingValue(endpoint.Network->IsConnected, "isConnected"),
                TraceLoggingValue(route->Route.DestinationPrefixString.c_str(), "destinationPrefix"),
                TraceLoggingValue(route->Route.DestinationPrefix.PrefixLength, "prefixLength"),
                TraceLoggingValue(route->Route.NextHopString.c_str(), "nextHop"),
                TraceLoggingValue(route->Route.Metric, "metric"));
            endpoint.StateTracking->Routes.erase(route);
        }
        // push route updates in case Linux deleted routes behind us after removing other various routes
        refreshAllRoutes = true;
    }

    // Fourth: update Linux with any addresses to add
    if (!endpoint.Network->IsHidden && endpoint.Network->IsConnected)
    {
        bool shouldRefreshAllAddresses = false;
        for (auto& hostAddress : endpoint.Network->IpAddresses)
        {
            auto trackedAddress = endpoint.StateTracking->IpAddresses.emplace(TrackedIpAddress(hostAddress)).first;
            // detect if previously sync'd addresses need to be updated
            // this addresses issues we've seen where addresses were removed from Linux without our knowledge
            shouldRefreshAllAddresses |= trackedAddress->SyncStatus == PendingAdd || trackedAddress->SyncStatus == PendingUpdate;
        }

        for (auto& trackedAddress : endpoint.StateTracking->IpAddresses)
        {
            std::optional<HRESULT> hr{};
            switch (trackedAddress.SyncStatus)
            {
            case PendingAdd:
            {
                hr = SendAddressRequestToGns(endpoint, trackedAddress, hns::ModifyRequestType::Add);
                if (FAILED(hr.value()))
                {
                    // try to update it instead if it already exists
                    hr = SendAddressRequestToGns(endpoint, trackedAddress, hns::ModifyRequestType::Update);
                }

                if (SUCCEEDED(hr.value()) && m_vmConfig.EnableHostAddressLoopback)
                {
                    // Add a special loopback route so that loopback packets flow through the host and back.
                    hr = SendLoopbackRequestToGns(endpoint, trackedAddress, hns::OperationType::Create);
                    if (FAILED(hr.value()))
                    {
                        WSL_LOG(
                            "WslMirroredNetworkManager::SyncIpStateWithLinux",
                            TraceLoggingValue("Failed to create loopback routes for local address", "message"),
                            TraceLoggingValue(wsl::core::networking::ToString(trackedAddress.SyncStatus), "AddressSyncStatus"),
                            TraceLoggingValue(endpoint.InterfaceGuid, "InterfaceGuid"),
                            TraceLoggingValue(endpoint.Network->IsConnected, "isConnected"),
                            TraceLoggingValue(trackedAddress.Address.AddressString.c_str(), "address"),
                            TraceLoggingValue(trackedAddress.Address.PrefixLength, "prefixLength"));
                    }
                }
                // push route updates in case Linux deleted routes behind us after refreshing addresses
                refreshAllRoutes = true;
                break;
            }

            case Synced:
            {
                auto fallThroughToUpdateAddress = shouldRefreshAllAddresses;

                // Check if this address needs to be updated (i.e., its PreferredLifetime / DAD state needs to be updated)
                auto hostAddress = endpoint.Network->IpAddresses.find(trackedAddress.Address);
                if (hostAddress != endpoint.Network->IpAddresses.end())
                {
                    if (trackedAddress.Address.IsPreferred() != hostAddress->IsPreferred())
                    {
                        trackedAddress.Address.PreferredLifetime = hostAddress->PreferredLifetime;
                        fallThroughToUpdateAddress = true;
                    }
                }

                if (!fallThroughToUpdateAddress)
                {
                    break;
                }

                trackedAddress.SyncStatus = PendingUpdate;
                trackedAddress.SyncRetryCount = TrackedIpAddress::MaxSyncRetryCount;
                __fallthrough;
            }

            case PendingUpdate:
                hr = SendAddressRequestToGns(endpoint, trackedAddress, hns::ModifyRequestType::Update);
                if (FAILED(hr.value()))
                {
                    // try to add it if it was removed in Linux
                    hr = SendAddressRequestToGns(endpoint, trackedAddress, hns::ModifyRequestType::Add);
                }

                if (SUCCEEDED(hr.value()) && m_vmConfig.EnableHostAddressLoopback)
                {
                    // Add a special loopback route so that loopback packets flow through the host and back.
                    hr = SendLoopbackRequestToGns(endpoint, trackedAddress, hns::OperationType::Create);
                    if (FAILED(hr.value()))
                    {
                        WSL_LOG(
                            "WslMirroredNetworkManager::SyncIpStateWithLinux",
                            TraceLoggingValue("Failed to create loopback routes for local address", "message"),
                            TraceLoggingValue(wsl::core::networking::ToString(trackedAddress.SyncStatus), "AddressSyncStatus"),
                            TraceLoggingValue(endpoint.InterfaceGuid, "InterfaceGuid"),
                            TraceLoggingValue(endpoint.Network->IsConnected, "isConnected"),
                            TraceLoggingValue(trackedAddress.Address.AddressString.c_str(), "address"),
                            TraceLoggingValue(trackedAddress.Address.PrefixLength, "prefixLength"));
                    }
                }

                // push route updates in case Linux deleted routes behind us after refreshing addresses
                refreshAllRoutes = true;
                break;

            case PendingRemoval:
                // This address is still slated for removal, which we'll try again later.
                continue;

            default:
                WI_ASSERT(false);
                continue;
            }

            if (SUCCEEDED(hr.value_or(E_FAIL)))
            {
                trackedAddress.SyncStatus = Synced;
                WSL_LOG(
                    "WslMirroredNetworkManager::SyncIpStateWithLinux",
                    TraceLoggingValue("Address synced", "message"),
                    TraceLoggingValue(endpoint.InterfaceGuid, "InterfaceGuid"),
                    TraceLoggingValue(endpoint.Network->IsConnected, "isConnected"),
                    TraceLoggingValue(trackedAddress.Address.AddressString.c_str(), "address"),
                    TraceLoggingValue(trackedAddress.Address.PrefixLength, "prefixLength"));
            }

            if (trackedAddress.SyncRetryCount == 0)
            {
                WSL_LOG(
                    "WslMirroredNetworkManager::SyncIpStateWithLinux",
                    TraceLoggingValue(
                        "Reached maximum retries to sync an address - we will no longer schedule the retry timer", "message"),
                    TraceLoggingValue(endpoint.InterfaceGuid, "InterfaceGuid"),
                    TraceLoggingValue(endpoint.Network->IsConnected, "isConnected"),
                    TraceLoggingValue(trackedAddress.Address.AddressString.c_str(), "address"),
                    TraceLoggingValue(trackedAddress.Address.PrefixLength, "prefixLength"));
            }

            syncSuccessful &= (trackedAddress.SyncStatus == Synced || trackedAddress.SyncRetryCount == 0);
        }
    }
    else
    {
        WSL_LOG(
            "WslMirroredNetworkManager::SyncIpStateWithLinux",
            TraceLoggingValue("Not adding addresses for hidden or disconnected interface", "message"),
            TraceLoggingValue(endpoint.InterfaceGuid, "InterfaceGuid"),
            TraceLoggingValue(endpoint.Network->IsHidden, "isHidden"),
            TraceLoggingValue(endpoint.Network->IsConnected, "isConnected"));
    }

    // Fourth: update Linux with any routes to add
    if (!endpoint.Network->IsHidden && endpoint.Network->IsConnected)
    {
        for (auto& hostRoute : endpoint.Network->Routes)
        {
            const auto trackedRoute = endpoint.StateTracking->Routes.emplace(TrackedRoute(hostRoute)).first;
            // detect if previously sync'd routes need to be updated
            // this addresses issues we've seen where routes were removed from Linux without our knowledge
            // and routes couldn't be updated later because required routes, like the prefix route, wasn't there
            refreshAllRoutes |= trackedRoute->SyncStatus == PendingAdd || trackedRoute->SyncStatus == PendingUpdate;
        }

        if (refreshAllRoutes)
        {
            WSL_LOG("WslMirroredNetworkManager::SyncIpStateWithLinux", TraceLoggingValue("Refreshing all routes", "message"));
        }

        for (auto& trackedRoute : endpoint.StateTracking->Routes)
        {
            std::optional<HRESULT> hr{};
            switch (trackedRoute.SyncStatus)
            {
            case PendingAdd:
                hr = SendRouteRequestToGns(endpoint, trackedRoute, hns::ModifyRequestType::Add);
                if (FAILED(hr.value()))
                {
                    // try to update it instead if it already exists
                    hr = SendRouteRequestToGns(endpoint, trackedRoute, hns::ModifyRequestType::Update);
                }
                break;

            case Synced:
                if (refreshAllRoutes)
                {
                    hr = SendRouteRequestToGns(endpoint, trackedRoute, hns::ModifyRequestType::Update);
                    if (FAILED(hr.value()))
                    {
                        // try to add it
                        hr = SendRouteRequestToGns(endpoint, trackedRoute, hns::ModifyRequestType::Add);
                    }
                    if (FAILED(hr.value()))
                    {
                        trackedRoute.SyncStatus = PendingUpdate;
                        trackedRoute.SyncRetryCount = TrackedRoute::MaxSyncRetryCount;
                    }
                }
                break;

            case PendingUpdate:
                hr = SendRouteRequestToGns(endpoint, trackedRoute, hns::ModifyRequestType::Update);
                if (FAILED(hr.value()))
                {
                    // try to add it
                    hr = SendRouteRequestToGns(endpoint, trackedRoute, hns::ModifyRequestType::Add);
                }
                break;

            case PendingRemoval:
                // This route is still slated for removal, which we'll try again later.
                continue;

            default:
                WI_ASSERT(false);
                continue;
            }

            if (SUCCEEDED(hr.value_or(E_FAIL)))
            {
                trackedRoute.SyncStatus = Synced;
                WSL_LOG(
                    "WslMirroredNetworkManager::SyncIpStateWithLinux",
                    TraceLoggingValue("Route synced", "message"),
                    TraceLoggingValue(endpoint.InterfaceGuid, "InterfaceGuid"),
                    TraceLoggingValue(endpoint.Network->IsConnected, "isConnected"),
                    TraceLoggingValue(trackedRoute.Route.DestinationPrefixString.c_str(), "destinationPrefix"),
                    TraceLoggingValue(trackedRoute.Route.DestinationPrefix.PrefixLength, "prefixLength"),
                    TraceLoggingValue(trackedRoute.Route.NextHopString.c_str(), "nextHop"),
                    TraceLoggingValue(trackedRoute.Route.Metric, "metric"));
            }

            if (trackedRoute.SyncRetryCount == 0)
            {
                WSL_LOG(
                    "WslMirroredNetworkManager::SyncIpStateWithLinux",
                    TraceLoggingValue(
                        "Reached maximum amount of retries to sync a route. This can happen if the route's next hop is not "
                        "reachable, as Linux does not allow such routes to be plumbed. Failure to sync the route will no longer "
                        "schedule the retry timer.",
                        "message"),
                    TraceLoggingValue(endpoint.InterfaceGuid, "InterfaceGuid"),
                    TraceLoggingValue(endpoint.Network->IsConnected, "isConnected"),
                    TraceLoggingValue(trackedRoute.Route.DestinationPrefixString.c_str(), "destinationPrefix"),
                    TraceLoggingValue(trackedRoute.Route.DestinationPrefix.PrefixLength, "prefixLength"),
                    TraceLoggingValue(trackedRoute.Route.NextHopString.c_str(), "nextHop"),
                    TraceLoggingValue(trackedRoute.Route.Metric, "metric"));
            }

            syncSuccessful &= (trackedRoute.SyncStatus == Synced || trackedRoute.SyncRetryCount == 0);
        }
    }
    else
    {
        WSL_LOG(
            "WslMirroredNetworkManager::SyncIpStateWithLinux",
            TraceLoggingValue("Not adding routes for hidden or disconnected interface", "message"),
            TraceLoggingValue(endpoint.InterfaceGuid, "InterfaceGuid"),
            TraceLoggingValue(endpoint.Network->IsHidden, "isHidden"),
            TraceLoggingValue(endpoint.Network->IsConnected, "isConnected"));
    }

    // Fifth: update Linux with updated DNS information
    if (m_dnsInfo != m_trackedDnsInfo)
    {
        if (FAILED(SendDnsRequestToGns(endpoint, m_dnsInfo, hns::ModifyRequestType::Update)))
        {
            syncSuccessful = false;
        }
        else
        {
            m_trackedDnsInfo = m_dnsInfo;
        }
    }

    endpoint.StateTracking->InitialSyncComplete = true;

    WSL_LOG(
        "WslMirroredNetworkManager::SyncIpStateWithLinux",
        TraceLoggingValue(endpoint.InterfaceGuid, "InterfaceGuid"),
        TraceLoggingValue(syncSuccessful, "syncSuccessful"));
    return syncSuccessful;
}

// We must determine what IP changes to push to Linux
_Requires_lock_held_(m_networkLock)
void wsl::core::networking::WslMirroredNetworkManager::UpdateAllEndpointsImpl(UpdateEndpointFlag updateFlag, _In_ PCSTR callingSource) noexcept
try
{
    static long s_updateAllEndpointsCounter = 0;
    const auto instanceCounter = InterlockedIncrement(&s_updateAllEndpointsCounter);

    if (updateFlag == UpdateEndpointFlag::None)
    {
        WSL_LOG(
            "WslMirroredNetworkManager::UpdateAllEndpointsImpl",
            TraceLoggingValue(instanceCounter, "instanceCounter"),
            TraceLoggingValue(callingSource, "callingSource"),
            TraceLoggingValue("None [exiting early]", "updateFlag"));
        return;
    }

    if (updateFlag == UpdateEndpointFlag::Default)
    {
        const auto currentTickCount = GetTickCount64();
        const auto timeFromLastUpdate = currentTickCount - m_lastUpdateAllEndpointsDefaultTime;

        if (timeFromLastUpdate >= m_debounceUpdateAllEndpointsTimerMs)
        {
            // It's been >= m_debounceUpdateAllEndpointsTimerMs since we last attempted an update, so go ahead and process it.
            WSL_LOG(
                "WslMirroredNetworkManager::UpdateAllEndpointsImpl",
                TraceLoggingValue(instanceCounter, "instanceCounter"),
                TraceLoggingValue(callingSource, "callingSource"),
                TraceLoggingValue(wsl::core::networking::ToString(updateFlag), "updateFlag"),
                TraceLoggingValue("Debounce time reset - continuing update", "state"),
                TraceLoggingValue(timeFromLastUpdate, "timeFromLastUpdate"),
                TraceLoggingValue(m_debounceUpdateAllEndpointsTimerMs, "m_debounceUpdateAllEndpointsTimerMs"));
            m_lastUpdateAllEndpointsDefaultTime = currentTickCount;
        }
        else if (!m_IsDebounceUpdateAllEndpointsDefaultTimerSet)
        {
            // The debounce timer is not already scheduled, so schedule it.
            WSL_LOG(
                "WslMirroredNetworkManager::UpdateAllEndpointsImpl",
                TraceLoggingValue(instanceCounter, "instanceCounter"),
                TraceLoggingValue(callingSource, "callingSource"),
                TraceLoggingValue(wsl::core::networking::ToString(updateFlag), "updateFlag"),
                TraceLoggingValue("Debouncing Notification - setting timer", "state"),
                TraceLoggingValue(timeFromLastUpdate, "timeFromLastUpdate"),
                TraceLoggingValue(m_debounceUpdateAllEndpointsTimerMs, "m_debounceUpdateAllEndpointsTimerMs"));

            // Set the due time just past the debounce timer duration, relative to the last update time.
            m_IsDebounceUpdateAllEndpointsDefaultTimerSet = true;
            FILETIME dueTime = wil::filetime::from_int64(static_cast<ULONGLONG>(
                -1 * (wil::filetime_duration::one_millisecond * (20 + m_debounceUpdateAllEndpointsTimerMs - timeFromLastUpdate))));
            SetThreadpoolTimer(m_debounceUpdateAllEndpointsDefaultTimer.get(), &dueTime, 0, 0);
            return;
        }
        else
        {
            // The debounce timer is already scheduled, so ignore this update.
            WSL_LOG(
                "WslMirroredNetworkManager::UpdateAllEndpointsImpl",
                TraceLoggingValue(instanceCounter, "instanceCounter"),
                TraceLoggingValue(callingSource, "callingSource"),
                TraceLoggingValue(wsl::core::networking::ToString(updateFlag), "updateFlag"),
                TraceLoggingValue("Debouncing Notification - timer already set", "state"),
                TraceLoggingValue(timeFromLastUpdate, "timeFromLastUpdate"),
                TraceLoggingValue(m_debounceUpdateAllEndpointsTimerMs, "m_debounceUpdateAllEndpointsTimerMs"));
            return;
        }
    }
    else
    {
        WSL_LOG(
            "WslMirroredNetworkManager::UpdateAllEndpointsImpl",
            TraceLoggingValue(instanceCounter, "instanceCounter"),
            TraceLoggingValue(callingSource, "callingSource"),
            TraceLoggingValue(wsl::core::networking::ToString(updateFlag), "updateFlag"));
    }

    m_latestHnsStatus = HnsStatus::NetworkConnectedWithHnsNotification;

    // Update IP properties on all interfaces on the host
    // N.B. We must process the DisableDefaultRoutes property of each host interface before we
    // process the host routes, as this property might impact the set of routes we choose to mirror.
    ProcessConnectivityChange();
    ProcessInterfaceChange();
    ProcessIpAddressChange();
    ProcessRouteChange();
    ProcessDNSChange();

    // Push IP state to Linux
    bool syncSuccessful = true;
    std::set<GUID, wsl::windows::common::helpers::GuidLess> mirroredConnectedInterfaces;
    for (auto& endpoint : m_networkEndpoints)
    {
        if (IsInterfaceIndexOfGelnic(endpoint.Network->InterfaceIndex))
        {
            continue;
        }

        // there may be more mirrored interfaces than 'host-connected' interfaces
        // e.g. network adapters which are disconnected or hidden
        // track all host-connected interfaces that have been successfully mirrored
        if (m_hostConnectedInterfaces.contains(endpoint.InterfaceGuid))
        {
            if (endpoint.Network->IsHidden)
            {
                WSL_LOG(
                    "WslMirroredNetworkManager::UpdateAllEndpointsImpl",
                    TraceLoggingValue(instanceCounter, "instanceCounter"),
                    TraceLoggingValue(endpoint.InterfaceGuid, "interfaceGuid"),
                    TraceLoggingValue(
                        "Resetting IsHidden to false and PendingIPInterfaceUpdate to true to update the Interface", "message"));
                endpoint.Network->IsHidden = false;
                // setting PendingIPInterfaceUpdate to tell SyncIpStateWithLinux to update the Interface state
                endpoint.Network->PendingIPInterfaceUpdate = true;
            }
            mirroredConnectedInterfaces.insert(endpoint.InterfaceGuid);
        }
        else
        {
            // if the host has hidden the interface that was mirrored by HNS
            // ensure the interface is not connected in Linux
            // we are deliberately overriding the endpoint state in this case
            WSL_LOG(
                "WslMirroredNetworkManager::UpdateAllEndpointsImpl",
                TraceLoggingValue(instanceCounter, "instanceCounter"),
                TraceLoggingValue(endpoint.InterfaceGuid, "interfaceGuid"),
                TraceLoggingValue(
                    "Setting IsHidden to true - this interface is hidden on the host and must not be connected in the container",
                    "message"));
            endpoint.Network->IsHidden = true;
            // setting PendingIPInterfaceUpdate to tell SyncIpStateWithLinux to update the Interface state
            endpoint.Network->PendingIPInterfaceUpdate = true;
        }

        if (!SyncIpStateWithLinux(endpoint))
        {
            // We failed to sync some bit of state.  Let's schedule a timer to try again in a bit.
            WSL_LOG(
                "WslMirroredNetworkManager::UpdateAllEndpointsImpl",
                TraceLoggingValue(instanceCounter, "instanceCounter"),
                TraceLoggingValue(endpoint.InterfaceGuid, "interfaceGuid"),
                TraceLoggingValue("Some IP state did not sync with Linux - scheduling a retry attempt", "message"),
                TraceLoggingValue(m_linuxIpStateRetryDebounceTimerMilliseconds, "m_linuxIpStateRetryDebounceTimerMilliseconds"));

            FILETIME dueTime = wil::filetime::from_int64(
                static_cast<ULONGLONG>(-1 * wil::filetime_duration::one_millisecond * m_linuxIpStateRetryDebounceTimerMilliseconds));

            SetThreadpoolTimer(m_retryLinuxIpStateSyncTimer.get(), &dueTime, 0, 1000);

            if (syncSuccessful)
            {
                // set to false the first pass through the for loop
                syncSuccessful = false;

                // Increase the IP state retry timer according to exponential back-off, capping at a maximum value.
                m_linuxIpStateRetryDebounceTimerMilliseconds =
                    std::min(m_linuxIpStateRetryDebounceTimerMilliseconds * 2, m_linuxIpStateRetryDebounceTimerMaxMilliseconds);
            }
        }
    }

    // If all of the following occurs, then we have entered the goal state.
    // 1) Mirrored all usable host interfaces
    // 2) Successfully sync'd all settings on those interfaces
    // 3) Not currently in the goal state
    if (syncSuccessful)
    {
        // Reset the IP state retry timer back to the minimum value.
        m_linuxIpStateRetryDebounceTimerMilliseconds = m_linuxIpStateRetryDebounceTimerMinMilliseconds;

        // if any host-connected interfaces are not yet mirrored, don't indicate we are in sync
        bool hnsMirroredInSyncWithHost = mirroredConnectedInterfaces == m_hostConnectedInterfaces;
        if (mirroredConnectedInterfaces != m_hostConnectedInterfaces)
        {
            // mirroredConnectedInterfaces won't equal m_hostConnectedInterfaces when:
            // - there are hidden host interfaces
            //   i.e., interfaces are in m_networkEndpoints but not in m_hostConnectedInterfaces
            // - when HNS hasn't yet mirrored a connected host interface
            //   i.e. interfaces are in m_hostConnectedInterfaces but not in m_networkEndpoints
            //
            // if HNS has not yet mirrored a host interface, we should not indicate we are in sync
            // but if hidden interfaces should not block being in sync

            // reset to true until we see if HNS hasn't yet mirrored a connected host interface
            hnsMirroredInSyncWithHost = true;
            // verify that HNS has mirrored all host-connected interfaces
            for (const auto& connectedHostInterface : m_hostConnectedInterfaces)
            {
                bool interfaceMatched = false;
                for (const auto& hnsEndpoint : m_networkEndpoints)
                {
                    if (connectedHostInterface == hnsEndpoint.InterfaceGuid)
                    {
                        interfaceMatched = true;
                        break;
                    }
                }

                if (!interfaceMatched)
                {
                    WSL_LOG(
                        "WslMirroredNetworkManager::UpdateAllEndpointsImpl",
                        TraceLoggingValue(instanceCounter, "instanceCounter"),
                        TraceLoggingValue("HNS has not yet mirrored a host connected Interface", "message"),
                        TraceLoggingValue(connectedHostInterface, "interfaceGuid"));
                    hnsMirroredInSyncWithHost = false;
                }
            }
        }

        if (hnsMirroredInSyncWithHost && !m_inMirroredGoalState.is_signaled())
        {
            WSL_LOG(
                "WslMirroredNetworkManager::UpdateAllEndpointsImpl",
                TraceLoggingValue(instanceCounter, "instanceCounter"),
                TraceLoggingValue("Reached goal state", "message"));
            m_inMirroredGoalState.SetEvent();

            // Telemetry to see how long it takes to reach the mirrored goal state for the first time.
            if (std::chrono::duration_cast<std::chrono::milliseconds>(m_initialMirroredGoalStateEndTime.time_since_epoch()) ==
                std::chrono::milliseconds::zero())
            {
                m_initialMirroredGoalStateEndTime = std::chrono::steady_clock::now();

                const auto waitTime = m_initialMirroredGoalStateEndTime - m_objectCreationTime;
                WSL_LOG(
                    "WslMirroringInitialGoalStateWait",
                    TraceLoggingValue((std::chrono::duration_cast<std::chrono::milliseconds>(waitTime)).count(), "waitTimeMs"),
                    TraceLoggingValue(m_vmConfig.EnableDnsTunneling, "DnsTunnelingEnabled"),
                    TraceLoggingValue(m_vmConfig.FirewallConfig.Enabled(), "HyperVFirewallEnabled"),
                    TraceLoggingValue(m_vmConfig.EnableAutoProxy, "AutoProxyFeatureEnabled")); // the feature is enabled, but we don't know if proxy settings are actually configured
            }
        }
    }
}
CATCH_LOG()

_Check_return_ HRESULT wsl::core::networking::WslMirroredNetworkManager::EnumerateNetworks(_Out_ std::vector<GUID>& NetworkIds) const noexcept
try
{
    auto lock = m_networkLock.lock_shared();
    WI_ASSERT(m_state == State::Started);
    if (m_state == State::Stopped)
    {
        return E_ABORT;
    }

    NetworkIds = EnumerateMirroredNetworks();
    return S_OK;
}
CATCH_RETURN()

void wsl::core::networking::WslMirroredNetworkManager::AddEndpoint(NetworkEndpoint&& newEndpoint, hns::HNSEndpoint&& endpointProperties) noexcept
{
    const auto lock = m_networkLock.lock_exclusive();
    if (m_state == State::Stopped)
    {
        return;
    }

    constexpr uint32_t defaultRetryCount = 0ul;
    AddEndpointImpl({std::move(newEndpoint), std::move(endpointProperties), defaultRetryCount});
}

_Requires_lock_held_(m_networkLock)
void wsl::core::networking::WslMirroredNetworkManager::AddEndpointImpl(EndpointTracking&& endpointTrackingObject) noexcept
{
    PCSTR executionStep = "";
    try
    {
        // Hot-add the network endpoint to the utility VM.
        hcs::NetworkAdapter networkEndpoint{};

        networkEndpoint.MacAddress = wsl::shared::string::ParseMacAddress(endpointTrackingObject.m_hnsEndpoint.MacAddress);

        // Set the instance id to the mirrored interfaceGuid so HNS -> netvsc can optimally use the same vmNIC constructs when the InterfaceGuid is the same
        hcs::ModifySettingRequest<hcs::NetworkAdapter> addEndpointRequest{};
        addEndpointRequest.ResourcePath =
            c_networkAdapterPrefix + wsl::shared::string::GuidToString<wchar_t>(endpointTrackingObject.m_networkEndpoint.InterfaceGuid);
        addEndpointRequest.RequestType = hcs::ModifyRequestType::Add;
        addEndpointRequest.Settings.EndpointId = endpointTrackingObject.m_hnsEndpoint.ID;
        addEndpointRequest.Settings.InstanceId = endpointTrackingObject.m_networkEndpoint.InterfaceGuid;

        addEndpointRequest.Settings.MacAddress = wsl::shared::string::ParseMacAddress(endpointTrackingObject.m_hnsEndpoint.MacAddress);
        auto addEndpointRequestString = wsl::shared::ToJsonW(addEndpointRequest);

        WSL_LOG(
            "WslMirroredNetworkManager::AddEndpoint [Creating HCS endpoint]",
            TraceLoggingValue(addEndpointRequestString.c_str(), "networkRequestString"));

        executionStep = "AddHcsEndpoint";
        auto hr = m_hnsQueue.submit_and_wait([&] {
            // RetryWithTimeout throws if fails every attempt - which is caught and returned by m_gnsMessageQueue
            auto retryCount = 0ul;
            return wsl::shared::retry::RetryWithTimeout<HRESULT>(
                [&] {
                    const auto retryHr = wil::ResultFromException(
                        [&] { wsl::windows::common::hcs::ModifyComputeSystem(m_hcsSystem, addEndpointRequestString.c_str()); });

                    WSL_LOG(
                        "WslMirroredNetworkManager::AddEndpoint [ModifyComputeSystem(ModifyRequestType::Add)]",
                        TraceLoggingValue(endpointTrackingObject.m_hnsEndpoint.ID, "endpointId"),
                        TraceLoggingValue(endpointTrackingObject.m_networkEndpoint.InterfaceGuid, "instanceId"),
                        TraceLoggingValue(retryHr, "retryHr"),
                        TraceLoggingValue(retryCount, "retryCount"));

                    ++retryCount;
                    return THROW_IF_FAILED(retryHr);
                },
                wsl::core::networking::AddEndpointRetryPeriod,
                wsl::core::networking::AddEndpointRetryTimeout,
                wsl::core::networking::AddEndpointRetryPredicate);
        });

        if (hr == HCN_E_ENDPOINT_ALREADY_ATTACHED)
        {
            WSL_LOG(
                "WslMirroredNetworkManager::AddEndpoint [Adding the endpoint returned HCN_E_ENDPOINT_ALREADY_ATTACHED - "
                "continuing]",
                TraceLoggingValue(endpointTrackingObject.m_hnsEndpoint.ID, "endpointId"));

            hr = S_OK;
        }
        else if (FAILED(hr))
        {
            THROW_HR(hr);
        }

        auto removeEndpointOnError = wil::scope_exit([&] {
            // try to delete the endpoint in HCS if anything failed
            // Set the instance id to the mirrored interfaceGuid so HNS -> netvsc can optimally use the same vmNIC constructs when the InterfaceGuid is the same
            hcs::ModifySettingRequest<hcs::NetworkAdapter> networkRequest{};
            networkRequest.ResourcePath =
                c_networkAdapterPrefix + wsl::shared::string::GuidToString<wchar_t>(endpointTrackingObject.m_networkEndpoint.InterfaceGuid);
            networkRequest.RequestType = hcs::ModifyRequestType::Remove;
            networkRequest.Settings.EndpointId = endpointTrackingObject.m_hnsEndpoint.ID;
            networkRequest.Settings.InstanceId = endpointTrackingObject.m_networkEndpoint.InterfaceGuid;

            const auto networkRequestString = wsl::shared::ToJsonW(std::move(networkRequest));

            // capturing by ref because we wait for the workitem to complete
            const auto modifyResult = m_hnsQueue.submit_and_wait([&] {
                windows::common::hcs::ModifyComputeSystem(m_hcsSystem, networkRequestString.c_str());
                return S_OK; // ModifyComputeSystem throws errors, caught by m_gnsMessageQueue
            });
            WSL_LOG(
                "WslMirroredNetworkManager::AddEndpoint [Removing the HCS mirrored endpoint after failure to Add]",
                TraceLoggingHResult(modifyResult, "hr"),
                TraceLoggingValue(endpointTrackingObject.m_hnsEndpoint.ID, "endpointId"),
                TraceLoggingValue(endpointTrackingObject.m_networkEndpoint.InterfaceGuid, "instanceId"));

            if (FAILED(modifyResult))
            {
                WSL_LOG(
                    "AddMirroredEndpointFailed",
                    TraceLoggingHResult(modifyResult, "result"),
                    TraceLoggingValue(endpointTrackingObject.m_networkEndpoint.InterfaceGuid, "InterfaceGuid"),
                    TraceLoggingValue(
                        endpointTrackingObject.m_networkEndpoint.Network ? endpointTrackingObject.m_networkEndpoint.Network->InterfaceType : 0,
                        "InterfaceType"),
                    TraceLoggingValue("RemoveHcsEndpointOnFailure", "executionStep"),
                    TraceLoggingValue(m_vmConfig.EnableDnsTunneling, "DnsTunnelingEnabled"),
                    TraceLoggingValue(m_vmConfig.FirewallConfig.Enabled(), "HyperVFirewallEnabled"),
                    TraceLoggingValue(m_vmConfig.EnableAutoProxy, "AutoProxyFeatureEnabled"), // the feature is enabled, but we don't know if proxy settings are actually configured
                    TraceLoggingValue(endpointTrackingObject.m_retryCount, "retryCount"));
            }

            // Inform the parent class to remove the endpoint object from GNS registration since we couldn't add the endpoint
        });

        // Refreshing the endpoint causes it to reach the GNSInterfaceState::Synchronized state in HNS
        // which is required to receive notifications.
        // When HcnModifyEndpoint returns, all GNS notifications have been processed and the interface is fully configured.
        hns::ModifyGuestEndpointSettingRequest<void> refreshRequest{};
        refreshRequest.RequestType = hns::ModifyRequestType::Refresh;
        refreshRequest.ResourceType = hns::GuestEndpointResourceType::Port;

        const auto refreshEndpointRequestString = ToJsonW(refreshRequest);
        WSL_LOG(
            "WslMirroredNetworkManager::AddEndpoint [Synchronizing HNS state]",
            TraceLoggingValue(endpointTrackingObject.m_hnsEndpoint.ID, "endpointId"),
            TraceLoggingValue(endpointTrackingObject.m_networkEndpoint.InterfaceGuid, "instanceId"));

        executionStep = "RefreshHcsEndpoint";
        THROW_IF_FAILED(m_hnsQueue.submit_and_wait([&] {
            // Don't retry if HcnModifyEndpoint fails with HCN_E_ENDPOINT_NOT_FOUND which indicates that the underlying network object was deleted.
            constexpr auto retryPredicate = [] { return wil::ResultFromCaughtException() != HCN_E_ENDPOINT_NOT_FOUND; };
            auto retryCount = 0ul;
            // RetryWithTimeout throws if fails every attempt - which is caught and returned by m_hnsQueue
            return wsl::shared::retry::RetryWithTimeout<HRESULT>(
                [&] {
                    wil::unique_cotaskmem_string error;
                    const auto retryHr = HcnModifyEndpoint(
                        endpointTrackingObject.m_networkEndpoint.Endpoint.get(), refreshEndpointRequestString.c_str(), &error);

                    WSL_LOG(
                        "WslMirroredNetworkManager::AddEndpoint [HcnModifyEndpoint(ModifyRequestType::Refresh)]",
                        TraceLoggingValue(endpointTrackingObject.m_networkEndpoint.EndpointId, "endpointId"),
                        TraceLoggingValue(endpointTrackingObject.m_networkEndpoint.InterfaceGuid, "instanceId"),
                        TraceLoggingValue(refreshEndpointRequestString.c_str(), "json"),
                        TraceLoggingHResult(retryHr, "retryHr"),
                        TraceLoggingValue(error.is_valid() ? error.get() : L"", "errorString"),
                        TraceLoggingValue(retryCount, "retryCount"));

                    ++retryCount;
                    return THROW_IF_FAILED(retryHr);
                },
                wsl::core::networking::AddEndpointRetryPeriod,
                wsl::core::networking::AddEndpointRetryTimeout,
                retryPredicate);
        }));

        // Notify GNS of the new adapter
        hns::VmNicCreatedNotification newAdapterNotification;
        // Set the adapterId == instanceId of the created Endpoint == the mirrored interfaceGuid
        newAdapterNotification.adapterId = endpointTrackingObject.m_networkEndpoint.InterfaceGuid;

        constexpr auto type = GnsMessageType(newAdapterNotification);
        const auto jsonString = ToJsonW(newAdapterNotification);
        WSL_LOG(
            "WslMirroredNetworkManager::AddEndpoint",
            TraceLoggingValue("VmNicCreatedNotification [queued]", "GnsMessage"),
            TraceLoggingValue(endpointTrackingObject.m_networkEndpoint.InterfaceGuid, "adapterId"),
            TraceLoggingValue(endpointTrackingObject.m_networkEndpoint.NetworkId, "networkId"),
            TraceLoggingValue(endpointTrackingObject.m_networkEndpoint.EndpointId, "endpointId"),
            TraceLoggingValue(endpointTrackingObject.m_networkEndpoint.InterfaceGuid, "interfaceGuid"),
            TraceLoggingValue(endpointTrackingObject.m_networkEndpoint.Network->InterfaceIndex, "interfaceIndex"),
            TraceLoggingValue(endpointTrackingObject.m_networkEndpoint.Network->InterfaceType, "interfaceType"),
            TraceLoggingValue(jsonString.c_str(), "jsonString"));

        int linuxResultCode{};
        // can safely capture by ref since we are waiting
        hr = m_gnsCallbackQueue.submit_and_wait(
            [&] { return m_callbackForGnsMessage(type, jsonString, GnsCallbackFlags::Wait, &linuxResultCode); });
        WSL_LOG(
            "WslMirroredNetworkManager::AddEndpoint",
            TraceLoggingValue("VmNicCreatedNotification [completed]", "GnsMessage"),
            TraceLoggingHResult(hr, "hr"),
            TraceLoggingValue(linuxResultCode, "linuxResultCode"));

        // Send the endpoint state (link status) to gns.
        // Also set the loopback device name to allow configuration by name.
        //
        // Temporarily set endpoint ID and PortFriendlyName to what LxGnsMessageInterfaceConfiguration expects.
        GUID originalEndpointId = endpointTrackingObject.m_hnsEndpoint.ID;
        std::wstring originalPortFriendlyName = endpointTrackingObject.m_hnsEndpoint.PortFriendlyName;
        endpointTrackingObject.m_hnsEndpoint.ID = endpointTrackingObject.m_networkEndpoint.InterfaceGuid;
        if (IsInterfaceIndexOfGelnic(endpointTrackingObject.m_networkEndpoint.Network->InterfaceIndex))
        {
            endpointTrackingObject.m_hnsEndpoint.PortFriendlyName = c_loopbackDeviceName;
        }
        WI_ASSERT(endpointTrackingObject.m_hnsEndpoint.IPAddress.empty());

        executionStep = "SendEndpointStateToGns";
        linuxResultCode = {};
        // can safely capture by ref since we are waiting
        hr = m_gnsCallbackQueue.submit_and_wait([&] {
            return m_callbackForGnsMessage(
                LxGnsMessageInterfaceConfiguration, ToJsonW(endpointTrackingObject.m_hnsEndpoint), GnsCallbackFlags::Wait, &linuxResultCode);
        });
        // restore the Endpoint ID GUID and PortFriendlyName
        endpointTrackingObject.m_hnsEndpoint.ID = originalEndpointId;
        endpointTrackingObject.m_hnsEndpoint.PortFriendlyName = originalPortFriendlyName;
        WSL_LOG(
            "WslMirroredNetworkManager::AddEndpoint [Update link status]",
            TraceLoggingHResult(hr, "hr"),
            TraceLoggingValue(linuxResultCode, "linuxResultCode"),
            TraceLoggingValue(endpointTrackingObject.m_hnsEndpoint.ID, "endpointId"),
            TraceLoggingValue(endpointTrackingObject.m_networkEndpoint.InterfaceGuid, "instanceId"),
            TraceLoggingValue(endpointTrackingObject.m_hnsEndpoint.PortFriendlyName.c_str(), "PortFriendlyName"));
        THROW_IF_FAILED(hr);

        endpointTrackingObject.m_networkEndpoint.Network->MacAddress = endpointTrackingObject.m_hnsEndpoint.MacAddress;

        if (IsInterfaceIndexOfGelnic(endpointTrackingObject.m_networkEndpoint.Network->InterfaceIndex))
        {
            // Create loopback device in the container which will also set up loopback communication with the host.
            hns::CreateDeviceRequest createLoopbackDevice;
            createLoopbackDevice.deviceName = c_loopbackDeviceName;
            createLoopbackDevice.type = hns::DeviceType::Loopback;
            // Set the lowerEdgeAdapterId == the InstanceId of the Endpoint == the mirrored interfaceGuid
            createLoopbackDevice.lowerEdgeAdapterId = endpointTrackingObject.m_networkEndpoint.InterfaceGuid;

            WSL_LOG(
                "WslMirroredNetworkManager::AddEndpoint",
                TraceLoggingValue("CreateDeviceRequest - loopback [queued]", "GnsMessage"),
                TraceLoggingValue(c_loopbackDeviceName, "deviceName"),
                TraceLoggingValue(endpointTrackingObject.m_networkEndpoint.InterfaceGuid, "lowerEdgeAdapterId"),
                TraceLoggingValue(endpointTrackingObject.m_networkEndpoint.EndpointId, "endpointId"),
                TraceLoggingValue(endpointTrackingObject.m_networkEndpoint.InterfaceGuid, "interfaceGuid"));
            constexpr auto gnsMessageType = GnsMessageType(createLoopbackDevice);

            linuxResultCode = {};
            // can safely capture by ref since we are waiting
            hr = m_gnsCallbackQueue.submit_and_wait([&] {
                return m_callbackForGnsMessage(gnsMessageType, ToJsonW(createLoopbackDevice), GnsCallbackFlags::Wait, &linuxResultCode);
            });
            WSL_LOG(
                "WslMirroredNetworkManager::AddEndpoint",
                TraceLoggingValue("CreateDeviceRequest - loopback [completed]", "GnsMessage"),
                TraceLoggingHResult(hr, "hr"),
                TraceLoggingValue(linuxResultCode, "linuxResultCode"));
        }
        else
        {
            // Perform per-interface configuration of net filter rules.
            hns::InterfaceNetFilterRequest interfaceNetFilterRequest;
            interfaceNetFilterRequest.targetDeviceName =
                wsl::shared::string::GuidToString<wchar_t>(endpointTrackingObject.m_networkEndpoint.InterfaceGuid);
            interfaceNetFilterRequest.operation = hns::OperationType::Create;
            interfaceNetFilterRequest.ephemeralPortRangeStart = m_ephemeralPortRange.first;
            interfaceNetFilterRequest.ephemeralPortRangeEnd = m_ephemeralPortRange.second;

            linuxResultCode = {};
            // can safely capture by ref since we are waiting
            hr = m_gnsCallbackQueue.submit_and_wait([&] {
                return m_callbackForGnsMessage(
                    LxGnsMessageInterfaceNetFilter, ToJsonW(interfaceNetFilterRequest), GnsCallbackFlags::Wait, &linuxResultCode);
            });
            LOG_IF_FAILED(hr);
            WSL_LOG(
                "WslMirroredNetworkManager::AddEndpoint [InterfaceNetFilterRequest]",
                TraceLoggingHResult(hr, "hr"),
                TraceLoggingValue(linuxResultCode, "linuxResultCode"),
                TraceLoggingValue(endpointTrackingObject.m_networkEndpoint.EndpointId, "endpointId"),
                TraceLoggingValue(endpointTrackingObject.m_networkEndpoint.InterfaceGuid, "interfaceGuid"),
                TraceLoggingValue(m_ephemeralPortRange.first, "ephemeralPortRangeStart"),
                TraceLoggingValue(m_ephemeralPortRange.second, "ephemeralPortRangeEnd"));
        }

        // WSL will track state for every endpoint (interface)
        endpointTrackingObject.m_networkEndpoint.StateTracking.emplace(m_vmConfig.FirewallConfig.VmCreatorId);
        endpointTrackingObject.m_networkEndpoint.StateTracking->SeedInitialState(*endpointTrackingObject.m_networkEndpoint.Network);

        m_networkEndpoints.emplace_back(std::move(endpointTrackingObject.m_networkEndpoint));

        // successfully tracked the added endpoint - release the scope guards
        removeEndpointOnError.release();

        // after added, we must determine what is the preferred interface to indicate to bond to connect
        UpdateAllEndpointsImpl(UpdateEndpointFlag::Default, "AddEndpoint");

        WSL_LOG(
            "CreateMirroredEndpointEnd",
            TraceLoggingHResult(S_OK, "result"),
            TraceLoggingValue(endpointTrackingObject.m_networkEndpoint.InterfaceGuid, "InterfaceGuid"),
            TraceLoggingValue(
                endpointTrackingObject.m_networkEndpoint.Network ? endpointTrackingObject.m_networkEndpoint.Network->InterfaceType : 0,
                "InterfaceType"),
            TraceLoggingValue(m_vmConfig.EnableDnsTunneling, "DnsTunnelingEnabled"),
            TraceLoggingValue(m_vmConfig.FirewallConfig.Enabled(), "HyperVFirewallEnabled"),
            TraceLoggingValue(m_vmConfig.EnableAutoProxy, "AutoProxyFeatureEnabled"), // the feature is enabled, but we don't know if proxy settings are actually configured
            TraceLoggingValue(endpointTrackingObject.m_retryCount, "retryCount"));
    }
    catch (...)
    {
        const auto hr = wil::ResultFromCaughtException();

        WSL_LOG(
            "AddMirroredEndpointFailed",
            TraceLoggingHResult(hr, "result"),
            TraceLoggingValue(endpointTrackingObject.m_networkEndpoint.InterfaceGuid, "InterfaceGuid"),
            TraceLoggingValue(
                endpointTrackingObject.m_networkEndpoint.Network ? endpointTrackingObject.m_networkEndpoint.Network->InterfaceType : 0,
                "InterfaceType"),
            TraceLoggingValue(executionStep, "executionStep"),
            TraceLoggingValue(m_vmConfig.EnableDnsTunneling, "DnsTunnelingEnabled"),
            TraceLoggingValue(m_vmConfig.FirewallConfig.Enabled(), "HyperVFirewallEnabled"),
            TraceLoggingValue(m_vmConfig.EnableAutoProxy, "AutoProxyFeatureEnabled"), // the feature is enabled, but we don't know if proxy settings are actually configured
            TraceLoggingValue(endpointTrackingObject.m_retryCount, "retryCount"));

        if (hr == HCN_E_ENDPOINT_NOT_FOUND)
        {
            WSL_LOG(
                "WslMirroredNetworkManager::AddEndpoint",
                TraceLoggingValue("HCN/HCS returned HCN_E_ENDPOINT_NOT_FOUND - not retrying", "GnsMessage"),
                TraceLoggingValue(endpointTrackingObject.m_networkEndpoint.NetworkId, "networkId"),
                TraceLoggingValue(endpointTrackingObject.m_networkEndpoint.EndpointId, "endpointId"),
                TraceLoggingValue(endpointTrackingObject.m_networkEndpoint.InterfaceGuid, "interfaceGuid"),
                TraceLoggingHResult(hr, "hr"));
            return;
        }

        try
        {
            ++endpointTrackingObject.m_retryCount;

            if (endpointTrackingObject.m_retryCount > m_maxAddEndpointRetryCount)
            {
                WSL_LOG(
                    "BlockedNetworkEndpoint",
                    TraceLoggingValue("WslMirroredNetworkManager::AddEndpoint", "where"),
                    TraceLoggingHResult(hr, "result"),
                    TraceLoggingValue(executionStep, "executionStep"),
                    TraceLoggingValue(endpointTrackingObject.m_networkEndpoint.InterfaceGuid, "InterfaceGuid"),
                    TraceLoggingValue(
                        endpointTrackingObject.m_networkEndpoint.Network ? endpointTrackingObject.m_networkEndpoint.Network->InterfaceType : 0,
                        "InterfaceType"),
                    TraceLoggingValue(m_vmConfig.EnableDnsTunneling, "DnsTunnelingEnabled"),
                    TraceLoggingValue(m_vmConfig.FirewallConfig.Enabled(), "HyperVFirewallEnabled"),
                    TraceLoggingValue(m_vmConfig.EnableAutoProxy, "AutoProxyFeatureEnabled")); // the feature is enabled, but we don't know if proxy settings are actually configured

                // we now need to guarantee that Update* gets called again - but we can't do it from this thread
                // will update our debounce-timer to fire soon to invoke Update - which will trigger the Blocked* path
                // since we are now blocked on this interface
                FILETIME dueTime = wil::filetime::from_int64(
                    static_cast<ULONGLONG>(-1 * wil::filetime_duration::one_millisecond * m_debounceUpdateAllEndpointsTimerMs));
                SetThreadpoolTimer(m_debounceUpdateAllEndpointsDefaultTimer.get(), &dueTime, 0, 0);
                return;
            }

            m_failedEndpointProperties.emplace_back(
                std::move(endpointTrackingObject.m_networkEndpoint),
                std::move(endpointTrackingObject.m_hnsEndpoint),
                endpointTrackingObject.m_retryCount);

            FILETIME dueTime = wil::filetime::from_int64(
                static_cast<ULONGLONG>(-1 * wil::filetime_duration::one_millisecond * m_debounceCreateEndpointFailureTimerMs));
            SetThreadpoolTimer(m_debounceCreateEndpointFailureTimer.get(), &dueTime, 0, 0);
        }
        CATCH_LOG()
    }
}

_Requires_lock_held_(m_networkLock)
_Check_return_ HRESULT wsl::core::networking::WslMirroredNetworkManager::RemoveEndpoint(const GUID& endpointId) noexcept
try
{
    const auto removedFailedEndpointCount = std::erase_if(m_failedEndpointProperties, [&](const auto& endpointTracking) {
        return endpointTracking.m_networkEndpoint.EndpointId == endpointId;
    });
    if (removedFailedEndpointCount > 0)
    {
        WSL_LOG(
            "WslMirroredNetworkManager::RemoveEndpoint - Endpoint removed from m_failedEndpointProperties",
            TraceLoggingValue(endpointId, "endpointId"));
    }

    WSL_LOG("WslMirroredNetworkManager::RemoveEndpoint", TraceLoggingValue(endpointId, "endpointId"));

    std::vector<NetworkEndpoint>::const_iterator foundEndpoint;

    try
    {
        foundEndpoint = std::find_if(m_networkEndpoints.cbegin(), m_networkEndpoints.cend(), [&](const auto& endpoint) {
            return endpoint.EndpointId == endpointId;
        });

        if (foundEndpoint == m_networkEndpoints.cend())
        {
            WSL_LOG("WslMirroredNetworkManager::RemoveEndpoint - Endpoint not found", TraceLoggingValue(endpointId, "endpointId"));
            return S_OK;
        }

        // Perform per-interface configuration of net filter rules.
        hns::InterfaceNetFilterRequest interfaceNetFilterRequest;
        interfaceNetFilterRequest.targetDeviceName = wsl::shared::string::GuidToString<wchar_t>(foundEndpoint->InterfaceGuid);
        interfaceNetFilterRequest.operation = hns::OperationType::Remove;

        int linuxResultCode{};
        // can safely capture by ref since we are waiting
        auto hr = m_gnsCallbackQueue.submit_and_wait([&] {
            return m_callbackForGnsMessage(
                LxGnsMessageInterfaceNetFilter, ToJsonW(std::move(interfaceNetFilterRequest)), GnsCallbackFlags::Wait, &linuxResultCode);
        });
        LOG_IF_FAILED(hr);
        WSL_LOG(
            "WslMirroredNetworkManager::RemoveEndpoint [InterfaceNetFilterRequest]",
            TraceLoggingHResult(hr, "hr"),
            TraceLoggingValue(linuxResultCode, "linuxResultCode"),
            TraceLoggingValue(endpointId, "endpointId"),
            TraceLoggingValue(foundEndpoint->InterfaceGuid, "interfaceGuid"));

        // A race exists between already queued operations for this interface on the GNS queue and HNS endpoint removal.
        // In order to resolve the race, while holding the m_networkLock, flush the GNS queue then delete the endpoint in HCS.
        WSL_LOG("WslMirroredNetworkManager::RemoveEndpoint", TraceLoggingValue("Flush GNS queue [queued]", "message"));

        linuxResultCode = {};
        // can safely capture by ref since we are waiting
        hr = m_gnsCallbackQueue.submit_and_wait([&] {
            return m_callbackForGnsMessage(LxGnsMessageNoOp, std::wstring(L""), GnsCallbackFlags::Wait, &linuxResultCode);
        });
        WSL_LOG(
            "WslMirroredNetworkManager::RemoveEndpoint",
            TraceLoggingValue("Flush GNS queue [completed]", "message"),
            TraceLoggingHResult(hr, "hr"),
            TraceLoggingValue(linuxResultCode, "linuxResultCode"));

        // try to delete the endpoint in HCS
        // Set the instance id to the mirrored interfaceGuid so HNS -> netvsc can optimally use the same vmNIC constructs when the InterfaceGuid is the same

        hcs::ModifySettingRequest<hcs::NetworkAdapter> networkRequest{};
        networkRequest.ResourcePath = c_networkAdapterPrefix + wsl::shared::string::GuidToString<wchar_t>(foundEndpoint->InterfaceGuid);
        networkRequest.RequestType = hcs::ModifyRequestType::Remove;
        networkRequest.Settings.InstanceId = foundEndpoint->InterfaceGuid;
        networkRequest.Settings.EndpointId = endpointId;

        const auto networkRequestString = wsl::shared::ToJsonW(networkRequest);

        WSL_LOG(
            "WslMirroredNetworkManager::RemoveEndpoint : Removing the HCS mirrored endpoint [queued]",
            TraceLoggingValue(networkRequestString.c_str(), "networkRequest"),
            TraceLoggingValue(endpointId, "endpointId"));
        // capturing by ref because we wait for the workitem to complete
        hr = m_hnsQueue.submit_and_wait([&] {
            windows::common::hcs::ModifyComputeSystem(m_hcsSystem, networkRequestString.c_str());
            return S_OK;
        });
        WSL_LOG(
            "WslMirroredNetworkManager::RemoveEndpoint : Removing the HCS mirrored endpoint [completed]",
            TraceLoggingHResult(hr, "hr"));

        if (FAILED(hr))
        {
            WSL_LOG(
                "RemoveMirroredEndpointFailed",
                TraceLoggingHResult(hr, "result"),
                TraceLoggingValue("RemoveHcsEndpoint", "executionStep"),
                TraceLoggingValue(m_vmConfig.EnableDnsTunneling, "DnsTunnelingEnabled"),
                TraceLoggingValue(m_vmConfig.FirewallConfig.Enabled(), "HyperVFirewallEnabled"),
                TraceLoggingValue(m_vmConfig.EnableAutoProxy, "AutoProxyFeatureEnabled")); // the feature is enabled, but we don't know if proxy settings are actually configured
        }
    }
    CATCH_LOG()

    // Remove the endpoint and its tracked state
    // Linux will delete any addresses and routes associated with the interface
    m_networkEndpoints.erase(foundEndpoint);
    WSL_LOG(
        "WslMirroredNetworkManager::RemoveEndpoint - Endpoint removed from m_networkEndpoints",
        TraceLoggingValue(endpointId, "endpointId"));

    // Is this necessary?
    UpdateAllEndpointsImpl(UpdateEndpointFlag::Default, "RemoveEndpoint");

    return S_OK;
}
CATCH_RETURN()

void wsl::core::networking::WslMirroredNetworkManager::SendCreateNotificationsForInitialEndpoints() noexcept
{
    WSL_LOG("WslMirroredNetworkManager::SendCreateNotificationsForInitialEndpoints");
    const auto lock = m_networkLock.lock_shared();
    if (m_state == State::Stopped)
    {
        return;
    }

    // Perform global configuration of net filter rules.
    int linuxResultCode{};
    // can safely capture by ref since we are waiting
    const auto hr = m_gnsCallbackQueue.submit_and_wait([&] {
        return m_callbackForGnsMessage(LxGnsMessageGlobalNetFilter, std::wstring(L""), GnsCallbackFlags::Wait, &linuxResultCode);
    });
    WSL_LOG(
        "WslMirroredNetworkManager::SendCreateNotificationsForInitialEndpoints",
        TraceLoggingValue("Sent message to perform global configuration of net filter rules", "message"),
        TraceLoggingHResult(hr, "hr"),
        TraceLoggingValue(linuxResultCode, "linuxResultCode"));
    LOG_IF_FAILED(hr);
}

HRESULT wsl::core::networking::WslMirroredNetworkManager::WaitForMirroredGoalState() noexcept
{
    WSL_LOG("WslMirroredNetworkManager::WaitForMirroredGoalState");

    return (m_inMirroredGoalState.wait(c_initialMirroredGoalStateWaitTimeoutMs)) ? S_OK : HRESULT_FROM_WIN32(ERROR_TIMEOUT);
}

_Check_return_ bool wsl::core::networking::WslMirroredNetworkManager::DoesEndpointExist(GUID networkId) const noexcept
try
{
    const auto lock = m_networkLock.lock_shared();
    if (m_state == State::Stopped)
    {
        return false;
    }

    return std::ranges::any_of(m_networkEndpoints, [&](const NetworkEndpoint& endpoint) { return endpoint.NetworkId == networkId; });
}
catch (...)
{
    LOG_CAUGHT_EXCEPTION();
    return false;
}

_Requires_lock_not_held_(m_networkLock)
void wsl::core::networking::WslMirroredNetworkManager::UpdateAllEndpoints(_In_ PCSTR sourceName) noexcept
{
    const auto lock = m_networkLock.lock_exclusive();
    if (m_state == State::Stopped)
    {
        return;
    }

    UpdateAllEndpointsImpl(UpdateEndpointFlag::Default, sourceName);
}

void wsl::core::networking::WslMirroredNetworkManager::OnNetworkConnectivityHintChange() noexcept
{
    const auto lock = m_networkLock.lock_exclusive();
    if (m_state == State::Stopped)
    {
        return;
    }

    UpdateAllEndpointsImpl(UpdateEndpointFlag::Default, "OnNetworkConnectivityHintChange");
}

// Strategy for handling notifications from HNS:
// 1) Always consume the data immediately.
// 2) If UpdateAllEndpointsImpl hasn't run for >= m_debounceUpdateAllEndpointsTimerMs then run it.
// 3) If UpdateAllEndpointsImpl has run < m_debounceUpdateAllEndpointsTimerMs ago, schedule the timer.
void wsl::core::networking::WslMirroredNetworkManager::OnNetworkEndpointChange() noexcept
{
    const auto lock = m_networkLock.lock_exclusive();
    if (m_state == State::Stopped)
    {
        return;
    }

    UpdateAllEndpointsImpl(UpdateEndpointFlag::Default, "OnNetworkEndpointChange");
}

void wsl::core::networking::WslMirroredNetworkManager::OnDnsSuffixChange() noexcept
try
{
    const auto lock = m_networkLock.lock_exclusive();
    if (m_state == State::Stopped)
    {
        return;
    }

    UpdateAllEndpointsImpl(UpdateEndpointFlag::Default, "OnDnsSuffixChange");
}
CATCH_LOG();

void wsl::core::networking::WslMirroredNetworkManager::TunAdapterStateChanged(_In_ const std::string& interfaceName, _In_ bool up) noexcept
{
}

void wsl::core::networking::WslMirroredNetworkManager::ReconnectGuestNetwork()
{
    auto lock = m_networkLock.lock_exclusive();
    if (m_state == State::Stopped)
    {
        return;
    }

    WSL_LOG("WslMirroredNetworkManager::ReconnectGuestNetwork");
    UpdateAllEndpointsImpl(UpdateEndpointFlag::ForceUpdate, "ReconnectGuestNetwork");
}

_Requires_lock_held_(m_networkLock)
wsl::core::networking::NetworkSettings wsl::core::networking::WslMirroredNetworkManager::GetNetworkSettingsOfInterface(DWORD ifIndex) const
{
    const auto matchingEndpoint =
        std::ranges::find_if(m_networkEndpoints, [&](const auto& endpoint) { return endpoint.Network->InterfaceIndex == ifIndex; });
    if (matchingEndpoint == std::end(m_networkEndpoints))
    {
        WSL_LOG("GetNetworkSettingsOfInterface - Network not found", TraceLoggingValue(ifIndex, "ifIndex"));
        return {};
    }
    else
    {
        WSL_LOG("GetNetworkSettingsOfInterface", TRACE_NETWORKSETTINGS_OBJECT(matchingEndpoint->Network.get()));
        return *matchingEndpoint->Network;
    }
}

std::shared_ptr<wsl::core::networking::NetworkSettings> wsl::core::networking::WslMirroredNetworkManager::GetEndpointSettings(
    const hns::HNSEndpoint& endpointProperties) const
{
    return wsl::core::networking::GetEndpointSettings(endpointProperties);
}

_Requires_lock_held_(m_networkLock)
_Check_return_ HRESULT wsl::core::networking::WslMirroredNetworkManager::UpdateHcnServiceTimer() noexcept
try
{
    // These values are chosen so that the connection will be retried 5 times.
    static constexpr DWORD INITIAL_RETRY_HCN_SERVICE_CONNECTION_TIMER_DURATION_MS = 1000; // 1 second.
    static constexpr DWORD MAX_RETRY_HCN_SERVICE_CONNECTION_TIMER_DURATION_MS = 80000;    // ~1.3 minute.

    // Check if the maximum retry attempt count has been reached.
    if (m_retryHcnServiceConnectionDurationMs <= MAX_RETRY_HCN_SERVICE_CONNECTION_TIMER_DURATION_MS)
    {
        // Determine how long until the timer should fire.
        if (m_retryHcnServiceConnectionDurationMs == 0)
        {
            // Use the initial duration value as this is the first time the timer is being armed.
            m_retryHcnServiceConnectionDurationMs = INITIAL_RETRY_HCN_SERVICE_CONNECTION_TIMER_DURATION_MS;
        }
        else
        {
            // Make sure that the timer duration can't overflow.
            static_assert(MAX_RETRY_HCN_SERVICE_CONNECTION_TIMER_DURATION_MS < (DWORD_MAX / 2));

            // Apply an exponential backoff.
            m_retryHcnServiceConnectionDurationMs *= 2;
        }

        WSL_LOG(
            "WslMirroredNetworkManager::UpdateHcnServiceTimer",
            TraceLoggingValue(m_retryHcnServiceConnectionDurationMs, "m_retryHcnServiceConnectionDurationMs"));

        FILETIME dueTime = wil::filetime::from_int64(
            static_cast<ULONGLONG>(-1 * wil::filetime_duration::one_millisecond * m_retryHcnServiceConnectionDurationMs));
        SetThreadpoolTimer(m_retryHcnServiceConnectionTimer.get(), &dueTime, 0, 1000);
    }
    else
    {
        WSL_LOG(
            "WslMirroredNetworkManager::UpdateHcnServiceTimer",
            TraceLoggingValue(0, "retryHcnServiceConnectionDurationMs (service is not active)"));
        THROW_WIN32(ERROR_SERVICE_NOT_ACTIVE);
    }

    return S_OK;
}
CATCH_RETURN()

_Requires_lock_held_(m_networkLock)
_Check_return_ HRESULT wsl::core::networking::WslMirroredNetworkManager::ResetHcnServiceSession() noexcept
try
{
    if (!m_hcnCallback)
    {
        WSL_LOG("WslMirroredNetworkManager::ResetHcnServiceSession - attempting to re-register"); // Attempt to resubscribe to HNS notifications.
        m_hcnCallback = windows::common::hcs::RegisterServiceCallback(HcnCallback, this);

        // if we can reregister, reset the retry timer.
        m_retryHcnServiceConnectionDurationMs = 0;
        SetThreadpoolTimer(m_retryHcnServiceConnectionTimer.get(), nullptr, 0, 0);

        std::vector<GUID> enumeratedNetworkIds;
        try
        {
            // Refresh the current list of networks. The list will then be kept
            // up to date by the subscription notifications.
            enumeratedNetworkIds = EnumerateMirroredNetworks();
        }
        catch (...)
        {
            const auto hr = wil::ResultFromCaughtException();
            WSL_LOG(
                "ResetHcnServiceSessionFailed",
                TraceLoggingValue(hr, "result"),
                TraceLoggingValue("HcnEnumerateNetworks", "executionStep"),
                TraceLoggingValue("Mirrored", "networkingMode"),
                TraceLoggingValue(m_vmConfig.EnableDnsTunneling, "DnsTunnelingEnabled"),
                TraceLoggingValue(m_vmConfig.FirewallConfig.Enabled(), "HyperVFirewallEnabled"),
                TraceLoggingValue(m_vmConfig.EnableAutoProxy, "AutoProxyFeatureEnabled")); // the feature is enabled, but we don't know if proxy settings are actually configured

            throw;
        }

        wil::unique_cotaskmem_string response;
        wil::unique_cotaskmem_string error;
        const auto enumEndpointsHr = HcnEnumerateEndpoints(nullptr, &response, &error);
        if (FAILED(enumEndpointsHr))
        {
            WSL_LOG(
                "ResetHcnServiceSessionFailed",
                TraceLoggingValue(enumEndpointsHr, "result"),
                TraceLoggingValue("HcnEnumerateEndpoints", "executionStep"),
                TraceLoggingValue("Mirrored", "networkingMode"),
                TraceLoggingValue(m_vmConfig.EnableDnsTunneling, "DnsTunnelingEnabled"),
                TraceLoggingValue(m_vmConfig.FirewallConfig.Enabled(), "HyperVFirewallEnabled"),
                TraceLoggingValue(m_vmConfig.EnableAutoProxy, "AutoProxyFeatureEnabled")); // the feature is enabled, but we don't know if proxy settings are actually configured
        }
        else
        {
            WSL_LOG(
                "WslMirroredNetworkManager::ResetHcnServiceSession - HcnEnumerateEndpoints",
                TraceLoggingValue(response.get(), "response"));
        }

        for (const auto& networkId : enumeratedNetworkIds)
        {
            // Must call back through MirroredNetworking to create a new Endpoint
            // note that the callback will not block - it just queues the work in MirroredNetworking
            LOG_IF_FAILED(AddNetwork(networkId));
        }
    }
    else
    {
        WSL_LOG("WslMirroredNetworkManager::ResetHcnServiceSession - already re-registered");
    }

    return S_OK;
}
CATCH_RETURN()

void wsl::core::networking::WslMirroredNetworkManager::TelemetryConnectionCallback(NLM_CONNECTIVITY hostConnectivity, uint32_t telemetryCounter) noexcept
try
{
    WSL_LOG("WslMirroredNetworkManager::TelemetryConnectionCallback");

    const auto lock = m_networkLock.lock_exclusive();
    if (m_state == State::Stopped)
    {
        return;
    }

    // if this is the inital callback for checking container connectivity, push this through as telemetry, so we can observe the time-to-connect
    if ((telemetryCounter > 1) && !(hostConnectivity & NLM_CONNECTIVITY_IPV4_INTERNET) && !(hostConnectivity & NLM_CONNECTIVITY_IPV6_INTERNET))
    {
        WSL_LOG(
            "WslMirroredNetworkManager::TelemetryConnectionCallback - not testing connectivity - host is not connected",
            TraceLoggingValue(telemetryCounter, "telemetryCounter"),
            TraceLoggingValue(wsl::core::networking::ToString(hostConnectivity).c_str(), "HostConnectivityLevel"));
        return;
    }

    int returnedIPv4Value{};
    LOG_IF_FAILED(m_gnsCallbackQueue.submit_and_wait([&] {
        return m_callbackForGnsMessage(LxGnsMessageConnectTestRequest, c_ipv4TestRequestTarget, GnsCallbackFlags::Wait, &returnedIPv4Value);
    }));

    int returnedIPv6Value{};
    LOG_IF_FAILED(m_gnsCallbackQueue.submit_and_wait([&] {
        return m_callbackForGnsMessage(LxGnsMessageConnectTestRequest, c_ipv6TestRequestTarget, GnsCallbackFlags::Wait, &returnedIPv6Value);
    }));

    // make the same connect requests as we just requested from the container
    const auto hostConnectivityCheck =
        wsl::shared::conncheck::CheckConnection(c_ipv4TestRequestTargetA, c_ipv6TestRequestTargetA, "80");
    const auto WindowsIpv4ConnCheckStatus = static_cast<uint32_t>(hostConnectivityCheck.Ipv4Status);
    const auto WindowsIpv6ConnCheckStatus = static_cast<uint32_t>(hostConnectivityCheck.Ipv6Status);
    const auto WindowsIPv4NlmConnectivityLevel = ConnectivityTelemetry::WindowsIPv4NlmConnectivityLevel(hostConnectivity);
    const auto WindowsIPv6NlmConnectivityLevel = ConnectivityTelemetry::WindowsIPv6NlmConnectivityLevel(hostConnectivity);
    const auto LinuxIPv4ConnCheckStatus = ConnectivityTelemetry::LinuxIPv4ConnCheckResult(returnedIPv4Value);
    const auto LinuxIPv6ConnCheckStatus = ConnectivityTelemetry::LinuxIPv6ConnCheckResult(returnedIPv6Value);

    const auto timeFromObjectCreation = std::chrono::steady_clock::now() - m_objectCreationTime;

    // Logs when network connectivity changes, used to compare network connectivity in the guest to the host to determine networking health
    WSL_LOG_TELEMETRY(
        "TelemetryConnectionCallback",
        PDT_ProductAndServicePerformance,
        TraceLoggingValue("Mirrored", "networkingMode"),
        TraceLoggingValue(telemetryCounter, "telemetryCounter"),
        TraceLoggingValue(
            (std::chrono::duration_cast<std::chrono::milliseconds>(timeFromObjectCreation)).count(), "timeFromObjectCreationMs"),
        TraceLoggingValue(wsl::core::networking::ToString(hostConnectivity).c_str(), "HostConnectivityLevel"),
        TraceLoggingValue(WindowsIPv4NlmConnectivityLevel, "WindowsIPv4ConnectivityLevel"),
        TraceLoggingValue(WindowsIPv6NlmConnectivityLevel, "WindowsIPv6ConnectivityLevel"),
        TraceLoggingValue(LinuxIPv4ConnCheckStatus, "LinuxIPv4ConnCheckStatus"),
        TraceLoggingValue(LinuxIPv6ConnCheckStatus, "LinuxIPv6ConnCheckStatus"),
        TraceLoggingValue(WindowsIpv4ConnCheckStatus, "WindowsIpv4ConnCheckStatus"),
        TraceLoggingValue(WindowsIpv6ConnCheckStatus, "WindowsIpv6ConnCheckStatus"),
        TraceLoggingValue(m_vmConfig.EnableDnsTunneling, "DnsTunnelingEnabled"),
        TraceLoggingValue(m_dnsTunnelingIpAddress.c_str(), "DnsTunnelingIpAddress"),
        TraceLoggingValue(m_vmConfig.FirewallConfig.Enabled(), "HyperVFirewallEnabled"),
        TraceLoggingValue(m_vmConfig.EnableAutoProxy, "AutoProxyFeatureEnabled")); // the feature is enabled, but we don't know if proxy settings are actually configured
}
CATCH_LOG()

void __stdcall wsl::core::networking::WslMirroredNetworkManager::HcnServiceConnectionTimerCallback(
    _Inout_ PTP_CALLBACK_INSTANCE, _Inout_opt_ PVOID Context, _Inout_ PTP_TIMER) noexcept
{
    WSL_LOG("WslMirroredNetworkManager::HcnServiceConnectionTimerCallback");

    auto* const manager = static_cast<WslMirroredNetworkManager*>(Context);

    const auto lock = manager->m_networkLock.lock_exclusive();
    WI_ASSERT(manager->m_state == State::Started);
    if (manager->m_state == State::Stopped)
    {
        return;
    }

    if (FAILED(manager->ResetHcnServiceSession()))
    {
        // The retry attempt was unsuccessful, re-arm the timer to try again.
        LOG_IF_FAILED(manager->UpdateHcnServiceTimer());
    }
}

void CALLBACK wsl::core::networking::WslMirroredNetworkManager::HcnCallback(
    _In_ DWORD NotificationType, _In_opt_ void* Context, _In_ HRESULT, _In_opt_ PCWSTR NotificationData) noexcept
try
{
    hns::NotificationBase data = {};
    if (NotificationType == HcnNotificationNetworkCreate || NotificationType == HcnNotificationNetworkPreDelete)
    {
        data = FromJson<hns::NotificationBase>(NotificationData);
    }

    auto* const manager = static_cast<WslMirroredNetworkManager*>(Context);

    const auto lock = manager->m_networkLock.lock_exclusive();
    WI_ASSERT(manager->m_state == State::Started);
    if (manager->m_state == State::Stopped)
    {
        return;
    }

    WSL_LOG(
        "WslMirroredNetworkManager::HcnCallback [HcnRegisterServiceCallback]",
        TraceLoggingValue(NotificationType, "notificationType"),
        TraceLoggingValue(wsl::windows::common::stringify::HcnNotificationsToString(NotificationType), "notificationTypeString"),
        TraceLoggingValue(data.ID, "networkId"),
        TraceLoggingValue(data.Flags, "flags"),
        TraceLoggingValue(NotificationData, "notificationData"));

    switch (NotificationType)
    {
    case HcnNotificationNetworkCreate:
    {
        // convert the enum to integer to allow for bitmap comparisons
        if (!WI_IsFlagSet(data.Flags, WI_EnumValue(hns::NetworkFlags::EnableFlowSteering)))
        {
            WSL_LOG("WslMirroredNetworkManager::HcnCallback [HcnRegisterServiceCallback] - not a mirrored network");
            return;
        }

        LOG_IF_FAILED(manager->AddNetwork(data.ID));
        break;
    }

    case HcnNotificationNetworkPreDelete:
    {
        // This notification is fired off right before HNS network deletion.
        // Ensure Containers release endpoints whether network deletion
        // is successful or not.
        LOG_IF_FAILED(manager->RemoveNetwork(data.ID));
        break;
    }

    case HcnNotificationServiceDisconnect:
    {
        // This notification indicates that the subscription has become invalid due to a loss
        // of connection to the server. This typically means that the HNS service has been
        // stopped or restarted.
        manager->m_hcnCallback.reset();

        manager->m_networkEndpoints.clear();

        LOG_IF_FAILED(manager->UpdateHcnServiceTimer());
        break;
    }
    }
    manager->UpdateAllEndpointsImpl(UpdateEndpointFlag::Default, "HcnCallback");
}
CATCH_LOG()

const char* wsl::core::networking::WslMirroredNetworkManager::StateToString(State state) noexcept
{
    switch (state)
    {
    case State::Stopped:
        return "Stopped";
    case State::Started:
        return "Started";
    case State::Starting:
        return "Starting";
    default:
        return "Unknown";
    }
}

void wsl::core::networking::WslMirroredNetworkManager::TraceLoggingRundown() const
{
    auto lock = m_networkLock.lock_shared();

    WSL_LOG(
        "WslMirroredNetworkManager::TraceLoggingRundown",
        TraceLoggingValue("Global State"),
        TraceLoggingValue(StateToString(m_state), "state"),
        TraceLoggingValue(GenerateResolvConf(m_trackedDnsInfo).c_str(), "dnsInfo"));

    for (const auto& network : m_networkEndpoints)
    {
        WSL_LOG("WslMirroredNetworkManager::TraceLoggingRundown", TRACE_NETWORKSETTINGS_OBJECT(network.Network));

        if (network.StateTracking)
        {
            WSL_LOG(
                "WslMirroredNetworkManager::TraceLoggingRundown",
                TraceLoggingValue("IpStateTracking Interface Info"),
                TraceLoggingValue(network.StateTracking->InterfaceGuid, "interfaceGuid"),
                TraceLoggingValue(network.StateTracking->InterfaceMtu, "mtu"));

            for (const auto& address : network.StateTracking->IpAddresses)
            {
                WSL_LOG(
                    "WslMirroredNetworkManager::TraceLoggingRundown",
                    TraceLoggingValue("IpStateTracking::IpAddresses"),
                    TraceLoggingValue(address.Address.AddressString.c_str(), "address"),
                    TraceLoggingValue(address.Address.PrefixLength, "prefixLength"),
                    TraceLoggingValue(wsl::core::networking::ToString(address.SyncStatus), "syncStatus"),
                    TraceLoggingValue(address.SyncRetryCount, "syncRetryCount"),
                    TraceLoggingValue(address.LoopbackSyncRetryCount, "loopbackSyncRetryCount"));
            }

            for (const auto& route : network.StateTracking->Routes)
            {
                WSL_LOG(
                    "WslMirroredNetworkManager::TraceLoggingRundown",
                    TraceLoggingValue("IpStateTracking::Routes"),
                    TraceLoggingValue(route.Route.ToString().c_str(), "route"),
                    TraceLoggingValue(route.Route.Metric, "metric"),
                    TraceLoggingValue(
                        !route.CanConflictWithLinuxAutoGenRoute() || route.LinuxConflictRemoved,
                        "linuxConflictRemovedOrDoesntExist"),
                    TraceLoggingValue(wsl::core::networking::ToString(route.SyncStatus), "syncStatus"),
                    TraceLoggingValue(route.SyncRetryCount, "syncRetryCount"));
            }
        }
    }
}
