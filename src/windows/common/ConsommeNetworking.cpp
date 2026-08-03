// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "ConsommeNetworking.h"
#include "GuestDeviceManager.h"
#include "Stringify.h"
#include "stringshared.h"

using namespace wsl::core::networking;
using namespace wsl::shared;
using namespace wsl::windows::common::stringify;
using wsl::core::ConsommeNetworking;

static constexpr auto c_eth0DeviceName = L"eth0";
static constexpr auto c_loopbackDeviceName = TEXT(LX_INIT_LOOPBACK_DEVICE_NAME);
static constexpr wsl::shared::string::MacAddress c_defaultClientMacAddress{0x00, 0x00, 0x00, 0x00, 0x01, 0x00};
static constexpr wsl::shared::string::MacAddress c_gatewayMacAddress{0x00, 0x11, 0x22, 0x33, 0x44, 0x55};

namespace {

EthernetAddress ToEthernetAddress(const wsl::shared::string::MacAddress& address)
{
    EthernetAddress result{};
    std::copy(address.begin(), address.end(), std::begin(result.bytes));
    return result;
}

Ipv4Address ToIpv4Address(const SOCKADDR_INET& address)
{
    Ipv4Address result{};
    if (address.si_family == AF_INET)
    {
        result.value = address.Ipv4.sin_addr.S_un.S_addr;
    }

    return result;
}

Ipv6Address ToIpv6Address(const SOCKADDR_INET& address)
{
    Ipv6Address result{};
    if (address.si_family == AF_INET6)
    {
        std::copy(std::begin(address.Ipv6.sin6_addr.u.Byte), std::end(address.Ipv6.sin6_addr.u.Byte), std::begin(result.bytes));
    }

    return result;
}

IpAddress ToIpAddress(const SOCKADDR_INET& address)
{
    IpAddress result{};
    if (address.si_family == AF_INET)
    {
        result.family = IpAddressFamily_V4;
        std::copy(
            reinterpret_cast<const BYTE*>(&address.Ipv4.sin_addr),
            reinterpret_cast<const BYTE*>(&address.Ipv4.sin_addr) + sizeof(address.Ipv4.sin_addr),
            std::begin(result.bytes));
    }
    else if (address.si_family == AF_INET6)
    {
        result.family = IpAddressFamily_V6;
        std::copy(std::begin(address.Ipv6.sin6_addr.u.Byte), std::end(address.Ipv6.sin6_addr.u.Byte), std::begin(result.bytes));
    }

    return result;
}

std::vector<IpAddress> ToIpAddresses(const DnsInfo& dns)
{
    std::vector<IpAddress> result;
    result.reserve(dns.Servers.size());
    for (const auto& server : dns.Servers)
    {
        if (server.empty())
        {
            continue;
        }

        result.emplace_back(ToIpAddress(wsl::windows::common::string::StringToSockAddrInet(wsl::shared::string::MultiByteToWide(server))));
    }

    return result;
}

WslVirtioNetConfig BuildVirtioNetConfig(
    const std::shared_ptr<NetworkSettings>& networkSettings, bool enableIpv6, std::optional<wsl::shared::string::MacAddress> clientMacAddress = {})
{
    ULONG netmask{};
    if (networkSettings->PreferredIpAddress.Address.si_family == AF_INET)
    {
        LOG_IF_WIN32_ERROR(ConvertLengthToIpv4Mask(networkSettings->PreferredIpAddress.PrefixLength, &netmask));
    }

    WslVirtioNetConfig config{};
    config.clientIp = ToIpv4Address(networkSettings->PreferredIpAddress.Address);
    config.hasClientIpv6 = enableIpv6 && networkSettings->PreferredIpv6Address.Address.si_family == AF_INET6;
    config.clientIpv6 = ToIpv6Address(networkSettings->PreferredIpv6Address.Address);
    config.clientMac = ToEthernetAddress(clientMacAddress.value_or(c_defaultClientMacAddress));
    config.gatewayIp = ToIpv4Address(networkSettings->GetBestGatewayAddress());
    config.gatewayMac = ToEthernetAddress(c_gatewayMacAddress);
    config.gatewayMacIpv6 = ToEthernetAddress(c_gatewayMacAddress);
    config.netmask.value = netmask;
    return config;
}

} // namespace

