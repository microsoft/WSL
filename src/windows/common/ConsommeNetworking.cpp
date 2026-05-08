// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "ConsommeNetworking.h"

using wsl::core::ConsommeNetworking;

ConsommeNetworking::ConsommeNetworking(bool enableLocalhostRelay) : m_enableLocalhostRelay(enableLocalhostRelay)
{
}

void ConsommeNetworking::Initialize()
{
    // No host-side initialization needed. Consomme configures NAT, DHCP, and
    // DNS inside the VMM process at VM boot time.
    WSL_LOG("ConsommeNetworking::Initialize");
}

void ConsommeNetworking::TraceLoggingRundown() noexcept
{
    WSL_LOG(
        "ConsommeNetworking::TraceLoggingRundown",
        TraceLoggingValue("Consomme", "NetworkingMode"),
        TraceLoggingValue(m_enableLocalhostRelay, "LocalhostRelay"));
}

void ConsommeNetworking::FillInitialConfiguration(LX_MINI_INIT_NETWORKING_CONFIGURATION& message)
{
    // Consomme provides a built-in DHCP server that assigns the guest an IP
    // address, gateway, and DNS server. The guest just needs to run a DHCP
    // client on the virtio-net interface.
    message.NetworkingMode = LxMiniInitNetworkingModeNat;
    message.EnableDhcpClient = true;
    message.DisableIpv6 = false;
    message.PortTrackerType = m_enableLocalhostRelay ? LxMiniInitPortTrackerTypeRelay : LxMiniInitPortTrackerTypeNone;
}

void ConsommeNetworking::StartPortTracker(wil::unique_socket&& socket)
{
    // Port tracking with consomme is handled via the standard relay mechanism.
    // No additional host-side port tracker setup is needed here.
    WSL_LOG("ConsommeNetworking::StartPortTracker", TraceLoggingValue("no-op", "Status"));
}
