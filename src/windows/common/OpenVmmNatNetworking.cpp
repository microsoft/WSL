// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "OpenVmmNatNetworking.h"
#include "WslCoreHostDnsInfo.h"

using wsl::core::OpenVmmNatNetworking;
using namespace wsl::shared;

namespace {

constexpr auto c_guestIpv4Address = L"10.0.0.2";
constexpr auto c_guestIpv4PrefixLength = 24;
constexpr auto c_gatewayIpv4Address = L"10.0.0.1";

} // namespace

VmNetworkAdapterRequest wsl::core::CreateOpenVmmNatNetworkAdapterRequest(bool EnableDnsTunneling)
{
    VmUserModeNatNetwork network;
    network.InternalDns = EnableDnsTunneling;
    constexpr std::array<BYTE, 6> clientMac{0x00, 0x00, 0x00, 0x00, 0x01, 0x00};
    std::copy(clientMac.begin(), clientMac.end(), std::begin(network.Configuration.clientMac.bytes));
    return {L"eth0", std::move(network)};
}

OpenVmmNatNetworking::OpenVmmNatNetworking(
    IVirtualMachineBackend& Backend,
    VmDeviceId Device,
    GUID AdapterId,
    GnsChannel&& GnsChannel,
    bool EnableLocalhostRelay,
    bool EnableDnsTunneling) :
    m_backend(Backend),
    m_device(Device),
    m_adapterId(AdapterId),
    m_gnsChannel(std::move(GnsChannel)),
    m_enableLocalhostRelay(EnableLocalhostRelay),
    m_enableDnsTunneling(EnableDnsTunneling)
{
}

OpenVmmNatNetworking::~OpenVmmNatNetworking()
{
    m_gnsChannel.Stop();
}

void OpenVmmNatNetworking::Initialize()
{
    hns::HNSEndpoint endpointProperties{};
    endpointProperties.ID = m_adapterId;
    endpointProperties.IPAddress = c_guestIpv4Address;
    endpointProperties.PrefixLength = c_guestIpv4PrefixLength;
    m_gnsChannel.SendEndpointState(endpointProperties);

    hns::Route route{};
    route.NextHop = c_gatewayIpv4Address;
    route.DestinationPrefix = LX_INIT_DEFAULT_ROUTE_PREFIX;
    route.Family = AF_INET;

    hns::ModifyGuestEndpointSettingRequest<hns::Route> routeNotification{};
    routeNotification.RequestType = hns::ModifyRequestType::Add;
    routeNotification.ResourceType = hns::GuestEndpointResourceType::Route;
    routeNotification.Settings = route;
    m_gnsChannel.SendHnsNotification(ToJsonW(routeNotification).c_str(), m_adapterId);

    wsl::core::networking::DnsInfo dns;
    if (m_enableDnsTunneling)
    {
        dns = wsl::core::networking::HostDnsInfo::GetDnsTunnelingSettings(c_gatewayIpv4Address);
    }
    else
    {
        dns = wsl::core::networking::HostDnsInfo::GetDnsSettings(wsl::core::networking::DnsSettingsFlags::IncludeVpn);
    }

    hns::ModifyGuestEndpointSettingRequest<hns::DNS> dnsNotification{};
    dnsNotification.RequestType = hns::ModifyRequestType::Update;
    dnsNotification.ResourceType = hns::GuestEndpointResourceType::DNS;
    dnsNotification.Settings = wsl::core::networking::BuildDnsNotification(dns);
    m_gnsChannel.SendHnsNotification(ToJsonW(dnsNotification).c_str(), m_adapterId);
}

void OpenVmmNatNetworking::TraceLoggingRundown() noexcept
{
    WSL_LOG(
        "OpenVmmNatNetworking::TraceLoggingRundown",
        TraceLoggingValue(m_enableLocalhostRelay, "localhostRelay"),
        TraceLoggingValue(m_enableDnsTunneling, "dnsTunneling"));
}

void OpenVmmNatNetworking::FillInitialConfiguration(LX_MINI_INIT_NETWORKING_CONFIGURATION& Message)
{
    Message.NetworkingMode = LxMiniInitNetworkingModeNat;
    Message.DisableIpv6 = false;
    Message.EnableDhcpClient = false;
    Message.PortTrackerType = LxMiniInitPortTrackerTypeMirrored;
    Message.EnableLoopbackRouting = m_enableLocalhostRelay;
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
        // A bind to a specific guest address isn't a valid host address.
        return 0;
    }

    if (loopback)
    {
        // IPv6 loopback cannot be routed through eth0; any other IPv4 loopback address stays on 'lo'.
        if (!m_enableLocalhostRelay || !ipv4 || addr.Ipv4.sin_addr.s_addr != htonl(INADDR_LOOPBACK))
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
    if (loopback)
    {
        request.GuestAddress.emplace();
        request.GuestAddress->family = IpAddressFamily_V4;
        std::copy_n(
            reinterpret_cast<const BYTE*>(&addr.Ipv4.sin_addr), sizeof(addr.Ipv4.sin_addr), std::begin(request.GuestAddress->bytes));
    }

    m_portBindings.emplace(key, m_backend.BindPort(m_device, request).Id);
    return 0;
}
