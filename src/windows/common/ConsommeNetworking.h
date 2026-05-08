// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include "INetworkingEngine.h"

namespace wsl::core {

// Default network parameters for consomme's built-in NAT/DHCP.
// These match the consomme backend defaults in OpenVMM.
constexpr auto c_consommeGuestIp = "10.0.0.2";
constexpr auto c_consommeGatewayIp = "10.0.0.1";
constexpr auto c_consommeSubnetMask = "24";
constexpr auto c_consommeInterface = "eth0";

// Networking engine for OpenVMM's built-in consomme NAT backend.
//
// Unlike NatNetworking and VirtioNetworking which manage the guest's network
// configuration from the host via GNS, consomme handles NAT, DHCP, and DNS
// entirely within the VMM process. The host-side engine is therefore minimal:
// it tells the guest to use DHCP and optionally enables localhost port relay.
class ConsommeNetworking final : public INetworkingEngine
{
public:
    explicit ConsommeNetworking(bool enableLocalhostRelay);
    ~ConsommeNetworking() override = default;

    ConsommeNetworking(const ConsommeNetworking&) = delete;
    ConsommeNetworking(ConsommeNetworking&&) = delete;
    ConsommeNetworking& operator=(const ConsommeNetworking&) = delete;
    ConsommeNetworking& operator=(ConsommeNetworking&&) = delete;

    void Initialize() override;
    void TraceLoggingRundown() noexcept override;
    void FillInitialConfiguration(LX_MINI_INIT_NETWORKING_CONFIGURATION& message) override;
    void StartPortTracker(wil::unique_socket&& socket) override;

private:
    bool m_enableLocalhostRelay{};
};

} // namespace wsl::core
