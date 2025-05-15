// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "WslCoreNetworkingSupport.h"
#include "Stringify.h"

using wsl::windows::common::Context;
using wsl::windows::common::ExecutionContext;

/// <summary>
/// Used for blocked interface telemetry
/// </summary>
enum class InterfaceUnsupportedReason
{
    UnknownInterface = 0,
    NrptDnsRulesDetected,
    InterfaceDetailsQueryFailed,
    NotPhysicalEthernet,
    BlockedRegistryKey
};

namespace details {

static bool FindInterfacesForNetworkAdapter(
    const IF_INDEX interfaceIndex, const GUID& interfaceGuid, bool metered, std::vector<wsl::core::networking::CurrentInterfaceInformation>& returnedNetworks)
{
    bool addedNetwork = false;

    wsl::core::networking::unique_ifstack_table interfaceStackTable{};
    if (FAILED_WIN32_LOG(GetIfStackTable(&interfaceStackTable)))
    {
        return addedNetwork;
    }

    wsl::core::networking::unique_address_table addressTable{};
    if (FAILED_WIN32_LOG(GetUnicastIpAddressTable(AF_INET, &addressTable)))
    {
        return addedNetwork;
    }

    // Find the IP interface(s) in the adapter's interface stack.
    std::vector<IF_INDEX> ipInterfaces{};
    std::queue<IF_INDEX> interfaceStack{};
    interfaceStack.push(interfaceIndex);
    while (!interfaceStack.empty())
    {
        IF_INDEX currInterfaceIndex = interfaceStack.front();
        interfaceStack.pop();

        for (unsigned int i = 0; i < interfaceStackTable.get()->NumEntries; i++)
        {
            if (interfaceStackTable.get()->Table[i].LowerLayerInterfaceIndex == currInterfaceIndex)
            {
                interfaceStack.push(interfaceStackTable.get()->Table[i].HigherLayerInterfaceIndex);
            }
        }

        MIB_IPINTERFACE_ROW ipInterfaceRow{};
        ipInterfaceRow.Family = AF_INET;
        ipInterfaceRow.InterfaceIndex = currInterfaceIndex;
        if (SUCCEEDED_WIN32(GetIpInterfaceEntry(&ipInterfaceRow)) && ipInterfaceRow.Connected)
        {
            // We found a connected IP interface.  Ensure it has a preferred IP address too.
            for (unsigned int i = 0; i < addressTable.get()->NumEntries; i++)
            {
                if (addressTable.get()->Table[i].InterfaceIndex == currInterfaceIndex && addressTable.get()->Table[i].DadState == IpDadStatePreferred)
                {
                    ipInterfaces.push_back(currInterfaceIndex);
                    break;
                }
            }
        }
    }

    for (auto currInterfaceIndex : ipInterfaces)
    {
        MIB_IF_ROW2 row{};
        row.InterfaceIndex = currInterfaceIndex;
        if (FAILED_WIN32_LOG(GetIfEntry2Ex(MibIfEntryNormalWithoutStatistics, &row)))
        {
            continue;
        }

        WSL_LOG(
            "FindInterfacesForNetworkAdapter : returning connected network profile for IP interface on NIC",
            TraceLoggingValue(interfaceGuid, "underlyingInterfaceGuid"),
            TraceLoggingValue(row.InterfaceGuid, "interfaceGuid"),
            TraceLoggingValue(row.Type, "ifType"),
            TraceLoggingValue(row.Alias, "ifAlias"),
            TraceLoggingValue(row.Description, "ifDescription"));

        returnedNetworks.emplace_back(row.InterfaceGuid, row.InterfaceLuid, row.Type, row.Alias, row.Description, metered);
        addedNetwork = true;
    }

    return addedNetwork;
}

} // namespace details

bool wsl::core::networking::IsMetered(ABI::Windows::Networking::Connectivity::NetworkCostType cost) noexcept
{
    return (cost == ABI::Windows::Networking::Connectivity::NetworkCostType::NetworkCostType_Fixed) ||
           (cost == ABI::Windows::Networking::Connectivity::NetworkCostType::NetworkCostType_Variable);
}

