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

void VirtioNetworking::StartPortTracker(wil::unique_socket&& socket)
{
    WI_ASSERT(!m_gnsPortTrackerChannel.has_value());

    m_gnsPortTrackerChannel.emplace(
        std::move(socket),
        [&](const SOCKADDR_INET& addr, int protocol, bool allocate) { return HandlePortNotification(addr, protocol, allocate); },
        [](const std::string&, bool) {}); // TODO: reconsider if InterfaceStateCallback is needed.
}

HRESULT VirtioNetworking::HandlePortNotification(const SOCKADDR_INET& addr, int protocol, bool allocate) const noexcept
{
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
    else if (addr.si_family == AF_INET6)
    {
        // The virtio net adapter does not yet support IPv6 packets, so any traffic would arrive via
        // IPv4. If the caller wants IPv4 they will also likely listen on an IPv4 address, which will
        // be handled as a separate callback to this same code.
        return 0;
    }

    auto lock = m_lock.lock_exclusive();
    const auto server = m_guestDeviceManager->GetRemoteFileSystem(VIRTIO_NET_CLASS_ID, c_defaultDeviceTag);
    if (server)
    {
        std::wstring portString = std::format(L"tag={};port_number={}", tag, addr.Ipv4.sin_port);
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

void NETIOAPI_API_ VirtioNetworking::OnNetworkConnectivityChange(PVOID context, NL_NETWORK_CONNECTIVITY_HINT hint)
{
    static_cast<VirtioNetworking*>(context)->RefreshGuestConnection();
}

void VirtioNetworking::RefreshGuestConnection() noexcept
try
{
    // Query current networking information before acquiring the lock.
    m_networkSettings = GetHostEndpointSettings();

    // TODO: Determine gateway MAC address
    std::wstringstream device_options;
    auto client_ip = m_networkSettings->PreferredIpAddress.AddressString;
    if (!client_ip.empty())
    {
        if (device_options.tellp() > 0)
        {
            device_options << L";";
        }
        device_options << L"client_ip=" << client_ip;
    }

    if (!m_networkSettings->MacAddress.empty())
    {
        if (device_options.tellp() > 0)
        {
            device_options << L";";
        }
        device_options << L"client_mac=" << m_networkSettings->MacAddress;
    }

    std::wstring default_route = m_networkSettings->GetBestGatewayAddressString();
    if (!default_route.empty())
    {
        if (device_options.tellp() > 0)
        {
            device_options << L";";
        }
        device_options << L"gateway_ip=" << default_route;
    }

    const auto newDeviceOptions = device_options.str();

    networking::DnsInfo currentDns{};
    if (WI_IsFlagSet(m_flags, VirtioNetworkingFlags::DnsTunneling))
    {
        currentDns = networking::HostDnsInfo::GetDnsTunnelingSettings(default_route);
    }
    else
    {
        currentDns = networking::HostDnsInfo::GetDnsSettings(networking::DnsSettingsFlags::IncludeVpn);
    }

    const auto minMtu = GetMinimumConnectedInterfaceMtu();

    // Acquire the lock and perform device updates.
    auto lock = m_lock.lock_exclusive();
    if (newDeviceOptions != m_trackedDeviceOptions)
    {
        m_trackedDeviceOptions = newDeviceOptions;

        // Add virtio net adapter to guest. If the adapter already exists update adapter state.
        if (!m_adapterId.has_value())
        {
            m_adapterId = m_guestDeviceManager->AddGuestDevice(
                VIRTIO_NET_DEVICE_ID, VIRTIO_NET_CLASS_ID, c_eth0DeviceName, nullptr, newDeviceOptions.c_str(), 0, m_userToken.get());
        }
        else
        {
            const auto server = m_guestDeviceManager->GetRemoteFileSystem(VIRTIO_NET_CLASS_ID, c_defaultDeviceTag);
            if (server)
            {
                LOG_IF_FAILED(server->AddSharePath(c_eth0DeviceName, newDeviceOptions.c_str(), 0));
            }
        }

        // N.B. The MAC address is advertised with the virtio device so doesn't need to be explicitly set.
        hns::HNSEndpoint endpointProperties;
        endpointProperties.ID = m_adapterId.value();
        endpointProperties.IPAddress = m_networkSettings->PreferredIpAddress.AddressString;
        endpointProperties.PrefixLength = m_networkSettings->PreferredIpAddress.PrefixLength;
        m_gnsChannel.SendEndpointState(endpointProperties);

        // Send the default route to GNS.
        if (!default_route.empty())
        {
            wsl::shared::hns::Route route;
            route.NextHop = default_route;
            route.DestinationPrefix = LX_INIT_DEFAULT_ROUTE_PREFIX;
            route.Family = AF_INET;

            hns::ModifyGuestEndpointSettingRequest<hns::Route> request;
            request.RequestType = hns::ModifyRequestType::Add;
            request.ResourceType = hns::GuestEndpointResourceType::Route;
            request.Settings = route;
            m_gnsChannel.SendHnsNotification(ToJsonW(request).c_str(), m_adapterId.value());
        }
    }

    // Send DNS update if needed.
    if (currentDns != m_trackedDnsSettings)
    {
        m_trackedDnsSettings = currentDns;
        hns::ModifyGuestEndpointSettingRequest<hns::DNS> notification{};
        notification.RequestType = hns::ModifyRequestType::Update;
        notification.ResourceType = hns::GuestEndpointResourceType::DNS;
        notification.Settings = networking::BuildDnsNotification(currentDns, m_dnsOptions);
        m_gnsChannel.SendHnsNotification(ToJsonW(notification).c_str(), m_adapterId.value());
    }

    // Send MTU update if needed.
    if (minMtu && minMtu.value() != m_networkMtu)
    {
        m_networkMtu = minMtu.value();
        hns::ModifyGuestEndpointSettingRequest<hns::NetworkInterface> notification{};
        notification.ResourceType = hns::GuestEndpointResourceType::Interface;
        notification.RequestType = hns::ModifyRequestType::Update;
        notification.Settings.Connected = true;
        notification.Settings.NlMtu = m_networkMtu;
        m_gnsChannel.SendHnsNotification(ToJsonW(notification).c_str(), m_adapterId.value());
    }
}
CATCH_LOG();

void VirtioNetworking::TraceLoggingRundown() noexcept
{
    auto lock = m_lock.lock_exclusive();

    WSL_LOG("VirtioNetworking::TraceLoggingRundown", TRACE_NETWORKSETTINGS_OBJECT(m_networkSettings));
}

void VirtioNetworking::FillInitialConfiguration(LX_MINI_INIT_NETWORKING_CONFIGURATION& message)
{
    message.NetworkingMode = LxMiniInitNetworkingModeVirtioProxy;
    message.DisableIpv6 = false;
    message.EnableDhcpClient = false;
    message.PortTrackerType = LX_MINI_INIT_PORT_TRACKER_TYPE::LxMiniInitPortTrackerTypeMirrored;
}
