// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "OpenVmmNatNetworking.h"

using wsl::core::OpenVmmNatNetworking;

VmNetworkAdapterRequest wsl::core::CreateOpenVmmNatNetworkAdapterRequest(bool EnableDnsTunneling)
{
    VmUserModeNatNetwork network;
    network.InternalDns = EnableDnsTunneling;
    constexpr std::array<BYTE, 6> clientMac{0x00, 0x00, 0x00, 0x00, 0x01, 0x00};
    std::copy(clientMac.begin(), clientMac.end(), std::begin(network.Configuration.clientMac.bytes));
    return {L"eth0", std::move(network)};
}

OpenVmmNatNetworking::OpenVmmNatNetworking(IVirtualMachineBackend& Backend, VmDeviceId Device, GnsChannel&& GnsChannel, bool EnableLocalhostRelay, int DhcpTimeout) :
    m_backend(Backend), m_device(Device), m_gnsChannel(std::move(GnsChannel)), m_enableLocalhostRelay(EnableLocalhostRelay), m_dhcpTimeout(DhcpTimeout)
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
    Message.DisableIpv6 = false;
    Message.EnableDhcpClient = true;
    Message.DhcpTimeout = static_cast<int>(std::round(m_dhcpTimeout / 1000.0));
    Message.PortTrackerType = LxMiniInitPortTrackerTypeMirrored;
}

void OpenVmmNatNetworking::StartPortTracker(wil::unique_socket&& Socket)
{
    WI_ASSERT(!m_gnsPortTrackerChannel.has_value());

    m_gnsPortTrackerChannel.emplace(
        std::move(Socket),
        [this](const SOCKADDR_INET& addr, int protocol, bool allocate) {
            return wil::ResultFromException([&]() { HandlePortNotification(addr, protocol, allocate); });
        },
        [](const std::string&, bool) {});
}

int OpenVmmNatNetworking::HandlePortNotification(const SOCKADDR_INET& addr, int protocol, bool allocate)
{
    THROW_HR_IF_MSG(
        HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED),
        protocol != IPPROTO_TCP && protocol != IPPROTO_UDP,
        "Unsupported bind protocol %d",
        protocol);

    if (addr.si_family != AF_INET && addr.si_family != AF_INET6)
    {
        return 0;
    }

    const bool ipv4 = addr.si_family == AF_INET;
    const bool loopback = ipv4 ? IN4_IS_ADDR_LOOPBACK(&addr.Ipv4.sin_addr) : IN6_IS_ADDR_LOOPBACK(&addr.Ipv6.sin6_addr);
    const bool unspecified = ipv4 ? IN4_IS_ADDR_UNSPECIFIED(&addr.Ipv4.sin_addr) : IN6_IS_ADDR_UNSPECIFIED(&addr.Ipv6.sin6_addr);
    if (!loopback && !unspecified)
    {
        // A bind to a specific guest address (e.g. the DHCP client on the guest's own IP) isn't a valid host address.
        return 0;
    }

    if (loopback)
    {
        // Only 127.0.0.1 and ::1 are intercepted; any other IPv4 loopback address stays on 'lo'.
        if (!m_enableLocalhostRelay || (ipv4 && addr.Ipv4.sin_addr.s_addr != htonl(INADDR_LOOPBACK)))
        {
            return 0;
        }
    }

    // The GNS channel stores the port in host byte order, so no conversion is needed.
    const auto port = ipv4 ? addr.Ipv4.sin_port : addr.Ipv6.sin6_port;
    const PortKey key{addr.si_family, protocol, port, loopback};
    auto lock = m_lock.lock_exclusive();

    if (!allocate)
    {
        const auto binding = m_portBindings.find(key);
        if (binding != m_portBindings.end())
        {
            m_backend.UnbindPort(binding->second);
            m_portBindings.erase(binding);
        }

        return 0;
    }

    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), m_portBindings.contains(key));

    VmPortBindingRequest request{};
    request.Protocol = (protocol == IPPROTO_UDP) ? TransportProtocol_Udp : TransportProtocol_Tcp;
    if (ipv4)
    {
        request.ListenAddress.family = IpAddressFamily_V4;
        std::copy_n(
            reinterpret_cast<const BYTE*>(&addr.Ipv4.sin_addr), sizeof(addr.Ipv4.sin_addr), std::begin(request.ListenAddress.bytes));
    }
    else
    {
        request.ListenAddress.family = IpAddressFamily_V6;
        std::copy_n(addr.Ipv6.sin6_addr.u.Byte, sizeof(addr.Ipv6.sin6_addr), std::begin(request.ListenAddress.bytes));
        request.ListenScopeId = addr.Ipv6.sin6_scope_id;
    }

    request.HostPort = port;
    request.GuestPort = port;

    m_portBindings.emplace(key, m_backend.BindPort(m_device, request).Id);
    return 0;
}
