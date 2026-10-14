// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "OpenVmmNatNetworking.h"
#include "WslCoreHostDnsInfo.h"
#include "WslCoreNetworkingSupport.h"

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

VmNetworkAdapterRequest wsl::core::CreateOpenVmmLoopbackNetworkAdapterRequest()
{
    VmUserModeNatNetwork network;
    constexpr std::array<BYTE, 6> clientMac{0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
    std::copy(clientMac.begin(), clientMac.end(), std::begin(network.Configuration.clientMac.bytes));
    network.InternalDns = false;
    network.ClientIp = L"127.0.0.1";
    network.GatewayIp = L"169.254.73.249";
    network.NetMask = L"255.255.255.240";
    network.AllowHostLocalAccess = true;
    return {L"loopback0", std::move(network)};
}

OpenVmmNatNetworking::OpenVmmNatNetworking(
    IVirtualMachineBackend& Backend,
    const VmNetworkAttachment& Adapter,
    std::optional<VmNetworkAttachment> LoopbackAdapter,
    GnsChannel&& GnsChannel,
    bool EnableLocalhostRelay,
    bool EnableDnsTunneling) :
    m_backend(Backend),
    m_device(Adapter.Id),
    m_adapterId(Adapter.GuestInstanceId.value()),
    m_gnsChannel(std::move(GnsChannel)),
    m_enableLocalhostRelay(EnableLocalhostRelay),
    m_enableDnsTunneling(EnableDnsTunneling)
{
    THROW_HR_IF(E_UNEXPECTED, EnableLocalhostRelay && !LoopbackAdapter.has_value());
    if (LoopbackAdapter.has_value())
    {
        m_loopbackDevice = LoopbackAdapter->Id;
        m_loopbackAdapterId = LoopbackAdapter->GuestInstanceId.value();
    }
}

OpenVmmNatNetworking::~OpenVmmNatNetworking()
{
    m_gnsChannel.Stop();
}

void OpenVmmNatNetworking::Initialize()
{
    if (m_enableLocalhostRelay)
    {
        hns::HNSEndpoint loopbackEndpointProperties{};
        loopbackEndpointProperties.ID = m_loopbackAdapterId.value();
        loopbackEndpointProperties.IPAddress = L"169.254.73.250";
        loopbackEndpointProperties.PrefixLength = 28;
        loopbackEndpointProperties.PortFriendlyName = L"loopback0";
        m_gnsChannel.SendEndpointState(loopbackEndpointProperties);

        hns::CreateDeviceRequest createLoopbackDevice{};
        createLoopbackDevice.deviceName = L"loopback0";
        createLoopbackDevice.type = hns::DeviceType::Loopback;
        createLoopbackDevice.lowerEdgeAdapterId = m_loopbackAdapterId.value();
        createLoopbackDevice.flags = hns::CreateDeviceFlags::DisableDAD;
        constexpr auto loopbackType = wsl::core::networking::GnsMessageType(createLoopbackDevice);
        m_gnsChannel.SendNetworkDeviceMessage(loopbackType, ToJsonW(createLoopbackDevice).c_str());
    }

    // Rename the loopback NIC first in case Linux initially assigned it eth0.
    hns::HNSEndpoint endpointProperties{};
    endpointProperties.ID = m_adapterId;
    endpointProperties.IPAddress = c_guestIpv4Address;
    endpointProperties.PrefixLength = c_guestIpv4PrefixLength;
    endpointProperties.PortFriendlyName = L"eth0";
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
        dns = wsl::core::networking::HostDnsInfo::GetDnsSettings(
            wsl::core::networking::DnsSettingsFlags::IncludeVpn | wsl::core::networking::DnsSettingsFlags::IncludeIpv6Servers);
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
    Message.EnableLoopbackRouting = false;
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

void OpenVmmNatNetworking::HandlePortNotification(const SOCKADDR_INET& addr, int protocol, bool allocate)
{
    THROW_HR_IF_MSG(
        HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED),
        protocol != IPPROTO_TCP && protocol != IPPROTO_UDP,
        "Unsupported bind protocol %d",
        protocol);

    if (addr.si_family != AF_INET && addr.si_family != AF_INET6)
    {
        return;
    }

    const bool ipv4 = addr.si_family == AF_INET;
    const bool loopback = ipv4 ? IN4_IS_ADDR_LOOPBACK(&addr.Ipv4.sin_addr) : IN6_IS_ADDR_LOOPBACK(&addr.Ipv6.sin6_addr);
    const bool unspecified = ipv4 ? IN4_IS_ADDR_UNSPECIFIED(&addr.Ipv4.sin_addr) : IN6_IS_ADDR_UNSPECIFIED(&addr.Ipv6.sin6_addr);
    if (!loopback && !unspecified)
    {
        // A bind to a specific guest address isn't a valid host address.
        return;
    }

    if (loopback && (!m_enableLocalhostRelay || (ipv4 && addr.Ipv4.sin_addr.s_addr != htonl(INADDR_LOOPBACK))))
    {
        // Only 127.0.0.1 and ::1 are relayed; any other loopback address stays on 'lo'.
        return;
    }

    auto lock = m_lock.lock_exclusive();

    // Relay host loopback connections through loopback0, which delivers them to the guest's loopback address.
    std::optional<SOCKADDR_INET> hostLoopback;
    if (m_enableLocalhostRelay)
    {
        hostLoopback = addr;
        if (ipv4)
        {
            hostLoopback->Ipv4.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        }
        else
        {
            hostLoopback->Ipv6.sin6_addr = in6addr_loopback;
            hostLoopback->Ipv6.sin6_scope_id = 0;
        }

        ModifyPort(true, hostLoopback.value(), protocol, allocate);
    }

    auto rollback = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&] {
        if (allocate && hostLoopback.has_value())
        {
            ModifyPort(true, hostLoopback.value(), protocol, false);
        }
    });

    if (!loopback)
    {
        ModifyPort(false, addr, protocol, allocate);
    }

    rollback.release();
}