ConsommeNetworking::ConsommeNetworking(
    GnsChannel&& gnsChannel, ConsommeNetworkingFlags flags, LPCWSTR dnsOptions, std::shared_ptr<GuestDeviceManager> guestDeviceManager, wil::shared_handle userToken) :
    m_guestDeviceManager(std::move(guestDeviceManager)),
    m_userToken(std::move(userToken)),
    m_gnsChannel(std::move(gnsChannel)),
    m_flags(flags),
    m_dnsOptions(dnsOptions)
{
}

ConsommeNetworking::~ConsommeNetworking()
{
    // Unregister the network notification callback to prevent it from using the GNS channel.
    m_networkNotifyHandle.reset();

    // Stop the GNS channel to unblock any stuck communications with the guest.
    m_gnsChannel.Stop();
}

void ConsommeNetworking::Initialize()
{
    // Initialize adapter state.
    RefreshGuestConnection();

    if (WI_IsFlagSet(m_flags, ConsommeNetworkingFlags::LocalhostRelay))
    {
        SetupLoopbackDevice();
    }

    THROW_IF_WIN32_ERROR(NotifyNetworkConnectivityHintChange(&ConsommeNetworking::OnNetworkConnectivityChange, this, TRUE, &m_networkNotifyHandle));
}

void ConsommeNetworking::TraceLoggingRundown() noexcept
{
    auto lock = m_lock.lock_exclusive();

    WSL_LOG("ConsommeNetworking::TraceLoggingRundown", TRACE_NETWORKSETTINGS_OBJECT(m_networkSettings));
}

void ConsommeNetworking::FillInitialConfiguration(LX_MINI_INIT_NETWORKING_CONFIGURATION& message)
{
    message.NetworkingMode = LxMiniInitNetworkingModeConsomme;
    message.DisableIpv6 = WI_IsFlagClear(m_flags, ConsommeNetworkingFlags::Ipv6);
    message.EnableDhcpClient = false;
    message.PortTrackerType = LX_MINI_INIT_PORT_TRACKER_TYPE::LxMiniInitPortTrackerTypeMirrored;
}

void ConsommeNetworking::StartPortTracker(wil::unique_socket&& socket)
{
    WI_ASSERT(!m_gnsPortTrackerChannel.has_value());

    m_gnsPortTrackerChannel.emplace(
        std::move(socket),
        [&](const SOCKADDR_INET& addr, int protocol, bool allocate) {
            return wil::ResultFromException([&]() {
                HandlePortNotification(addr, protocol, INETADDR_PORT(reinterpret_cast<const SOCKADDR*>(&addr)), allocate);
            });
        },
        [](const std::string&, bool) {}); // TODO: reconsider if InterfaceStateCallback is needed.
}

void NETIOAPI_API_ ConsommeNetworking::OnNetworkConnectivityChange(PVOID context, NL_NETWORK_CONNECTIVITY_HINT hint)
try
{
    static_cast<ConsommeNetworking*>(context)->RefreshGuestConnection();
}
CATCH_LOG()

uint16_t ConsommeNetworking::HandlePortNotification(const SOCKADDR_INET& addr, int protocol, uint16_t guestPort, bool allocate) const
{
    if (addr.si_family == AF_INET6 && WI_IsFlagClear(m_flags, ConsommeNetworkingFlags::Ipv6))
    {
        return 0;
    }

    const auto ipAddress = (addr.si_family == AF_INET) ? reinterpret_cast<const void*>(&addr.Ipv4.sin_addr)
                                                       : reinterpret_cast<const void*>(&addr.Ipv6.sin6_addr);
    const bool loopback = INET_IS_ADDR_LOOPBACK(addr.si_family, ipAddress);
    const bool unspecified = INET_IS_ADDR_UNSPECIFIED(addr.si_family, ipAddress);
    if (addr.si_family == AF_INET && loopback)
    {
        // Only intercepting 127.0.0.1; any other loopback address will remain on 'lo'.
        if (addr.Ipv4.sin_addr.s_addr != htonl(INADDR_LOOPBACK))
        {
            return 0;
        }
    }
    SOCKADDR_INET localAddr = addr;
    std::function<void()> removePort;

    auto hostPort = INETADDR_PORT(reinterpret_cast<const SOCKADDR*>(&addr));
    auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&removePort]() {
        if (removePort)
        {
            removePort();
        }
    });

    if (WI_IsFlagSet(m_flags, ConsommeNetworkingFlags::LocalhostRelay) && (unspecified || loopback))
    {
        if (!loopback)
        {
            INETADDR_SETLOOPBACK(reinterpret_cast<PSOCKADDR>(&localAddr));
            if (addr.si_family == AF_INET)
            {
                localAddr.Ipv4.sin_port = addr.Ipv4.sin_port;
            }
            else
            {
                localAddr.Ipv6.sin6_port = addr.Ipv6.sin6_port;
            }
        }

        hostPort = ModifyOpenPorts(c_loopbackDeviceName, localAddr, hostPort, guestPort, protocol, allocate);

        // Revert the change on failure.
        removePort = [&]() { ModifyOpenPorts(c_loopbackDeviceName, localAddr, hostPort, guestPort, protocol, !allocate); };
    }

    if (!loopback)
    {
        hostPort = ModifyOpenPorts(c_eth0DeviceName, addr, hostPort, guestPort, protocol, allocate);
    }

    cleanup.release();

    return hostPort;
}

