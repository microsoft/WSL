// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "VirtioNetworking.h"
#include "GuestDeviceManager.h"
#include "Stringify.h"
#include "stringshared.h"

using namespace wsl::core::networking;
using namespace wsl::shared;
using namespace wsl::windows::common::stringify;
using wsl::core::VirtioNetworking;

static constexpr auto c_eth0DeviceName = L"eth0";
static constexpr auto c_loopbackDeviceName = TEXT(LX_INIT_LOOPBACK_DEVICE_NAME);

VirtioNetworking::VirtioNetworking(
    GnsChannel&& gnsChannel, VirtioNetworkingFlags flags, LPCWSTR dnsOptions, std::shared_ptr<GuestDeviceManager> guestDeviceManager, wil::shared_handle userToken) :
    m_guestDeviceManager(std::move(guestDeviceManager)),
    m_userToken(std::move(userToken)),
    m_gnsChannel(std::move(gnsChannel)),
    m_flags(flags),
    m_dnsOptions(dnsOptions)
{
}

VirtioNetworking::~VirtioNetworking()
{
    // Unregister the network notification callback to prevent it from using the GNS channel.
    m_networkNotifyHandle.reset();

    // Stop the GNS channel to unblock any stuck communications with the guest.
    m_gnsChannel.Stop();
}

void VirtioNetworking::Initialize()
{
    // Initialize adapter state.
    RefreshGuestConnection();

    if (WI_IsFlagSet(m_flags, VirtioNetworkingFlags::LocalhostRelay))
    {
        SetupLoopbackDevice();
    }

    THROW_IF_WIN32_ERROR(NotifyNetworkConnectivityHintChange(&VirtioNetworking::OnNetworkConnectivityChange, this, TRUE, &m_networkNotifyHandle));
}

void VirtioNetworking::TraceLoggingRundown() noexcept
{
    auto lock = m_lock.lock_exclusive();

    WSL_LOG("VirtioNetworking::TraceLoggingRundown", TRACE_NETWORKSETTINGS_OBJECT(m_networkSettings));
}

void VirtioNetworking::FillInitialConfiguration(LX_MINI_INIT_NETWORKING_CONFIGURATION& message)
{
    message.NetworkingMode = LxMiniInitNetworkingModeVirtioProxy;
    message.DisableIpv6 = WI_IsFlagClear(m_flags, VirtioNetworkingFlags::Ipv6);
    message.EnableDhcpClient = false;
    message.PortTrackerType = LX_MINI_INIT_PORT_TRACKER_TYPE::LxMiniInitPortTrackerTypeMirrored;
}

void VirtioNetworking::StartPortTracker(wil::unique_socket&& socket)
{
    WI_ASSERT(!m_gnsPortTrackerChannel.has_value());

    m_gnsPortTrackerChannel.emplace(
        std::move(socket),
        [&](const SOCKADDR_INET& addr, int protocol, bool allocate) { return HandlePortNotification(addr, protocol, allocate); },
        [](const std::string&, bool) {}); // TODO: reconsider if InterfaceStateCallback is needed.
}

void NETIOAPI_API_ VirtioNetworking::OnNetworkConnectivityChange(PVOID context, NL_NETWORK_CONNECTIVITY_HINT hint)
try
{
    static_cast<VirtioNetworking*>(context)->RefreshGuestConnection();
}
CATCH_LOG()

