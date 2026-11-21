// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "OpenVmmConsommeNetworking.h"
#include "WslCoreHostDnsInfo.h"
#include "WslCoreNetworkingSupport.h"

using wsl::core::OpenVmmConsommeNetworking;
using namespace wsl::shared;

namespace {

constexpr auto c_guestIpv4Address = L"10.0.0.2";
constexpr auto c_guestIpv4PrefixLength = 24;
constexpr auto c_gatewayIpv4Address = L"10.0.0.1";

} // namespace

VmNetworkAdapterRequest wsl::core::CreateOpenVmmConsommeNetworkAdapterRequest(bool EnableDnsTunneling, const wsl::shared::string::MacAddress& ClientMac)
{
    VmUserModeNatNetwork network;
    network.InternalDns = EnableDnsTunneling;
    std::copy(ClientMac.begin(), ClientMac.end(), std::begin(network.Configuration.clientMac.bytes));
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

void wsl::core::MapHostLoopbackDnsServers(networking::DnsInfo& Dns, IVirtualMachineBackend& Backend, VmDeviceId Adapter)
{
    for (auto& server : Dns.Servers)
    {
        const auto address = wsl::windows::common::string::StringToSockAddrInet(wsl::shared::string::MultiByteToWide(server));
        IpAddress destination{};
        if (address.si_family == AF_INET && IN4_IS_ADDR_LOOPBACK(&address.Ipv4.sin_addr))
        {
            destination.family = IpAddressFamily_V4;
            std::memcpy(destination.bytes, &address.Ipv4.sin_addr, sizeof(address.Ipv4.sin_addr));
        }
        else if (address.si_family == AF_INET6 && IN6_IS_ADDR_V4MAPPED(&address.Ipv6.sin6_addr))
        {
            IN_ADDR mapped{};
            std::memcpy(&mapped, &address.Ipv6.sin6_addr.u.Byte[12], sizeof(mapped));
            if (!IN4_IS_ADDR_LOOPBACK(&mapped))
            {
                continue;
            }
            destination.family = IpAddressFamily_V4;
            std::memcpy(destination.bytes, &mapped, sizeof(mapped));
        }
        else if (address.si_family == AF_INET6 && IN6_IS_ADDR_LOOPBACK(&address.Ipv6.sin6_addr))
        {
            destination.family = IpAddressFamily_V6;
            std::memcpy(destination.bytes, &address.Ipv6.sin6_addr, sizeof(address.Ipv6.sin6_addr));
        }
        else
        {
            continue;
        }

        const auto mapped = Backend.CreateVirtualAddress(Adapter, destination);
        server = wsl::shared::string::WideToMultiByte(wsl::windows::common::string::IpAddressToWstring(mapped));
        if (mapped.family == IpAddressFamily_V6)
        {
            server += "%eth0";
        }
    }
}

OpenVmmConsommeNetworking::OpenVmmConsommeNetworking(
    IVirtualMachineBackend& Backend,
    const VmNetworkAttachment& Adapter,
    std::optional<VmNetworkAttachment> LoopbackAdapter,
    GnsChannel&& GnsChannel,
    bool EnableLocalhostRelay,
    bool EnableDnsTunneling,
    bool EnableIpv6) :
    m_backend(Backend),
    m_device(Adapter.Id),
    m_adapterId(Adapter.GuestInstanceId.value()),
    m_gnsChannel(std::move(GnsChannel)),
    m_enableLocalhostRelay(EnableLocalhostRelay),
    m_enableDnsTunneling(EnableDnsTunneling),
    m_enableIpv6(EnableIpv6)
{
    THROW_HR_IF(E_UNEXPECTED, EnableLocalhostRelay && !LoopbackAdapter.has_value());
    if (LoopbackAdapter.has_value())
    {
        m_loopbackDevice = LoopbackAdapter->Id;
        m_loopbackAdapterId = LoopbackAdapter->GuestInstanceId.value();
    }
}

OpenVmmConsommeNetworking::~OpenVmmConsommeNetworking()
{
    m_gnsChannel.Stop();
    m_networkNotifyHandle.reset();
}

void OpenVmmConsommeNetworking::Initialize()
{
    auto lock = m_dnsLock.lock_exclusive();
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

    UpdateDns();
    if (!m_enableDnsTunneling)
    {
        THROW_IF_WIN32_ERROR(NotifyNetworkConnectivityHintChange(
            &OpenVmmConsommeNetworking::OnNetworkConnectivityChange, this, true, &m_networkNotifyHandle));
    }
}

void NETIOAPI_API_ OpenVmmConsommeNetworking::OnNetworkConnectivityChange(PVOID Context, NL_NETWORK_CONNECTIVITY_HINT)
{
    static_cast<OpenVmmConsommeNetworking*>(Context)->RefreshDns();
}

void OpenVmmConsommeNetworking::RefreshDns() noexcept
try
{
    auto lock = m_dnsLock.lock_exclusive();
    UpdateDns();
}
CATCH_LOG()

void OpenVmmConsommeNetworking::UpdateDns()
{
    networking::DnsInfo dns;
    if (m_enableDnsTunneling)
    {
        dns = networking::HostDnsInfo::GetDnsTunnelingSettings(c_gatewayIpv4Address);
    }
    else
    {
        dns = networking::HostDnsInfo::GetDnsSettings(networking::DnsSettingsFlags::IncludeVpn | networking::DnsSettingsFlags::IncludeIpv6Servers);
        MapHostLoopbackDnsServers(dns, m_backend, m_device);
    }

    hns::ModifyGuestEndpointSettingRequest<hns::DNS> dnsNotification{};
    dnsNotification.RequestType = hns::ModifyRequestType::Update;
    dnsNotification.ResourceType = hns::GuestEndpointResourceType::DNS;
    dnsNotification.Settings = networking::BuildDnsNotification(dns);
    m_gnsChannel.SendHnsNotification(ToJsonW(dnsNotification).c_str(), m_adapterId);
}

void OpenVmmConsommeNetworking::TraceLoggingRundown() noexcept
{
    WSL_LOG(
        "OpenVmmConsommeNetworking::TraceLoggingRundown",
        TraceLoggingValue(m_enableLocalhostRelay, "localhostRelay"),
        TraceLoggingValue(m_enableDnsTunneling, "dnsTunneling"),
        TraceLoggingValue(m_enableIpv6, "ipv6"));
}

void OpenVmmConsommeNetworking::FillInitialConfiguration(LX_MINI_INIT_NETWORKING_CONFIGURATION& Message)
{
    Message.NetworkingMode = LxMiniInitNetworkingModeNat;
    Message.DisableIpv6 = false;
    Message.EnableDhcpClient = false;
    Message.PortTrackerType = LxMiniInitPortTrackerTypeMirrored;
}

void OpenVmmConsommeNetworking::StartPortTracker(wil::unique_socket&& Socket)
{
    WI_ASSERT(!m_gnsPortTrackerChannel.has_value());

    m_gnsPortTrackerChannel.emplace(
        std::move(Socket),
        [this](const SOCKADDR_INET& addr, int protocol, bool allocate) {
            const auto result = wil::ResultFromException([&]() { HandlePortNotification(addr, protocol, allocate); });
            LOG_IF_FAILED_MSG(
                result,
                "Port mapping failed: address=%hs port=%u protocol=%d allocate=%d",
                wsl::windows::common::string::SockAddrInetToString(addr).c_str(),
                INETADDR_PORT(reinterpret_cast<const SOCKADDR*>(&addr)),
                protocol,
                allocate);

            switch (result)
            {
            case S_OK:
                return 0;

            case HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS):
            case HRESULT_FROM_WIN32(WSAEADDRINUSE):
            case E_ACCESSDENIED:
            case HRESULT_FROM_WIN32(WSAEACCES):
                return -LX_EADDRINUSE;

            default:
                return -LX_EINVAL;
            }
        },
        [](const std::string&, bool) {});
}