uint16_t ConsommeNetworking::ModifyOpenPorts(
    _In_ PCWSTR tag, _In_ const SOCKADDR_INET& hostAddress, _In_ uint16_t HostPort, _In_ uint16_t GuestPort, _In_ int protocol, _In_ bool isOpen) const
{
    THROW_HR_IF_MSG(
        HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED),
        protocol != IPPROTO_TCP && protocol != IPPROTO_UDP,
        "Unsupported bind protocol %d",
        protocol);

    auto lock = m_lock.lock_exclusive();
    const auto device = m_guestDeviceManager->GetVirtioNetDevice(tag);
    const auto transportProtocol = (protocol == IPPROTO_UDP) ? TransportProtocol_Udp : TransportProtocol_Tcp;
    auto listenAddress = ToIpAddress(hostAddress);

    if (isOpen)
    {
        UINT16 allocatedPort{};
        THROW_IF_FAILED(device->BindPort(transportProtocol, &listenAddress, HostPort, GuestPort, &allocatedPort));
        WSL_LOG(
            "MapVirtioPort",
            TraceLoggingValue(tag, "Tag"),
            TraceLoggingValue(HostPort, "HostPort"),
            TraceLoggingValue(GuestPort, "GuestPort"));
        return allocatedPort;
    }

    THROW_IF_FAILED(device->UnbindPort(transportProtocol, listenAddress.family, GuestPort));
    WSL_LOG(
        "UnmapVirtioPort",
        TraceLoggingValue(tag, "Tag"),
        TraceLoggingValue(HostPort, "HostPort"),
        TraceLoggingValue(GuestPort, "GuestPort"));
    return HostPort;
}

HRESULT ConsommeNetworking::MapPort(_In_ const SOCKADDR_INET& ListenAddress, _In_ USHORT GuestPort, _In_ int Protocol, _Out_ USHORT* AllocatedHostPort) const
try
{
    RETURN_HR_IF(E_POINTER, AllocatedHostPort == nullptr);
    RETURN_HR_IF_MSG(E_INVALIDARG, Protocol != IPPROTO_TCP && Protocol != IPPROTO_UDP, "Invalid protocol: %i", Protocol);

    *AllocatedHostPort = 0;

    *AllocatedHostPort = HandlePortNotification(ListenAddress, Protocol, GuestPort, true);

    return S_OK;
}
CATCH_RETURN()

HRESULT ConsommeNetworking::UnmapPort(_In_ const SOCKADDR_INET& ListenAddress, _In_ USHORT GuestPort, _In_ int Protocol) const
try
{
    RETURN_HR_IF(E_INVALIDARG, Protocol != IPPROTO_TCP && Protocol != IPPROTO_UDP);

    HandlePortNotification(ListenAddress, Protocol, GuestPort, false);

    return S_OK;
}
CATCH_RETURN()