bool wsl::core::networking::IsFlowSteeringSupportedByHns() noexcept
{
    static bool supported = false;
    static std::once_flag fseMethodsLoadedFlag;
    static constexpr auto c_computeNetworkModuleName = L"ComputeNetwork.dll";
    std::call_once(fseMethodsLoadedFlag, [&]() {
        try
        {
            static LxssDynamicFunction<decltype(HcnReserveGuestNetworkServicePortRange)> allocatePortRange{DynamicFunctionErrorLogs::None};
            RETURN_IF_FAILED_EXPECTED(
                allocatePortRange.load(c_computeNetworkModuleName, "HcnReserveGuestNetworkServicePortRange"));

            static LxssDynamicFunction<decltype(HcnReserveGuestNetworkServicePort)> allocatePort{DynamicFunctionErrorLogs::None};
            RETURN_IF_FAILED_EXPECTED(allocatePortRange.load(c_computeNetworkModuleName, "HcnReserveGuestNetworkServicePort"));

            static LxssDynamicFunction<decltype(HcnReleaseGuestNetworkServicePortReservationHandle)> releasePort{DynamicFunctionErrorLogs::None};
            RETURN_IF_FAILED_EXPECTED(
                allocatePortRange.load(c_computeNetworkModuleName, "HcnReleaseGuestNetworkServicePortReservationHandle"));

            supported = true;
        }
        CATCH_LOG()
        return S_OK;
    });

    if (!supported)
    {
        WSL_LOG("IsFlowSteeringSupportedByHns (false) - Port reservation functions are not present");
    }
    return supported;
}

