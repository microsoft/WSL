// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include "GnsChannel.h"
#include "INetworkingEngine.h"
#include "IVirtualMachineBackend.h"

namespace wsl::core {

VmNetworkAdapterRequest CreateOpenVmmNatNetworkAdapterRequest();

class OpenVmmNatNetworking : public INetworkingEngine
{
public:
    OpenVmmNatNetworking(GnsChannel&& GnsChannel, bool EnableLocalhostRelay, int DhcpTimeout);
    ~OpenVmmNatNetworking() override;

    void Initialize() override;
    void TraceLoggingRundown() noexcept override;
    void FillInitialConfiguration(LX_MINI_INIT_NETWORKING_CONFIGURATION& Message) override;
    void StartPortTracker(wil::unique_socket&& Socket) override;

private:
    GnsChannel m_gnsChannel;
    bool m_enableLocalhostRelay;
    int m_dhcpTimeout;
};

} // namespace wsl::core