void ConsommeNetworking::RefreshGuestConnection()
{
    // Query current networking information before acquiring the lock.
    auto networkSettings = GetHostEndpointSettings();

    std::wstring default_route = networkSettings->GetBestGatewayAddressString();

    networking::DnsInfo currentDns{};
    if (WI_IsFlagSet(m_flags, ConsommeNetworkingFlags::DnsTunneling))
    {
        currentDns = networking::HostDnsInfo::GetDnsTunnelingSettings(default_route);
    }
    else
    {
        wsl::core::networking::DnsSettingsFlags dnsFlags = networking::DnsSettingsFlags::IncludeVpn;
        WI_SetFlagIf(dnsFlags, networking::DnsSettingsFlags::IncludeIpv6Servers, WI_IsFlagSet(m_flags, ConsommeNetworkingFlags::Ipv6));
        currentDns = networking::HostDnsInfo::GetDnsSettings(dnsFlags);
    }

    const auto minMtu = GetMinimumConnectedInterfaceMtu();
    const auto virtioNetConfig = BuildVirtioNetConfig(networkSettings, WI_IsFlagSet(m_flags, ConsommeNetworkingFlags::Ipv6));

    // Acquire the lock and perform device updates.
    auto lock = m_lock.lock_exclusive();

    // Add virtio net adapter to guest. Subsequent address/route/DNS changes are sent through GNS notifications below.
    if (!m_adapterId.has_value())
    {
        WSL_LOG(
            "RefreshVirtioNetConnection",
            TraceLoggingValue(networkSettings->PreferredIpAddress.AddressString.c_str(), "ClientIp"),
            TraceLoggingValue(networkSettings->PreferredIpAddress.PrefixLength, "PrefixLength"),
            TraceLoggingValue(default_route.c_str(), "GatewayIp"),
            TraceLoggingValue(networkSettings->PreferredIpv6Address.AddressString.c_str(), "ClientIpv6"));
        m_adapterId =
            m_guestDeviceManager->AddVirtioNetDevice(c_eth0DeviceName, virtioNetConfig, ToIpAddresses(currentDns), m_userToken.get());
    }

    UpdateIpv4Address(networkSettings->PreferredIpAddress);
    if (WI_IsFlagSet(m_flags, ConsommeNetworkingFlags::Ipv6))
    {
        UpdateIpv6Address(networkSettings->PreferredIpv6Address);
    }

    UpdateDefaultRoute(default_route);

    UpdateDnsSettings(currentDns);
    UpdateMtu(minMtu);

    m_networkSettings = std::move(networkSettings);
}

void ConsommeNetworking::SetupLoopbackDevice()
{
    auto loopbackSettings = std::make_shared<NetworkSettings>();
    const auto* clientIp = WI_IsFlagSet(m_flags, ConsommeNetworkingFlags::LoopbackClientIp) ? L"127.0.0.1" : L"169.254.73.250";
    loopbackSettings->PreferredIpAddress.Address = wsl::windows::common::string::StringToSockAddrInet(clientIp);
    loopbackSettings->PreferredIpAddress.AddressString = clientIp;
    loopbackSettings->PreferredIpAddress.PrefixLength = 28;
    loopbackSettings->Routes.emplace(EndpointRoute::DefaultRoute(AF_INET, wsl::windows::common::string::StringToSockAddrInet(L"169.254.73.249")));
    m_localhostAdapterId = m_guestDeviceManager->AddVirtioNetDevice(
        c_loopbackDeviceName, BuildVirtioNetConfig(loopbackSettings, false, c_gatewayMacAddress), {}, m_userToken.get());

    // The loopback gateway (see LX_INIT_IPV4_LOOPBACK_GATEWAY_ADDRESS) is 169.254.73.249, so use a /28 subnet
    // that includes both the client and gateway addresses.
    // N.B. The MAC address is advertised with the virtio device so doesn't need to be explicitly set.
    hns::HNSEndpoint endpointProperties;
    endpointProperties.ID = m_localhostAdapterId.value();
    endpointProperties.IPAddress = L"169.254.73.250";
    endpointProperties.PrefixLength = 28;
    endpointProperties.PortFriendlyName = c_loopbackDeviceName;
    m_gnsChannel.SendEndpointState(endpointProperties);

    hns::CreateDeviceRequest createLoopbackDevice;
    createLoopbackDevice.deviceName = c_loopbackDeviceName;
    createLoopbackDevice.type = hns::DeviceType::Loopback;
    createLoopbackDevice.lowerEdgeAdapterId = m_localhostAdapterId.value();

    // ipv6 duplicate address detection (DAD) breaks the ipv6 localhost relay since we can't predict the address before the guest tells us about it.
    createLoopbackDevice.flags = hns::CreateDeviceFlags::DisableDAD;
    constexpr auto loopbackType = GnsMessageType(createLoopbackDevice);
    m_gnsChannel.SendNetworkDeviceMessage(loopbackType, ToJsonW(createLoopbackDevice).c_str());
}

void ConsommeNetworking::SendDefaultRoute(const std::wstring& gateway, hns::ModifyRequestType requestType)
{
    if (gateway.empty() || !m_adapterId.has_value())
    {
        return;
    }

    wsl::shared::hns::Route route;
    route.NextHop = gateway;
    route.DestinationPrefix = LX_INIT_DEFAULT_ROUTE_PREFIX;
    route.Family = AF_INET;

    hns::ModifyGuestEndpointSettingRequest<hns::Route> request;
    request.RequestType = requestType;
    request.ResourceType = hns::GuestEndpointResourceType::Route;
    request.Settings = route;
    m_gnsChannel.SendHnsNotification(ToJsonW(request).c_str(), m_adapterId.value());
}

