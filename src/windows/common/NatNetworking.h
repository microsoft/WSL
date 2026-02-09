// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include <netlistmgr.h>

#include "INetworkingEngine.h"
#include "GnsChannel.h"
#include "DnsResolver.h"
#include "WslCoreConfig.h"
#include "WslCoreNetworkEndpointSettings.h"
#include "WslCoreHostDnsInfo.h"
#include "WslCoreNetworkingSupport.h"
#include "hns_schema.h"

namespace wsl::core {

class NatNetworking final : public INetworkingEngine
{
public:
    NatNetworking(HCS_SYSTEM system, wsl::windows::common::hcs::unique_hcn_network&& network, GnsChannel&& gnsChannel, Config& config, wil::unique_socket&& dnsHvsocket);
    ~NatNetworking() override;

    // Note: This class cannot be moved because m_networkNotifyHandle captures a 'this' pointer.
    NatNetworking(const NatNetworking&) = delete;
    NatNetworking(NatNetworking&&) = delete;
    NatNetworking& operator=(const NatNetworking&) = delete;
    NatNetworking& operator=(NatNetworking&&) = delete;

    void Initialize() override;
    void TraceLoggingRundown() noexcept override;
    void FillInitialConfiguration(LX_MINI_INIT_NETWORKING_CONFIGURATION& message) override;
    void StartPortTracker(wil::unique_socket&& socket) override;

    static wsl::windows::common::hcs::unique_hcn_network CreateNetwork(wsl::core::Config& config);
    static bool IsHyperVFirewallSupported(const wsl::core::Config& vmConfig) noexcept;

private:
    static void NETIOAPI_API_ OnNetworkConnectivityChange(PVOID context, NL_NETWORK_CONNECTIVITY_HINT hint);
    static std::optional<ULONGLONG> FindNatInterfaceLuid(const SOCKADDR_INET& natAddress, const NL_NETWORK_CONNECTIVITY_HINT& currentConnectivityHint);
    std::pair<networking::EphemeralHcnEndpoint, wsl::shared::hns::HNSEndpoint> CreateEndpoint(const std::wstring& IpAddress) const;
    static wsl::windows::common::hcs::unique_hcn_network CreateNetworkInternal(wsl::core::Config& config);

    void RefreshGuestConnection(NL_NETWORK_CONNECTIVITY_HINT connectivityHint) noexcept;
    _Requires_lock_held_(m_lock)
    void UpdateDns(std::optional<PCWSTR> gatewayAddress = std::nullopt) noexcept;
    void UpdateMtu();
    void AttachEndpoint(networking::EphemeralHcnEndpoint&& endpoint, const wsl::shared::hns::HNSEndpoint& properties);
    void TelemetryConnectionCallback(NLM_CONNECTIVITY hostConnectivity, uint32_t telemetryCounter) noexcept;

    mutable wil::srwlock m_lock;

    // Handle for the Hcn* Api. Owned by the caller (WslCoreVm), this is a non-owning copy
    const HCS_SYSTEM m_system{};
    Config& m_config;
    wsl::windows::common::hcs::unique_hcn_network m_network{};

    bool m_connectivityTelemetryEnabled{false};
    wsl::core::networking::ConnectivityTelemetry m_connectivityTelemetry;

    // Optional DNS resolver used for DNS tunneling
    std::optional<networking::DnsResolver> m_dnsTunnelingResolver;

    std::wstring m_dnsTunnelingIpAddress;

    std::chrono::time_point<std::chrono::steady_clock> m_objectCreationTime = std::chrono::steady_clock::now();

    std::optional<networking::DnsSuffixRegistryWatcher> m_dnsSuffixRegistryWatcher;
    // The latest DNS settings configured in Linux
    _Guarded_by_(m_lock) networking::DnsInfo m_trackedDnsSettings {};

    GnsChannel m_gnsChannel;
    std::shared_ptr<networking::NetworkSettings> m_networkSettings;
    networking::EphemeralHcnEndpoint m_endpoint;
    ULONG m_networkMtu = 0;

    std::optional<networking::HostDnsInfo> m_mirrorDnsInfo;
    networking::unique_notify_handle m_networkNotifyHandle{};
};

} // namespace wsl::core