void OpenVmmConsommeNetworking::HandlePortNotification(SOCKADDR_INET addr, int protocol, bool allocate)
{
    THROW_HR_IF_MSG(
        HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED),
        protocol != IPPROTO_TCP && protocol != IPPROTO_UDP,
        "Unsupported bind protocol %d",
        protocol);

    if (addr.si_family == AF_INET6 && IN6_IS_ADDR_V4MAPPED(&addr.Ipv6.sin6_addr))
    {
        SOCKADDR_IN ipv4Address{};
        ipv4Address.sin_family = AF_INET;
        ipv4Address.sin_port = addr.Ipv6.sin6_port;
        memcpy(&ipv4Address.sin_addr, &addr.Ipv6.sin6_addr.u.Byte[12], sizeof(ipv4Address.sin_addr));
        addr = {};
        addr.Ipv4 = ipv4Address;
    }

    // Binds that reserve nothing on the host stay in the guest; returning success lets Linux validate them itself.
    if ((addr.si_family != AF_INET && addr.si_family != AF_INET6) || (addr.si_family == AF_INET6 && !m_enableIpv6))
    {
        return;
    }

    const bool ipv4 = addr.si_family == AF_INET;
    const bool loopback = ipv4 ? IN4_IS_ADDR_LOOPBACK(&addr.Ipv4.sin_addr) : IN6_IS_ADDR_LOOPBACK(&addr.Ipv6.sin6_addr);
    const bool unspecified = ipv4 ? IN4_IS_ADDR_UNSPECIFIED(&addr.Ipv4.sin_addr) : IN6_IS_ADDR_UNSPECIFIED(&addr.Ipv6.sin6_addr);

    // As with Consomme on HCS, only 127.0.0.1 is relayed among the loopback addresses. Unlike it, binds to specific
    // addresses also stay in the guest, since the guest's address isn't one of the host's.
    if ((loopback && (!ipv4 || addr.Ipv4.sin_addr.s_addr != htonl(INADDR_LOOPBACK))) || (!loopback && !unspecified))
    {
        return;
    }

    auto lock = m_lock.lock_exclusive();

    // Relay host 127.0.0.1 to the guest through loopback0. Linux drops inbound packets addressed to ::1 on non-loopback
    // interfaces, so IPv6 binds aren't relayed.
    std::optional<SOCKADDR_INET> relayAddress;
    if (m_enableLocalhostRelay && ipv4)
    {
        relayAddress = addr;
        relayAddress->Ipv4.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ModifyPort(true, *relayAddress, protocol, allocate);
    }

    // As with Consomme on HCS, a wildcard bind also reserves the port on the host's addresses, and forwards it to the guest.
    if (unspecified)
    {
        auto revertRelay = wil::scope_exit([&] {
            if (relayAddress && allocate)
            {
                LOG_IF_FAILED(wil::ResultFromException([&] { ModifyPort(true, *relayAddress, protocol, false); }));
            }
        });

        ModifyPort(false, addr, protocol, allocate);
        revertRelay.release();
    }
}

void OpenVmmConsommeNetworking::ModifyPort(bool loopbackDevice, const SOCKADDR_INET& hostAddress, int protocol, bool allocate)
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
