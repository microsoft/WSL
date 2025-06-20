// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "BridgedNetworking.h"
#include "hcs.hpp"

using wsl::core::BridgedNetworking;
using wsl::core::networking::NetworkSettings;
using namespace wsl::windows::common;

BridgedNetworking::BridgedNetworking(HCS_SYSTEM system, const Config& config) : m_system(system), m_config(config)
{
}

void BridgedNetworking::Initialize()
{
    if (m_config.VmSwitch.empty())
    {
        THROW_HR_WITH_USER_ERROR(WSL_E_VMSWITCH_NOT_SET, wsl::shared::Localization::MessageVmSwitchNotSet());
    }

    std::vector<std::wstring> availableSwitches;
    wsl::windows::common::hcs::unique_hcn_network network;
    std::optional<GUID> switchId;
    for (const auto& id : wsl::core::networking::EnumerateNetworks())
    {
        try
        {
            network = wsl::core::networking::OpenNetwork(id);
            auto [networkProperties, propertiesString] = wsl::core::networking::QueryNetworkProperties(network.get());
            if (networkProperties.Name == m_config.VmSwitch)
            {
                switchId = id;
                break;
            }

            availableSwitches.emplace_back(std::move(networkProperties.Name));
        }
        CATCH_LOG()
    }

    if (!switchId.has_value())
    {
        THROW_HR_WITH_USER_ERROR(
            WSL_E_VMSWITCH_NOT_FOUND,
            wsl::shared::Localization::MessageVmSwitchNotFound(
                m_config.VmSwitch.c_str(), wsl::shared::string::Join<wchar_t>(availableSwitches, ',').c_str()));
    }

    wsl::shared::hns::HostComputeEndpoint hnsEndpoint{};
    hnsEndpoint.SchemaVersion.Major = 2;
    hnsEndpoint.SchemaVersion.Minor = 16;
    hnsEndpoint.HostComputeNetwork = switchId.value();
    wsl::shared::hns::EndpointPolicy<wsl::shared::hns::PortnameEndpointPolicySetting> endpointPortNamePolicy{};
    endpointPortNamePolicy.Type = wsl::shared::hns::EndpointPolicyType::PortName;
    hnsEndpoint.Policies.emplace_back(std::move(endpointPortNamePolicy));
    m_endpoint = wsl::core::networking::CreateEphemeralHcnEndpoint(network.get(), hnsEndpoint);

    hcs::ModifySettingRequest<hcs::NetworkAdapter> networkRequest{};
    networkRequest.Settings.MacAddress = m_config.MacAddress;
    networkRequest.Settings.EndpointId = m_endpoint.Id;
    networkRequest.Settings.InstanceId = m_endpoint.Id;
    networkRequest.RequestType = hcs::ModifyRequestType::Add;
    networkRequest.ResourcePath = networking::c_networkAdapterPrefix +
                                  wsl::shared::string::GuidToString<wchar_t>(m_endpoint.Id, wsl::shared::string::GuidToStringFlags::None);

    windows::common::hcs::ModifyComputeSystem(m_system, wsl::shared::ToJsonW(networkRequest).c_str());
}

void BridgedNetworking::TraceLoggingRundown() noexcept
{
    // No-op.
}

void BridgedNetworking::FillInitialConfiguration(LX_MINI_INIT_NETWORKING_CONFIGURATION& message)
{
    message.NetworkingMode = LxMiniInitNetworkingModeBridged;
    message.DisableIpv6 = !m_config.EnableIpv6;
    message.EnableDhcpClient = m_config.EnableDhcp;
    message.DhcpTimeout = static_cast<int>(std::round(m_config.DhcpTimeout / 1000));
    message.PortTrackerType = m_config.EnableLocalhostRelay ? LxMiniInitPortTrackerTypeRelay : LxMiniInitPortTrackerTypeNone;
}

void BridgedNetworking::StartPortTracker(wil::unique_socket&&)
{
    WI_ASSERT(false);
}