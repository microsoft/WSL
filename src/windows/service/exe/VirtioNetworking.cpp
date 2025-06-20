// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "VirtioNetworking.h"
#include "Stringify.h"
#include "stringshared.h"

using namespace wsl::core::networking;
using namespace wsl::shared;
using namespace wsl::windows::common::stringify;
using wsl::core::VirtioNetworking;

static constexpr auto c_loopbackDeviceName = TEXT(LX_INIT_LOOPBACK_DEVICE_NAME);

VirtioNetworking::VirtioNetworking(GnsChannel&& gnsChannel, const Config& config) :
    m_gnsChannel(std::move(gnsChannel)), m_config(config)
{
}

VirtioNetworking& VirtioNetworking::OnAddGuestDevice(const AddGuestDeviceRoutine& addGuestDeviceRoutine)
{
    m_addGuestDeviceRoutine = addGuestDeviceRoutine;
    return *this;
}

VirtioNetworking& VirtioNetworking::OnModifyOpenPorts(const ModifyOpenPortsCallback& modifyOpenPortsCallback)
{
    m_modifyOpenPortsCallback = modifyOpenPortsCallback;
    return *this;
}

VirtioNetworking& VirtioNetworking::OnGuestInterfaceStateChanged(const GuestInterfaceStateChangeCallback& guestInterfaceStateChangedCallback)
{
    m_guestInterfaceStateChangeCallback = guestInterfaceStateChangedCallback;
    return *this;
}

void VirtioNetworking::Initialize()
try
{
    THROW_HR_IF(E_NOT_SET, !m_addGuestDeviceRoutine || !m_modifyOpenPortsCallback || !m_guestInterfaceStateChangeCallback);

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

    auto dns_servers = m_networkSettings->DnsServersString();
    if (!dns_servers.empty())
    {
        if (device_options.tellp() > 0)
        {
            device_options << L";";
        }
        device_options << L"nameservers=" << dns_servers;
    }

    // Add virtio net adapter to guest
    m_adapterId = (*m_addGuestDeviceRoutine)(c_virtioNetworkClsid, c_virtioNetworkDeviceId, L"eth0", device_options.str().c_str());

    auto lock = m_lock.lock_exclusive();

    hns::HNSEndpoint endpointProperties;
    endpointProperties.ID = m_adapterId;
    endpointProperties.IPAddress = m_networkSettings->PreferredIpAddress.AddressString;
    endpointProperties.PrefixLength = m_networkSettings->PreferredIpAddress.PrefixLength;
    m_gnsChannel.SendEndpointState(endpointProperties);

    // N.B. The MAC address is advertised with the virtio device so doesn't need to be explicitly set.

    // Send the default route to gns
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
        m_gnsChannel.SendHnsNotification(ToJsonW(request).c_str(), m_adapterId);
    }

    // Update DNS information.
    if (!dns_servers.empty())
    {
        // TODO: DNS domain suffixes
        hns::DNS dnsSettings{};
        dnsSettings.Options = LX_INIT_RESOLVCONF_FULL_HEADER;
        dnsSettings.ServerList = dns_servers;
        UpdateDns(std::move(dnsSettings));
    }

    if (m_config.EnableLocalhostRelay)
    {
        SetupLoopbackDevice();
    }

    THROW_IF_WIN32_ERROR(NotifyNetworkConnectivityHintChange(&VirtioNetworking::OnNetworkConnectivityChange, this, true, &m_networkNotifyHandle));
}
CATCH_LOG()

void VirtioNetworking::SetupLoopbackDevice()
{
    m_localhostAdapterId = (*m_addGuestDeviceRoutine)(
        c_virtioNetworkClsid, c_virtioNetworkDeviceId, c_loopbackDeviceName, L"client_ip=127.0.0.1;client_mac=00:11:22:33:44:55");

    hns::HNSEndpoint endpointProperties;
    endpointProperties.ID = m_localhostAdapterId;
    // The loopback gateway (see LX_INIT_IPV4_LOOPBACK_GATEWAY_ADDRESS) is 169.254.73.152, so assign loopback0 an
    // address of 169.254.73.153 with a netmask of 30 so that the only addresses associated with this adapter are
    // itself and the gateway.
    endpointProperties.IPAddress = L"169.254.73.153";
    endpointProperties.PrefixLength = 30;
    endpointProperties.PortFriendlyName = c_loopbackDeviceName;
    m_gnsChannel.SendEndpointState(endpointProperties);

    // N.B. The MAC address is advertised with the virtio device so doesn't need to be explicitly set.

    hns::CreateDeviceRequest createLoopbackDevice;
    createLoopbackDevice.deviceName = c_loopbackDeviceName;
    createLoopbackDevice.type = hns::DeviceType::Loopback;
    createLoopbackDevice.lowerEdgeAdapterId = m_localhostAdapterId;
    constexpr auto loopbackType = GnsMessageType(createLoopbackDevice);
    m_gnsChannel.SendNetworkDeviceMessage(loopbackType, ToJsonW(createLoopbackDevice).c_str());
}

