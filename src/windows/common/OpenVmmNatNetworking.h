// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include "GnsChannel.h"
#include "GnsPortTrackerChannel.h"
#include "INetworkingEngine.h"
#include "IVirtualMachineBackend.h"

namespace wsl::core {

VmNetworkAdapterRequest CreateOpenVmmNatNetworkAdapterRequest(bool EnableDnsTunneling);

class OpenVmmNatNetworking : public INetworkingEngine
{
public:
    OpenVmmNatNetworking(IVirtualMachineBackend& Backend, VmDeviceId Device, GnsChannel&& GnsChannel, bool EnableLocalhostRelay, int DhcpTimeout);
    ~OpenVmmNatNetworking() override;

    void Initialize() override;
    void TraceLoggingRundown() noexcept override;
    void FillInitialConfiguration(LX_MINI_INIT_NETWORKING_CONFIGURATION& Message) override;
    void StartPortTracker(wil::unique_socket&& Socket) override;

private:
    using PortKey = std::tuple<int, int, uint16_t, bool>; // address family, protocol, guest port, loopback

    int HandlePortNotification(const SOCKADDR_INET& addr, int protocol, bool allocate);

    IVirtualMachineBackend& m_backend;
    VmDeviceId m_device;
    GnsChannel m_gnsChannel;
    bool m_enableLocalhostRelay;
    int m_dhcpTimeout;
    wil::srwlock m_lock;
    _Guarded_by_(m_lock) std::map<PortKey, VmPortBindingId> m_portBindings;
    std::optional<GnsPortTrackerChannel> m_gnsPortTrackerChannel;
};

} // namespace wsl::core
