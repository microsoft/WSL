// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include "INetworkingEngine.h"
#include "GnsChannel.h"
#include "WslCoreConfig.h"
#include "WslCoreNetworkEndpointSettings.h"

namespace wsl::core {

class BridgedNetworking : public INetworkingEngine
{
public:
    BridgedNetworking(const wsl::windows::common::hcs::shared_hcs_system& system, const Config& config);
    ~BridgedNetworking() override = default;

    void Initialize() override;
    void TraceLoggingRundown() noexcept override;
    void FillInitialConfiguration(LX_MINI_INIT_NETWORKING_CONFIGURATION& message) override;
    void StartPortTracker(wil::unique_socket&& socket) override;

private:
    // Handle for the Hcn* Api. Shared with WslCoreVm
    const wsl::windows::common::hcs::shared_hcs_system m_system;

    const Config& m_config;
    wsl::core::networking::EphemeralHcnEndpoint m_endpoint;
};

} // namespace wsl::core