void VirtioNetworking::StartPortTracker(wil::unique_socket&& socket)
{
    WI_ASSERT(!m_gnsPortTrackerChannel.has_value());

    m_gnsPortTrackerChannel.emplace(
        std::move(socket),
        [&](const SOCKADDR_INET& addr, int protocol, bool allocate) { return HandlePortNotification(addr, protocol, allocate); },
        [&](_In_ const std::string& interfaceName, _In_ bool up) { (*m_guestInterfaceStateChangeCallback)(interfaceName, up); });
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

    if (m_config.EnableLocalhostRelay && (unspecified || loopback))
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
        result = (*m_modifyOpenPortsCallback)(c_virtioNetworkClsid, c_loopbackDeviceName, localAddr, protocol, allocate);
        LOG_HR_IF_MSG(E_FAIL, result != S_OK, "Failure adding localhost relay port %d", localAddr.Ipv4.sin_port);
    }
    if (!loopback)
    {
        const int localResult = (*m_modifyOpenPortsCallback)(c_virtioNetworkClsid, L"eth0", addr, protocol, allocate);
        LOG_HR_IF_MSG(E_FAIL, localResult != S_OK, "Failure adding relay port %d", addr.Ipv4.sin_port);
        if (result == 0)
        {
            result = localResult;
        }
    }
    return result;
}

void NETIOAPI_API_ VirtioNetworking::OnNetworkConnectivityChange(PVOID context, NL_NETWORK_CONNECTIVITY_HINT hint)
{
    static_cast<VirtioNetworking*>(context)->RefreshGuestConnection(hint);
}

void VirtioNetworking::RefreshGuestConnection(NL_NETWORK_CONNECTIVITY_HINT connectivityHint) noexcept
try
{
    auto lock = m_lock.lock_exclusive();
    UpdateMtu();
}
CATCH_LOG();

void VirtioNetworking::UpdateDns(hns::DNS&& dnsSettings)
{
    hns::ModifyGuestEndpointSettingRequest<hns::DNS> notification{};
    notification.RequestType = hns::ModifyRequestType::Update;
    notification.ResourceType = hns::GuestEndpointResourceType::DNS;
    notification.Settings = std::move(dnsSettings);
    m_gnsChannel.SendHnsNotification(ToJsonW(notification).c_str(), m_adapterId);
}

void VirtioNetworking::UpdateMtu()
{
    unique_interface_table interfaceTable{};
    THROW_IF_WIN32_ERROR(::GetIpInterfaceTable(AF_UNSPEC, &interfaceTable));

    ULONG minMtu = ULONG_MAX;
    for (ULONG index = 0; index < interfaceTable.get()->NumEntries; index++)
    {
        const auto& ipInterface = interfaceTable.get()->Table[index];
        if (ipInterface.Connected)
        {
            minMtu = std::min(ipInterface.NlMtu, minMtu);
        }
    }

    // Only send the update if the MTU changed.
    if (minMtu != ULONG_MAX && minMtu != m_networkMtu)
    {
        hns::ModifyGuestEndpointSettingRequest<hns::NetworkInterface> notification{};
        notification.ResourceType = hns::GuestEndpointResourceType::Interface;
        notification.RequestType = hns::ModifyRequestType::Update;
        notification.Settings.NlMtu = m_networkMtu;
        notification.Settings.Connected = true;

        WSL_LOG("VirtioNetworking::UpdateMtu", TraceLoggingValue(m_networkMtu, "VirtioMtu"));

        // TODO: Why was this commented ?
        // m_gnsChannel.SendHnsNotification(ToJsonW(notification).c_str(), m_endpointId);
    }
}

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