HRESULT VirtioNetworking::HandlePortNotification(const SOCKADDR_INET& addr, int protocol, bool allocate) const noexcept
{
    if (addr.si_family == AF_INET6 && WI_IsFlagClear(m_flags, VirtioNetworkingFlags::Ipv6))
    {
        return S_OK;
    }

    int result = 0;
    const auto ipAddress = (addr.si_family == AF_INET) ? reinterpret_cast<const void*>(&addr.Ipv4.sin_addr)
                                                       : reinterpret_cast<const void*>(&addr.Ipv6.sin6_addr);
    const bool loopback = INET_IS_ADDR_LOOPBACK(addr.si_family, ipAddress);
    const bool unspecified = INET_IS_ADDR_UNSPECIFIED(addr.si_family, ipAddress);
    if (addr.si_family == AF_INET && loopback)
    {
        // Only intercepting 127.0.0.1; any other loopback address will remain on 'lo'.
        if (addr.Ipv4.sin_addr.s_addr != htonl(INADDR_LOOPBACK))
        {
            return result;
        }
    }

    if (WI_IsFlagSet(m_flags, VirtioNetworkingFlags::LocalhostRelay) && (unspecified || loopback))
    {
        SOCKADDR_INET localAddr = addr;
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
        result = ModifyOpenPorts(c_loopbackDeviceName, localAddr, protocol, allocate);
        LOG_HR_IF_MSG(
            E_FAIL, result != S_OK, "Failure adding localhost relay port %d", INETADDR_PORT(reinterpret_cast<const SOCKADDR*>(&localAddr)));
    }

    if (!loopback)
    {
        const int localResult = ModifyOpenPorts(c_eth0DeviceName, addr, protocol, allocate);
        LOG_HR_IF_MSG(E_FAIL, localResult != S_OK, "Failure adding relay port %d", INETADDR_PORT(reinterpret_cast<const SOCKADDR*>(&addr)));
        if (result == 0)
        {
            result = localResult;
        }
    }

    return result;
}

int VirtioNetworking::ModifyOpenPorts(_In_ PCWSTR tag, _In_ const SOCKADDR_INET& addr, _In_ int protocol, _In_ bool isOpen) const
{
    if (protocol != IPPROTO_TCP && protocol != IPPROTO_UDP)
    {
        LOG_HR_MSG(HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED), "Unsupported bind protocol %d", protocol);
        return 0;
    }

    auto lock = m_lock.lock_exclusive();
    const auto server = m_guestDeviceManager->GetRemoteFileSystem(VIRTIO_NET_CLASS_ID, c_defaultDeviceTag);
    if (server)
    {
        std::wstring portString = std::format(L"tag={};port_number={}", tag, INETADDR_PORT(reinterpret_cast<const SOCKADDR*>(&addr)));
        if (protocol == IPPROTO_UDP)
        {
            portString += L";udp";
        }

        if (!isOpen)
        {
            portString += L";allocate=false";
        }
        else
        {
            const auto addrStr = wsl::windows::common::string::SockAddrInetToWstring(addr);
            portString += std::format(L";listen_addr={}", addrStr);
        }

        LOG_IF_FAILED(server->AddShare(portString.c_str(), nullptr, 0));
    }

    return 0;
}

void VirtioNetworking::RefreshGuestConnection()
{
    // Query current networking information before acquiring the lock.
    auto networkSettings = GetHostEndpointSettings();

    std::wstring device_options;
    auto appendOption = [&device_options](std::wstring_view key, std::wstring_view value) {
        if (!value.empty())
        {
            std::format_to(std::back_inserter(device_options), L"{}{}={}", device_options.empty() ? L"" : L";", key, value);
        }
    };

    appendOption(L"client_ip", networkSettings->PreferredIpAddress.AddressString);
    std::wstring default_route = networkSettings->GetBestGatewayAddressString();
    appendOption(L"gateway_ip", default_route);
    if (WI_IsFlagSet(m_flags, VirtioNetworkingFlags::Ipv6))
    {
        appendOption(L"client_ip_ipv6", networkSettings->PreferredIpv6Address.AddressString);
    }

    networking::DnsInfo currentDns{};
    if (WI_IsFlagSet(m_flags, VirtioNetworkingFlags::DnsTunneling))
    {
        currentDns = networking::HostDnsInfo::GetDnsTunnelingSettings(default_route);
    }
    else
    {
        wsl::core::networking::DnsSettingsFlags dnsFlags = networking::DnsSettingsFlags::IncludeVpn;
        WI_SetFlagIf(dnsFlags, networking::DnsSettingsFlags::IncludeIpv6Servers, WI_IsFlagSet(m_flags, VirtioNetworkingFlags::Ipv6));
        currentDns = networking::HostDnsInfo::GetDnsSettings(dnsFlags);
    }

    const auto minMtu = GetMinimumConnectedInterfaceMtu();

    // Acquire the lock and perform device updates.
    auto lock = m_lock.lock_exclusive();

    // Add virtio net adapter to guest. If the adapter already exists update adapter state.
    if (device_options != m_trackedDeviceOptions)
    {
        if (!m_adapterId.has_value())
        {
            m_adapterId = m_guestDeviceManager->AddGuestDevice(
                VIRTIO_NET_DEVICE_ID, VIRTIO_NET_CLASS_ID, c_eth0DeviceName, nullptr, device_options.c_str(), 0, m_userToken.get());
        }
        else
        {
            const auto server = m_guestDeviceManager->GetRemoteFileSystem(VIRTIO_NET_CLASS_ID, c_defaultDeviceTag);
            if (server)
            {
                LOG_IF_FAILED(server->AddSharePath(c_eth0DeviceName, device_options.c_str(), 0));
            }
        }

        m_trackedDeviceOptions = device_options;
    }

    UpdateIpv4Address(networkSettings->PreferredIpAddress);
    if (WI_IsFlagSet(m_flags, VirtioNetworkingFlags::Ipv6))
    {
        UpdateIpv6Address(networkSettings->PreferredIpv6Address);
    }

    UpdateDefaultRoute(default_route);

    UpdateDnsSettings(currentDns);
    UpdateMtu(minMtu);

    m_networkSettings = std::move(networkSettings);
}