void OpenVmmNatNetworking::ModifyPort(bool loopbackDevice, const SOCKADDR_INET& hostAddress, int protocol, bool allocate)
{
    // The GNS channel stores the port in host byte order, so no conversion is needed.
    const bool ipv4 = hostAddress.si_family == AF_INET;
    const auto port = ipv4 ? hostAddress.Ipv4.sin_port : hostAddress.Ipv6.sin6_port;
    const PortKey key{loopbackDevice, hostAddress.si_family, protocol, port};

    if (!allocate)
    {
        const auto binding = m_portBindings.find(key);
        if (binding != m_portBindings.end())
        {
            m_backend.UnbindPort(binding->second);
            m_portBindings.erase(binding);
        }

        return;
    }

    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), m_portBindings.contains(key));

    VmPortBindingRequest request{};
    request.Protocol = (protocol == IPPROTO_UDP) ? TransportProtocol_Udp : TransportProtocol_Tcp;
    if (ipv4)
    {
        request.ListenAddress.family = IpAddressFamily_V4;
        std::copy_n(
            reinterpret_cast<const BYTE*>(&hostAddress.Ipv4.sin_addr),
            sizeof(hostAddress.Ipv4.sin_addr),
            std::begin(request.ListenAddress.bytes));
    }
    else
    {
        request.ListenAddress.family = IpAddressFamily_V6;
        std::copy_n(hostAddress.Ipv6.sin6_addr.u.Byte, sizeof(hostAddress.Ipv6.sin6_addr), std::begin(request.ListenAddress.bytes));
        request.ListenScopeId = hostAddress.Ipv6.sin6_scope_id;
    }

    request.HostPort = port;
    request.GuestPort = port;
    m_portBindings.emplace(key, m_backend.BindPort(loopbackDevice ? m_loopbackDevice.value() : m_device, request).Id);
}