std::optional<ULONGLONG> VirtioNetworking::FindVirtioInterfaceLuid(const SOCKADDR_INET& VirtioAddress, const NL_NETWORK_CONNECTIVITY_HINT& currentConnectivityHint)
{
    constexpr ULONGLONG maxTimeToWaitMs = 10 * 1000;
    constexpr ULONG timeToSleepMs = 100;
    const auto startTickCount = GetTickCount64();

    NET_LUID VirtioLuid{};
    for (;;)
    {
        unique_address_table addressTable;
        THROW_IF_WIN32_ERROR(GetUnicastIpAddressTable(AF_INET, &addressTable));
        for (const auto& address : wil::make_range(addressTable.get()->Table, addressTable.get()->NumEntries))
        {
            if (VirtioAddress == address.Address)
            {
                VirtioLuid.Value = address.InterfaceLuid.Value;
                break;
            }

            WSL_LOG(
                "VirtioNetworking::FindVirtioInterfaceLuid [IP Address comparison mismatch]",
                TraceLoggingValue(wsl::windows::common::string::SockAddrInetToString(VirtioAddress).c_str(), "VirtioAddress"),
                TraceLoggingValue(
                    wsl::windows::common::string::SockAddrInetToString(address.Address).c_str(), "enumeratedAddress"));
        }

        if (VirtioLuid.Value != 0)
        {
            break;
        }

        // give up if something is just broken and taking too long
        if (GetTickCount64() - startTickCount >= maxTimeToWaitMs)
        {
            break;
        }
        // else sleep and try again shortly
        Sleep(timeToSleepMs);
        // bail if connectivity on the host has completely changed
        NL_NETWORK_CONNECTIVITY_HINT latestConnectivityHint{};
        GetNetworkConnectivityHint(&latestConnectivityHint);
        if (latestConnectivityHint != currentConnectivityHint)
        {
            WSL_LOG("VirtioNetworking::FindVirtioInterfaceLuid [connectivity changed while waiting for the Virtio interface]");
            THROW_WIN32_MSG(ERROR_RETRY, "connectivity changed while waiting for the Virtio interface");
        }
    }

    if (VirtioLuid.Value == 0)
    {
        WSL_LOG(
            "VirtioNetworking::FindVirtioInterfaceLuid [IP address not found]",
            TraceLoggingValue(VirtioLuid.Value, "VirtioInterfaceLuid"),
            TraceLoggingValue(wsl::windows::common::string::SockAddrInetToString(VirtioAddress).c_str(), "VirtioIPAddress"));
        return {};
    }

    WSL_LOG(
        "VirtioNetworking::FindVirtioInterfaceLuid [waiting for Virtio interface to be connected]",
        TraceLoggingValue(VirtioLuid.Value, "VirtioInterfaceLuid"),
        TraceLoggingValue(wsl::windows::common::string::SockAddrInetToString(VirtioAddress).c_str(), "VirtioIPAddress"));

    bool ipv4Connected = false;
    for (;;)
    {
        unique_interface_table interfaceTable{};
        THROW_IF_WIN32_ERROR(::GetIpInterfaceTable(AF_UNSPEC, &interfaceTable));
        // we only track the IPv4 interface because we only Virtio IPv4 to the container
        for (auto index = 0ul; index < interfaceTable.get()->NumEntries; ++index)
        {
            const auto& ipInterface = interfaceTable.get()->Table[index];
            if (ipInterface.Family == AF_INET && !!ipInterface.Connected && ipInterface.InterfaceLuid.Value == VirtioLuid.Value)
            {
                ipv4Connected = true;
                break;
            }
        }
        if (ipv4Connected)
        {
            break;
        }

        // give up if something is just broken and taking too long
        if (GetTickCount64() - startTickCount >= maxTimeToWaitMs)
        {
            break;
        }
        // else sleep and try again shortly
        Sleep(timeToSleepMs);
        // bail if connectivity on the host has completely changed
        NL_NETWORK_CONNECTIVITY_HINT latestConnectivityHint{};
        GetNetworkConnectivityHint(&latestConnectivityHint);
        if (latestConnectivityHint != currentConnectivityHint)
        {
            WSL_LOG("VirtioNetworking::FindVirtioInterfaceLuid [connectivity changed while waiting for the Virtio interface]");
            THROW_WIN32_MSG(ERROR_RETRY, "connectivity changed while waiting for the Virtio interface");
        }
    }

    // return zero if it's not connected yet so we can retry the next cycle
    return ipv4Connected ? VirtioLuid.Value : std::optional<ULONGLONG>();
}