std::vector<wsl::core::networking::CurrentInterfaceInformation> wsl::core::networking::EnumerateConnectedInterfaces()
{
    using ABI::Windows::Foundation::Collections::IVectorView;
    using ABI::Windows::Networking::Connectivity::ConnectionProfile;
    using ABI::Windows::Networking::Connectivity::IConnectionCost;
    using ABI::Windows::Networking::Connectivity::INetworkAdapter;
    using ABI::Windows::Networking::Connectivity::INetworkInformationStatics;
    using ABI::Windows::Networking::Connectivity::NetworkConnectivityLevel;
    using ABI::Windows::Networking::Connectivity::NetworkCostType;

    std::vector<wsl::core::networking::CurrentInterfaceInformation> returnedNetworks;
    try
    {
        const auto roInit = wil::RoInitialize();
        auto networkInformationStatics =
            wil::GetActivationFactory<INetworkInformationStatics>(RuntimeClass_Windows_Networking_Connectivity_NetworkInformation);
        THROW_HR_IF_NULL_MSG(E_OUTOFMEMORY, networkInformationStatics.get(), "null INetworkInformationStatics");

        wil::com_ptr<IVectorView<ConnectionProfile*>> connectionList;
        THROW_IF_FAILED(networkInformationStatics->GetConnectionProfiles(&connectionList));

        for (const auto& connectionProfile : wil::get_range(connectionList.get()))
        {
            NetworkConnectivityLevel connectivityLevel{};
            CONTINUE_IF_FAILED(connectionProfile->GetNetworkConnectivityLevel(&connectivityLevel));
            if (connectivityLevel == NetworkConnectivityLevel::NetworkConnectivityLevel_None)
            {
                continue;
            }

            wil::com_ptr<IConnectionCost> connectionCost;
            CONTINUE_IF_FAILED(connectionProfile->GetConnectionCost(&connectionCost));

            NetworkCostType cost{};
            CONTINUE_IF_FAILED(connectionCost->get_NetworkCostType(&cost));
            bool metered = IsMetered(cost);

            wil::com_ptr<INetworkAdapter> networkAdapter;
            CONTINUE_IF_FAILED(connectionProfile->get_NetworkAdapter(&networkAdapter));

            IFTYPE ifType{};
            CONTINUE_IF_FAILED(networkAdapter->get_IanaInterfaceType(reinterpret_cast<UINT32*>(&ifType)));

            GUID interfaceGuid{};
            CONTINUE_IF_FAILED(networkAdapter->get_NetworkAdapterId(&interfaceGuid));

            NET_LUID interfaceLuid{};
            CONTINUE_IF_FAILED_WIN32(ConvertInterfaceGuidToLuid(&interfaceGuid, &interfaceLuid));

            MIB_IF_ROW2 row{};
            row.InterfaceLuid = interfaceLuid;
            CONTINUE_IF_FAILED_WIN32(GetIfEntry2Ex(MibIfEntryNormalWithoutStatistics, &row));

            MIB_IPINTERFACE_ROW ipIfRow{};
            InitializeIpInterfaceEntry(&ipIfRow);
            ipIfRow.Family = AF_INET;
            ipIfRow.InterfaceLuid = interfaceLuid;
            if (FAILED_WIN32(GetIpInterfaceEntry(&ipIfRow)))
            {
                // There is no IP interface directly attached to the given network adapter.  One way this could happen
                // is if the network adapter is under an external vmswitch.  If that's the case, there should be at
                // least one IP interface farther up the network adapter's interface stack.  We return all such IP
                // interfaces as connected interfaces, as we don't know which is preferred at this point.
                WSL_LOG(
                    "EnumerateConnectedInterfaces : connection profile's network adapter is not directly bound to TCP/IP - "
                    "searching its interface stack for an IP interface",
                    TraceLoggingValue(interfaceGuid, "physicalInterfaceGuid"),
                    TraceLoggingValue(ifType, "ifType"),
                    TraceLoggingValue(row.Alias, "ifAlias"),
                    TraceLoggingValue(row.Description, "ifDescription"),
                    TraceLoggingValue(wsl::windows::common::stringify::ToString(connectivityLevel), "connectivityLevel"));

                if (!details::FindInterfacesForNetworkAdapter(row.InterfaceIndex, interfaceGuid, metered, returnedNetworks))
                {
                    WSL_LOG(
                        "EnumerateConnectedInterfaces : could not find any IP interfaces for connected network profile",
                        TraceLoggingValue(interfaceGuid, "interfaceGuid"));
                }
                // TODO - if FindInterfacesForNetworkAdapter returns false, what should we add to returnedNetworks
            }
            else
            {
                WSL_LOG(
                    "EnumerateConnectedInterfaces : returning connected network profile",
                    TraceLoggingValue(interfaceGuid, "interfaceGuid"),
                    TraceLoggingValue(row.Type, "ifType"),
                    TraceLoggingValue(row.Alias, "ifAlias"),
                    TraceLoggingValue(row.Description, "ifDescription"),
                    TraceLoggingValue(wsl::windows::common::stringify::ToString(connectivityLevel), "connectivityLevel"));

                returnedNetworks.emplace_back(interfaceGuid, interfaceLuid, ifType, row.Alias, row.Description, metered);
            }
        }
    }
    CATCH_LOG()

    return returnedNetworks;
}

wsl::core::networking::EphemeralHcnEndpoint wsl::core::networking::CreateEphemeralHcnEndpoint(
    HCN_NETWORK network, const wsl::shared::hns::HostComputeEndpoint& endpointSettings)
{
    wsl::core::networking::EphemeralHcnEndpoint endpoint{};
    wil::unique_cotaskmem_string error;
    const auto settings = wsl::shared::ToJsonW(endpointSettings);

    ExecutionContext context(Context::HNS);
    const auto result = HcnCreateEndpoint(network, endpoint.Id, settings.c_str(), &endpoint.Endpoint, &error);
    THROW_IF_FAILED_MSG(result, "HcnCreateEndpoint(%ls) failed: %ls", settings.c_str(), error.get());

    return endpoint;
}