void VirtioNetworking::SetupLoopbackDevice()
{
    m_localhostAdapterId = m_guestDeviceManager->AddGuestDevice(
        VIRTIO_NET_DEVICE_ID,
        VIRTIO_NET_CLASS_ID,
        c_loopbackDeviceName,
        nullptr,
        L"client_ip=127.0.0.1;client_mac=00:11:22:33:44:55",
        0,
        m_userToken.get());

    // The loopback gateway (see LX_INIT_IPV4_LOOPBACK_GATEWAY_ADDRESS) is 169.254.73.152, so assign loopback0 an
    // address of 169.254.73.153 with a netmask of 30 so that the only addresses associated with this adapter are
    // itself and the gateway.
    // N.B. The MAC address is advertised with the virtio device so doesn't need to be explicitly set.
    hns::HNSEndpoint endpointProperties;
    endpointProperties.ID = m_localhostAdapterId.value();
    endpointProperties.IPAddress = L"169.254.73.153";
    endpointProperties.PrefixLength = 30;
    endpointProperties.PortFriendlyName = c_loopbackDeviceName;
    m_gnsChannel.SendEndpointState(endpointProperties);

    hns::CreateDeviceRequest createLoopbackDevice;
    createLoopbackDevice.deviceName = c_loopbackDeviceName;
    createLoopbackDevice.type = hns::DeviceType::Loopback;
    createLoopbackDevice.lowerEdgeAdapterId = m_localhostAdapterId.value();
    constexpr auto loopbackType = GnsMessageType(createLoopbackDevice);
    m_gnsChannel.SendNetworkDeviceMessage(loopbackType, ToJsonW(createLoopbackDevice).c_str());
}

void VirtioNetworking::SendDefaultRoute(const std::wstring& gateway, hns::ModifyRequestType requestType)
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

void VirtioNetworking::UpdateDefaultRoute(const std::wstring& gateway)
{
    if (gateway == m_trackedDefaultRoute || !m_adapterId.has_value())
    {
        return;
    }

    SendDefaultRoute(m_trackedDefaultRoute, hns::ModifyRequestType::Remove);
    m_trackedDefaultRoute = gateway;
    SendDefaultRoute(gateway, hns::ModifyRequestType::Add);
}

void VirtioNetworking::UpdateDnsSettings(const networking::DnsInfo& dns)
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

void VirtioNetworking::UpdateIpv4Address(const networking::EndpointIpAddress& ipAddress)
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

void VirtioNetworking::SendIpv6Address(const networking::EndpointIpAddress& ipAddress, hns::ModifyRequestType requestType)
{
    WI_ASSERT(WI_IsFlagSet(m_flags, VirtioNetworkingFlags::Ipv6));

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

void VirtioNetworking::UpdateIpv6Address(const networking::EndpointIpAddress& ipAddress)
{
    WI_ASSERT(WI_IsFlagSet(m_flags, VirtioNetworkingFlags::Ipv6));

    if (ipAddress == m_trackedIpv6Address || !m_adapterId.has_value())
    {
        return;
    }

    SendIpv6Address(m_trackedIpv6Address, hns::ModifyRequestType::Remove);
    m_trackedIpv6Address = ipAddress;
    SendIpv6Address(ipAddress, hns::ModifyRequestType::Add);
}

void VirtioNetworking::UpdateMtu(std::optional<ULONG> mtu)
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