void ConsommeNetworking::UpdateDefaultRoute(const std::wstring& gateway)
{
    if (gateway == m_trackedDefaultRoute || !m_adapterId.has_value())
    {
        return;
    }

    SendDefaultRoute(m_trackedDefaultRoute, hns::ModifyRequestType::Remove);
    m_trackedDefaultRoute = gateway;
    SendDefaultRoute(gateway, hns::ModifyRequestType::Add);
}

void ConsommeNetworking::UpdateDnsSettings(const networking::DnsInfo& dns)
{
    if (dns == m_trackedDnsSettings || !m_adapterId.has_value())
    {
        return;
    }

    m_trackedDnsSettings = dns;

    hns::ModifyGuestEndpointSettingRequest<hns::DNS> notification{};
    notification.RequestType = hns::ModifyRequestType::Update;
    notification.ResourceType = hns::GuestEndpointResourceType::DNS;
    notification.Settings = networking::BuildDnsNotification(dns, m_dnsOptions);
    m_gnsChannel.SendHnsNotification(ToJsonW(notification).c_str(), m_adapterId.value());
}

void ConsommeNetworking::UpdateIpv4Address(const networking::EndpointIpAddress& ipAddress)
{
    if (ipAddress == m_trackedIpv4Address || ipAddress.AddressString.empty() || !m_adapterId.has_value())
    {
        return;
    }

    m_trackedIpv4Address = ipAddress;

    // N.B. SendEndpointState triggers SetAdapterConfiguration on the Linux side
    // which brings the interface UP and configures the full adapter state.
    hns::HNSEndpoint endpointProperties;
    endpointProperties.ID = m_adapterId.value();
    endpointProperties.IPAddress = ipAddress.AddressString;
    endpointProperties.PrefixLength = ipAddress.PrefixLength;
    m_gnsChannel.SendEndpointState(endpointProperties);
}

void ConsommeNetworking::SendIpv6Address(const networking::EndpointIpAddress& ipAddress, hns::ModifyRequestType requestType)
{
    WI_ASSERT(WI_IsFlagSet(m_flags, ConsommeNetworkingFlags::Ipv6));

    if (ipAddress.AddressString.empty() || !m_adapterId.has_value())
    {
        return;
    }

    // The HNSEndpoint schema doesn't support IPv6 addresses, so use ModifyGuestEndpointSettingRequest.
    hns::ModifyGuestEndpointSettingRequest<hns::IPAddress> request;
    request.RequestType = requestType;
    request.ResourceType = hns::GuestEndpointResourceType::IPAddress;
    request.Settings.Address = ipAddress.AddressString;
    request.Settings.Family = ipAddress.Address.si_family;
    request.Settings.OnLinkPrefixLength = ipAddress.PrefixLength;
    request.Settings.PreferredLifetime = ULONG_MAX;
    m_gnsChannel.SendHnsNotification(ToJsonW(request).c_str(), m_adapterId.value());
}

void ConsommeNetworking::UpdateIpv6Address(const networking::EndpointIpAddress& ipAddress)
{
    WI_ASSERT(WI_IsFlagSet(m_flags, ConsommeNetworkingFlags::Ipv6));

    if (ipAddress == m_trackedIpv6Address || !m_adapterId.has_value())
    {
        return;
    }

    SendIpv6Address(m_trackedIpv6Address, hns::ModifyRequestType::Remove);
    m_trackedIpv6Address = ipAddress;
    SendIpv6Address(ipAddress, hns::ModifyRequestType::Add);
}

void ConsommeNetworking::UpdateMtu(std::optional<ULONG> mtu)
{
    if (!mtu || mtu.value() == m_networkMtu || !m_adapterId.has_value())
    {
        return;
    }

    m_networkMtu = mtu.value();

    hns::ModifyGuestEndpointSettingRequest<hns::NetworkInterface> notification{};
    notification.ResourceType = hns::GuestEndpointResourceType::Interface;
    notification.RequestType = hns::ModifyRequestType::Update;
    notification.Settings.Connected = true;
    notification.Settings.NlMtu = m_networkMtu;
    m_gnsChannel.SendHnsNotification(ToJsonW(notification).c_str(), m_adapterId.value());
}
