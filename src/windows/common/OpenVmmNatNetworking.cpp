// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "OpenVmmNatNetworking.h"

using wsl::core::OpenVmmNatNetworking;

VmNetworkAdapterRequest wsl::core::CreateOpenVmmNatNetworkAdapterRequest()
{
    VmUserModeNatNetwork network;
    constexpr std::array<BYTE, 6> clientMac{0x00, 0x00, 0x00, 0x00, 0x01, 0x00};
    std::copy(clientMac.begin(), clientMac.end(), std::begin(network.Configuration.clientMac.bytes));
    return {L"eth0", std::move(network)};
}

OpenVmmNatNetworking::OpenVmmNatNetworking(GnsChannel&& GnsChannel, bool EnableLocalhostRelay, int DhcpTimeout) :
    m_gnsChannel(std::move(GnsChannel)), m_enableLocalhostRelay(EnableLocalhostRelay), m_dhcpTimeout(DhcpTimeout)
{
}

OpenVmmNatNetworking::~OpenVmmNatNetworking()
{
    m_gnsChannel.Stop();
}

void OpenVmmNatNetworking::Initialize()
{
}

void OpenVmmNatNetworking::TraceLoggingRundown() noexcept
{
    WSL_LOG(
        "OpenVmmNatNetworking::TraceLoggingRundown",
        TraceLoggingValue(m_enableLocalhostRelay, "localhostRelay"),
        TraceLoggingValue(m_dhcpTimeout, "dhcpTimeout"));
}

void OpenVmmNatNetworking::FillInitialConfiguration(LX_MINI_INIT_NETWORKING_CONFIGURATION& Message)
{
    Message.NetworkingMode = LxMiniInitNetworkingModeNat;
    Message.DisableIpv6 = true;
    Message.EnableDhcpClient = true;
    Message.DhcpTimeout = static_cast<int>(std::round(m_dhcpTimeout / 1000.0));
    Message.PortTrackerType = m_enableLocalhostRelay ? LxMiniInitPortTrackerTypeRelay : LxMiniInitPortTrackerTypeNone;
}

void OpenVmmNatNetworking::StartPortTracker(wil::unique_socket&&)
{
    THROW_HR(E_NOTIMPL);
}
