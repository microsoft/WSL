// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "NatNetworking.h"
#include "WslCoreNetworkEndpointSettings.h"
#include "WslCoreHostDnsInfo.h"
#include "Stringify.h"
#include "WslCoreFirewallSupport.h"
#include "hcs.hpp"

using namespace wsl::core::networking;
using namespace wsl::windows::common::stringify;
using namespace wsl::windows::common::string;
using namespace wsl::windows::common::hcs;
using namespace wsl::shared;
using wsl::core::NatNetworking;
using wsl::windows::common::Context;
using wsl::windows::common::ExecutionContext;
using wsl::windows::common::hcs::unique_hcn_endpoint;

// This static list is used to keep of which endpoints are in use by other users.
// It's needed because when we see an endpoint with the same ip address we want,
// we have no way to differentiate between an endpoint that we previously used
// that didn't get deleted, and an endpoint actively in use by another user.
static wil::srwlock g_endpointsInUseLock;
static std::vector<GUID> g_endpointsInUse;

NatNetworking::NatNetworking(
    HCS_SYSTEM system, wsl::windows::common::hcs::unique_hcn_network&& network, GnsChannel&& gnsChannel, Config& config, wil::unique_socket&& dnsHvsocket) :
    m_system(system), m_config(config), m_network(std::move(network)), m_gnsChannel(std::move(gnsChannel))
{
    m_connectivityTelemetryEnabled = config.EnableTelemetry && !WslTraceLoggingShouldDisableTelemetry();

    if (dnsHvsocket)
    {
        // Create the DNS resolver used for DNS tunneling.
        networking::DnsResolverFlags resolverFlags{};
        WI_SetFlagIf(resolverFlags, networking::DnsResolverFlags::BestEffortDnsParsing, m_config.BestEffortDnsParsing);

        m_dnsTunnelingResolver.emplace(std::move(dnsHvsocket), resolverFlags);

        m_dnsTunnelingIpAddress = wsl::windows::common::string::IntegerIpv4ToWstring(config.DnsTunnelingIpAddress.value());
    }
    else if (!config.EnableDnsProxy)
    {
        // EnableDnsProxy indicates to use the DNS/NAT shared access service to proxy DNS requests
        // If this is false then wsl will assign a prioritized set of DNS servers into the Linux container
        // prioritized means:
        // - can only set 3 DNS servers (Linux limitation)
        // - when there are multiple host connected interfaces, we need to use the DNS servers from the most-likely-to-be-used interface on the host
        m_mirrorDnsInfo.emplace();
    }
}

NatNetworking::~NatNetworking()
{
    // Stop DNS suffix change notifications first, as those can call into the GNS channel.
    m_dnsSuffixRegistryWatcher.reset();

    // Stop the GNS channel to unblock any stuck communications with the guest
    // calling this before m_connectivityTelemetry.Reset() to unblock that callback if it's attempting a connectivity request in Linux
    m_gnsChannel.Stop();

    // Stop the telemetry timer which could queue work to linux (through m_gnsChannel)
    m_connectivityTelemetry.Reset();

    // Unregister the network notification callback to prevent notifications from running while the remainder of the object is destroyed.
    m_networkNotifyHandle.reset();

    auto lock = g_endpointsInUseLock.lock_exclusive();
    auto eraseRange = std::ranges::remove(g_endpointsInUse, m_endpoint.Id);
    g_endpointsInUse.erase(eraseRange.begin(), eraseRange.end());
}

void NatNetworking::TelemetryConnectionCallback(NLM_CONNECTIVITY hostConnectivity, uint32_t telemetryCounter) noexcept
try
{
    WSL_LOG("NatNetworking::TelemetryConnectionCallback");

    // if this is the inital callback for checking container connectivity, push this through as telemetry, so we can observe the time-to-connect
    if ((telemetryCounter == 1) || (hostConnectivity & NLM_CONNECTIVITY_IPV4_INTERNET))
    {
        int returnedIPv4Value{};
        const auto requestStatus = wil::ResultFromException([&] {
            const auto lock = m_lock.lock_exclusive();
            returnedIPv4Value = m_gnsChannel.SendNetworkDeviceMessageReturnResult(LxGnsMessageConnectTestRequest, c_ipv4TestRequestTarget);
        });

        // make the same connect requests as we just requested from the container
        const auto hostConnCheckResult = wsl::shared::conncheck::CheckConnection(c_ipv4TestRequestTargetA, nullptr, "80");
        const auto WindowsIpv4ConnCheckStatus = static_cast<uint32_t>(hostConnCheckResult.Ipv4Status);
        const auto WindowsIpv6ConnCheckStatus = static_cast<uint32_t>(hostConnCheckResult.Ipv6Status);

        const auto WindowsIPv4NlmConnectivityLevel = ConnectivityTelemetry::WindowsIPv4NlmConnectivityLevel(hostConnectivity);
        const auto WindowsIPv6NlmConnectivityLevel = ConnectivityTelemetry::WindowsIPv6NlmConnectivityLevel(hostConnectivity);
        const auto LinuxIPv4ConnCheckStatus = ConnectivityTelemetry::LinuxIPv4ConnCheckResult(returnedIPv4Value);
        // NAT doesn't have an IPv6 result because NAT is only IPv4 -- 2 == failed to connect
        constexpr auto LinuxIPv6ConnCheckStatus = 2;

        const auto timeFromObjectCreation = std::chrono::steady_clock::now() - m_objectCreationTime;
        WSL_LOG_TELEMETRY(
            "TelemetryConnectionCallback",
            PDT_ProductAndServicePerformance,
            TraceLoggingValue("NAT", "networkingMode"),
            TraceLoggingValue(telemetryCounter, "telemetryCounter"),
            TraceLoggingValue(
                (std::chrono::duration_cast<std::chrono::milliseconds>(timeFromObjectCreation)).count(),
                "timeFromObjectCreationMs"),
            TraceLoggingValue(wsl::core::networking::ToString(hostConnectivity).c_str(), "HostConnectivityLevel"),
            TraceLoggingValue(WindowsIPv4NlmConnectivityLevel, "WindowsIPv4ConnectivityLevel"),
            TraceLoggingValue(WindowsIPv6NlmConnectivityLevel, "WindowsIPv6ConnectivityLevel"),
            TraceLoggingValue(LinuxIPv4ConnCheckStatus, "LinuxIPv4ConnCheckStatus"),
            TraceLoggingValue(LinuxIPv6ConnCheckStatus, "LinuxIPv6ConnCheckStatus"),
            TraceLoggingValue(WindowsIpv4ConnCheckStatus, "WindowsIpv4ConnCheckStatus"),
            TraceLoggingValue(WindowsIpv6ConnCheckStatus, "WindowsIpv6ConnCheckStatus"),
            TraceLoggingHResult(requestStatus, "statusSendingMessageToLinux"),
            TraceLoggingValue(m_config.EnableDnsTunneling, "DnsTunnelingEnabled"),
            TraceLoggingValue(m_dnsTunnelingIpAddress.c_str(), "DnsTunnelingIpAddress"),
            TraceLoggingValue(m_config.FirewallConfig.Enabled(), "HyperVFirewallEnabled"),
            TraceLoggingValue(m_config.EnableAutoProxy, "AutoProxyFeatureEnabled")); // the feature is enabled, but we don't know if proxy settings are actually configured
    }
    else
    {
        WSL_LOG(
            "NatNetworking::TelemetryConnectionCallback - not testing connectivity - host is not connected",
            TraceLoggingValue(wsl::core::networking::ToString(hostConnectivity).c_str(), "HostConnectivityLevel"));
    }
}
CATCH_LOG()

bool NatNetworking::IsHyperVFirewallSupported(const wsl::core::Config& vmConfig) noexcept
{
    const auto hyperVFirewallSupport = wsl::core::networking::GetHyperVFirewallSupportVersion(vmConfig.FirewallConfig);

    switch (hyperVFirewallSupport)
    {
    case HyperVFirewallSupport::None:
        WSL_LOG("IsHyperVFirewallSupported returning false: No Hyper-V Firewall API present");
        return false;

    case HyperVFirewallSupport::Version1:
        // we don't support using a NAT *and* Hyper-V Firewall when Windows only has the V1 APIs
        WSL_LOG(
            "IsHyperVFirewallSupported returning false: Hyper-V Firewall not supported with a NAT-network and v1 Hyper-V "
            "Firewall APIs");
        return false;

    case HyperVFirewallSupport::Version2:
    {
        return true;
    }

    default:
        WI_ASSERT(false);
        return false;
    }
}

std::pair<wsl::core::networking::EphemeralHcnEndpoint, wsl::shared::hns::HNSEndpoint> NatNetworking::CreateEndpoint(const std::wstring& IpAddress) const
{
    hns::HostComputeEndpoint hnsEndpoint{};
    hnsEndpoint.SchemaVersion.Major = 2;
    hnsEndpoint.SchemaVersion.Minor = 16;

    // Network Id
    hnsEndpoint.HostComputeNetwork = m_config.NatNetworkId();

    // Port name policy
    hns::EndpointPolicy<hns::PortnameEndpointPolicySetting> endpointPortNamePolicy{};
    endpointPortNamePolicy.Type = hns::EndpointPolicyType::PortName;
    hnsEndpoint.Policies.emplace_back(std::move(endpointPortNamePolicy));

    // IP Address
    if (!IpAddress.empty())
    {
        wsl::shared::hns::IpConfig endpointIpConfig{};
        endpointIpConfig.IpAddress = IpAddress;
        hnsEndpoint.IpConfigurations.emplace_back(endpointIpConfig);
    }

    // Firewall policy
    if (m_config.FirewallConfig.Enabled())
    {
        hns::EndpointPolicy<hns::FirewallPolicySetting> endpointFirewallPolicy{};
        endpointFirewallPolicy.Settings.VmCreatorId = m_config.FirewallConfig.VmCreatorId.value();
        endpointFirewallPolicy.Settings.PolicyFlags = hns::FirewallPolicyFlags::None;
        endpointFirewallPolicy.Type = hns::EndpointPolicyType::Firewall;
        hnsEndpoint.Policies.emplace_back(std::move(endpointFirewallPolicy));
    }

    auto endpoint = wsl::core::networking::CreateEphemeralHcnEndpoint(m_network.get(), hnsEndpoint);

    return {std::move(endpoint), wsl::windows::common::hcs::GetEndpointProperties(endpoint.Endpoint.get())};
}

void NatNetworking::Initialize()
{
    auto lock = m_lock.lock_exclusive();
    wil::unique_cotaskmem_string error;
    wsl::shared::hns::HNSEndpoint endpointProperties{};

    // First try to find an existing endpoint that we can use.
    if (!m_config.NatIpAddress.empty())
    {
        PCSTR executionStep = "";
        try
        {
            // Enumerating and attaching the endpoints need to be an atomic operation between different users.
            // Keep the lock until endpoint is created.
            // Its currently safe to take this lock while holding the member m_lock
            // because g_endpointsInUseLock is only ever locked during the d'tor
            auto endpointLock = g_endpointsInUseLock.lock_exclusive();

            wil::unique_cotaskmem_string endpointsJson;
            wil::unique_cotaskmem_string endpointsError;

            // Unfortunately it's not possible to filter endpoints by IP address
            // (since internally HNS will convert the IP address field to an array of objects, and the objects themselves won't be equal
            // because the query will only have on field set), so we need to manually iterate through the endpoints on the network.
            for (const auto& id : EnumerateEndpointsByNetworkId(m_config.NatNetworkId()))
            {
                wil::unique_cotaskmem_string openEndpointError;
                wsl::windows::common::hcs::unique_hcn_endpoint openEndpoint;
                executionStep = "HcnOpenEndpoint";
                auto result = HcnOpenEndpoint(id, &openEndpoint, &openEndpointError);
                THROW_HR_IF_MSG(result, FAILED(result), "HcnOpenEndpoint %ls", openEndpointError.get());

                executionStep = "HcnQueryEndpointProperties";
                auto properties = wsl::windows::common::hcs::GetEndpointProperties(openEndpoint.get());
                if (properties.IPAddress == m_config.NatIpAddress)
                {
                    THROW_HR_IF_MSG(
                        E_UNEXPECTED,
                        std::ranges::find(g_endpointsInUse, id) != g_endpointsInUse.end(),
                        "Endpoint is in use by another address. Refusing to delete it.");

                    // TODO: this means WSL just whacked a different container's NAT address
                    //   e.g., this just broke MDAG or Sandbox if they happened to use this same address range
                    //   this sounds like a really bad idea

                    // Found an endpoint on the same network with the IP address we want: delete it so it doesn't conflict
                    // with ours.
                    LOG_HR_MSG(E_UNEXPECTED, "Found a conflicting endpoint. Deleting it");
                    openEndpoint.reset();
                    executionStep = "HcnDeleteEndpoint";
                    result = HcnDeleteEndpoint(id, &openEndpointError);
                    THROW_HR_IF_MSG(result, FAILED(result), "HcnDeleteEndpoint %ls", openEndpointError.get());
                }
            }

            // Create and attach the endpoint.
            wsl::core::networking::EphemeralHcnEndpoint endpoint;
            executionStep = "HcnCreateEndpoint";
            std::tie(endpoint, endpointProperties) = CreateEndpoint(m_config.NatIpAddress);
            executionStep = "AttachEndpoint";
            AttachEndpoint(std::move(endpoint), endpointProperties);
            g_endpointsInUse.emplace_back(endpointProperties.ID);
        }
        catch (...)
        {
            WSL_LOG(
                "ConstrainedNetworkEndpointCreationFailed",
                TraceLoggingValue(executionStep, "executionStep"),
                TraceLoggingValue("NAT", "networkingMode"),
                TraceLoggingValue(m_config.EnableDnsTunneling, "DnsTunnelingEnabled"),
                TraceLoggingValue(m_config.FirewallConfig.Enabled(), "HyperVFirewallEnabled"),
                TraceLoggingValue(m_config.EnableAutoProxy, "AutoProxyFeatureEnabled"), // the feature is enabled, but we don't know if proxy settings are actually configured
                TraceLoggingHexUInt32(wil::ResultFromCaughtException(), "result"));
        }
    }

    if (!m_endpoint.Endpoint)
    {
        PCSTR executionStep = "";
        try
        {
            // If no IP address was passed or if the endpoint couldn't be created / attached, create a new one without the IP address requirement.
            networking::EphemeralHcnEndpoint endpoint;
            executionStep = "HcnCreateEndpoint";
            std::tie(endpoint, endpointProperties) = CreateEndpoint(L"");
            executionStep = "AttachEndpoint";
            AttachEndpoint(std::move(endpoint), endpointProperties);
        }
        catch (...)
        {
            const auto hr = wil::ResultFromCaughtException();
            WSL_LOG(
                "NewEndpointCreationFailed",
                TraceLoggingValue(executionStep, "executionStep"),
                TraceLoggingValue("NAT", "networkingMode"),
                TraceLoggingValue(m_config.EnableDnsTunneling, "DnsTunnelingEnabled"),
                TraceLoggingValue(m_config.FirewallConfig.Enabled(), "HyperVFirewallEnabled"),
                TraceLoggingValue(m_config.EnableAutoProxy, "AutoProxyFeatureEnabled"), // the feature is enabled, but we don't know if proxy settings are actually configured
                TraceLoggingHexUInt32(hr, "result"));
            throw;
        }

        if (!m_config.NatIpAddress.empty())
        {
            EMIT_USER_WARNING(wsl::shared::Localization::MessageFailedToCreateNetworkEndpoint(
                m_config.NatIpAddress.c_str(), endpointProperties.IPAddress.c_str()));
        }

        // Record the new IP address associated to the endpoint.
        m_config.NatIpAddress = endpointProperties.IPAddress;
    }

    WI_ASSERT(m_endpoint.Endpoint);

    // Send the endpoint state (ip address & link) to gns
    m_gnsChannel.SendEndpointState(endpointProperties);

    // Send the default route to gns

    hns::ModifyGuestEndpointSettingRequest<hns::Route> request;
    request.RequestType = hns::ModifyRequestType::Add;
    request.ResourceType = hns::GuestEndpointResourceType::Route;
    request.Settings.NextHop = endpointProperties.GatewayAddress;
    request.Settings.DestinationPrefix = LX_INIT_DEFAULT_ROUTE_PREFIX;
    request.Settings.Family = AF_INET;

    m_gnsChannel.SendHnsNotification(ToJsonW(request).c_str(), m_endpoint.Id);

    if (m_dnsTunnelingResolver)
    {
        // Register notifications for DNS suffix changes after we create the Endpoint
        //
        // Note: DNS suffix change notifications are used only if DNS tunneling is enabled. DNS behavior for NAT mode
        // without DNS tunneling remains unchanged
        m_dnsSuffixRegistryWatcher.emplace([this] {
            const auto watcher_lock = m_lock.lock_exclusive();
            UpdateDns();
        });
    }

    // Update DNS information.
    UpdateDns(endpointProperties.GatewayAddress.c_str());

    // if using the shared access DNS proxy, ensure that the shared access service is allowed inbound UDP access.
    if (!m_mirrorDnsInfo && !m_dnsTunnelingResolver)
    {
        // N.B. This rule works around a host OS issue that prevents the DNS proxy from working on older versions of Windows.
        ConfigureSharedAccessFirewallRule();
    }

    THROW_IF_WIN32_ERROR(NotifyNetworkConnectivityHintChange(&NatNetworking::OnNetworkConnectivityChange, this, true, &m_networkNotifyHandle));

    // once the VM is created, start the telemetry timer
    if (m_connectivityTelemetryEnabled)
    {
        m_connectivityTelemetry.StartTimer([&](NLM_CONNECTIVITY hostConnectivity, uint32_t telemetryCounter) {
            TelemetryConnectionCallback(hostConnectivity, telemetryCounter);
        });
    }
}

void NatNetworking::AttachEndpoint(wsl::core::networking::EphemeralHcnEndpoint&& endpoint, const wsl::shared::hns::HNSEndpoint& properties)
{

    // for mirrored endpoints, we will set the InstanceId to the InterfaceGuid of the host interface we mirror - as we add &
    // remove them dynamically for NAT endpoints, we will just set the InstanceId to the EndpointId

    ModifySettingRequest<NetworkAdapter> networkRequest{};
    networkRequest.ResourcePath = networking::c_networkAdapterPrefix + wsl::shared::string::GuidToString<wchar_t>(properties.ID);
    networkRequest.RequestType = ModifyRequestType::Add;
    networkRequest.Settings.EndpointId = properties.ID;
    networkRequest.Settings.InstanceId = properties.ID;

    networkRequest.Settings.MacAddress = wsl::shared::string::ParseMacAddress(properties.MacAddress);
    auto retryCount = 0ul;
    const auto hr = wsl::shared::retry::RetryWithTimeout<HRESULT>(
        [&] {
            HRESULT exceptionHr = wil::ResultFromException(
                [&] { wsl::windows::common::hcs::ModifyComputeSystem(m_system, wsl::shared::ToJsonW(networkRequest).c_str()); });

            WSL_LOG(
                "NatNetworking::AttachEndpoint [ModifyComputeSystem(ModifyRequestType::Add)]",
                TraceLoggingValue(properties.ID, "endpointId"),
                TraceLoggingValue(exceptionHr, "hr"),
                TraceLoggingValue(retryCount, "retryCount"));

            ++retryCount;
            return THROW_IF_FAILED(exceptionHr);
        },
        wsl::core::networking::AddEndpointRetryPeriod,
        wsl::core::networking::AddEndpointRetryTimeout,
        wsl::core::networking::AddEndpointRetryPredicate);

    if (hr == HCN_E_ENDPOINT_ALREADY_ATTACHED)
    {
        WSL_LOG(
            "NatNetworking::AttachEndpoint [Adding the endpoint returned HCN_E_ENDPOINT_ALREADY_ATTACHED - continuing]",
            TraceLoggingValue(properties.ID, "endpointId"));
    }
    else if (FAILED(hr))
    {
        THROW_HR(hr);
    }

    m_endpoint = std::move(endpoint);
    m_networkSettings = GetEndpointSettings(properties);
}

void NatNetworking::StartPortTracker(wil::unique_socket&& socket)
{
    WI_ASSERT(false);
}

void NETIOAPI_API_ NatNetworking::OnNetworkConnectivityChange(PVOID context, NL_NETWORK_CONNECTIVITY_HINT hint)
{
    auto* thisPtr = static_cast<NatNetworking*>(context);

    thisPtr->RefreshGuestConnection(hint);
    thisPtr->m_connectivityTelemetry.UpdateTimer();
}

void NatNetworking::RefreshGuestConnection(NL_NETWORK_CONNECTIVITY_HINT connectivityHint) noexcept
try
{
    auto lock = m_lock.lock_exclusive();

    WSL_LOG(
        "NatNetworking::RefreshGuestConnection",
        TraceLoggingValue(wsl::windows::common::stringify::ToString(connectivityHint.ConnectivityLevel), "ConnectivityLevel"),
        TraceLoggingValue(wsl::windows::common::stringify::ToString(connectivityHint.ConnectivityCost), "ConnectivityCost"));

    UpdateMtu();
    UpdateDns();
}
CATCH_LOG()

_Requires_lock_held_(m_lock)
void NatNetworking::UpdateDns(std::optional<PCWSTR> gatewayAddress) noexcept
try
{
    if (!m_dnsTunnelingResolver && !m_mirrorDnsInfo && !gatewayAddress)
    {
        return;
    }

    networking::DnsInfo latestDnsSettings{};

    // true if the "domain" entry of /etc/resolv.conf should be configured
    // Note: the "domain" entry allows a single DNS suffix to be configured
    bool configureLinuxDomain = false;

    // NAT mode with DNS tunneling
    if (m_dnsTunnelingResolver)
    {
        latestDnsSettings = HostDnsInfo::GetDnsTunnelingSettings(m_dnsTunnelingIpAddress);
    }
    // NAT mode without Shared Access DNS proxy
    else if (m_mirrorDnsInfo)
    {
        m_mirrorDnsInfo->UpdateNetworkInformation();
        const auto settings = m_mirrorDnsInfo->GetDnsSettings(DnsSettingsFlags::IncludeVpn);

        latestDnsSettings.Servers = std::move(settings.Servers);

        if (!settings.Domains.empty())
        {
            latestDnsSettings.Domains.emplace_back(std::move(settings.Domains.front()));
            configureLinuxDomain = true;
        }
    }
    // NAT mode with Shared Access DNS proxy
    else if (gatewayAddress)
    {
        // set the NAT gateway address when using the NAT IPv4 DNS proxy
        latestDnsSettings.Servers.emplace_back(wsl::shared::string::WideToMultiByte(gatewayAddress.value()));
    }

    if (latestDnsSettings != m_trackedDnsSettings)
    {
        auto dnsNotification = BuildDnsNotification(latestDnsSettings, configureLinuxDomain);

        WSL_LOG(
            "NatNetworking::UpdateDns",
            TraceLoggingValue(dnsNotification.Domain.c_str(), "domain"),
            TraceLoggingValue(dnsNotification.Options.c_str(), "options"),
            TraceLoggingValue(dnsNotification.Search.c_str(), "search"),
            TraceLoggingValue(dnsNotification.ServerList.c_str(), "serverList"));

        hns::ModifyGuestEndpointSettingRequest<hns::DNS> notification{};
        notification.RequestType = hns::ModifyRequestType::Update;
        notification.ResourceType = hns::GuestEndpointResourceType::DNS;
        notification.Settings = std::move(dnsNotification);
        m_gnsChannel.SendHnsNotification(ToJsonW(notification).c_str(), m_endpoint.Id);

        m_trackedDnsSettings = std::move(latestDnsSettings);
    }
}
CATCH_LOG()

void NatNetworking::UpdateMtu()
{
    const auto minMtu = GetMinimumConnectedInterfaceMtu();

    // Only send the update if the MTU changed.
    if (minMtu && minMtu.value() != m_networkMtu)
    {
        m_networkMtu = minMtu.value();

        hns::ModifyGuestEndpointSettingRequest<hns::NetworkInterface> notification{};
        notification.ResourceType = hns::GuestEndpointResourceType::Interface;
        notification.RequestType = hns::ModifyRequestType::Update;
        notification.Settings.Connected = true;
        notification.Settings.NlMtu = m_networkMtu;

        WSL_LOG(
            "NatNetworking::UpdateMtu", TraceLoggingValue(m_endpoint.Id, "endpointId"), TraceLoggingValue(m_networkMtu, "natMtu"));

        m_gnsChannel.SendHnsNotification(ToJsonW(notification).c_str(), m_endpoint.Id);
    }
}

void NatNetworking::TraceLoggingRundown() noexcept
{
    auto lock = m_lock.lock_exclusive();

    WSL_LOG(
        "NatNetworking::TraceLoggingRundown",
        TraceLoggingValue(m_config.NatNetworkId(), "networkId"),
        TraceLoggingValue(m_endpoint.Id, "endpointId"),
        TRACE_NETWORKSETTINGS_OBJECT(m_networkSettings));
}

void NatNetworking::FillInitialConfiguration(LX_MINI_INIT_NETWORKING_CONFIGURATION& message)
{
    message.NetworkingMode = LxMiniInitNetworkingModeNat;
    message.DisableIpv6 = false;
    message.EnableDhcpClient = false;
    message.PortTrackerType = m_config.EnableLocalhostRelay ? LxMiniInitPortTrackerTypeRelay : LxMiniInitPortTrackerTypeNone;
}

// before sending anything to the container, we must wait for the NAT IP Interfaces on the host to be connected.
// there's a possible race here if the physical adapter gets connected but the NAT vNIC interface take a bit longer
std::optional<ULONGLONG> NatNetworking::FindNatInterfaceLuid(const SOCKADDR_INET& natAddress, const NL_NETWORK_CONNECTIVITY_HINT& currentConnectivityHint)
{
    constexpr ULONGLONG maxTimeToWaitMs = 10ull * 1000ull;
    constexpr ULONG timeToSleepMs = 100ul;
    const auto startTickCount = GetTickCount64();

    NET_LUID natLuid{};
    for (;;)
    {
        // HNS does not give us the interface guid/luid/index of the vNIC that is used for this NAT configuration
        // because we don't constrain our NAT interface to any one host NIC
        // we only have the assigned IPAddress - we'll have to use that to find the interface to check its state
        // this is NAT - so it's an IPv4 address
        unique_address_table addressTable;
        THROW_IF_WIN32_ERROR(GetUnicastIpAddressTable(AF_INET, &addressTable));
        for (const auto& address : wil::make_range(addressTable.get()->Table, addressTable.get()->NumEntries))
        {
            if (natAddress == address.Address)
            {
                natLuid.Value = address.InterfaceLuid.Value;
                break;
            }

            WSL_LOG(
                "NatNetworking::FindNatInterfaceLuid [IP Address comparison mismatch]",
                TraceLoggingValue(wsl::windows::common::string::SockAddrInetToString(natAddress).c_str(), "natAddress"),
                TraceLoggingValue(
                    wsl::windows::common::string::SockAddrInetToString(address.Address).c_str(), "enumeratedAddress"));
        }

        if (natLuid.Value != 0)
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
            WSL_LOG("NatNetworking::FindNatInterfaceLuid [connectivity changed while waiting for the NAT interface]");
            THROW_WIN32_MSG(ERROR_RETRY, "connectivity changed while waiting for the NAT interface");
        }
    }

    if (natLuid.Value == 0)
    {
        WSL_LOG(
            "NatNetworking::FindNatInterfaceLuid [IP address not found]",
            TraceLoggingValue(natLuid.Value, "natInterfaceLuid"),
            TraceLoggingValue(wsl::windows::common::string::SockAddrInetToString(natAddress).c_str(), "natIPAddress"));
        return {};
    }

    WSL_LOG(
        "NatNetworking::FindNatInterfaceLuid [waiting for NAT interface to be connected]",
        TraceLoggingValue(natLuid.Value, "natInterfaceLuid"),
        TraceLoggingValue(wsl::windows::common::string::SockAddrInetToString(natAddress).c_str(), "natIPAddress"));

    bool ipv4Connected = false;
    for (;;)
    {
        unique_interface_table interfaceTable{};
        THROW_IF_WIN32_ERROR(::GetIpInterfaceTable(AF_UNSPEC, &interfaceTable));
        // we only track the IPv4 interface because we only NAT IPv4 to the container
        for (auto index = 0ul; index < interfaceTable.get()->NumEntries; ++index)
        {
            const auto& ipInterface = interfaceTable.get()->Table[index];
            if (ipInterface.Family == AF_INET && !!ipInterface.Connected && ipInterface.InterfaceLuid.Value == natLuid.Value)
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
            WSL_LOG("NatNetworking::FindNatInterfaceLuid [connectivity changed while waiting for the NAT interface]");
            THROW_WIN32_MSG(ERROR_RETRY, "connectivity changed while waiting for the NAT interface");
        }
    }

    // return zero if it's not connected yet so we can retry the next cycle
    return ipv4Connected ? natLuid.Value : std::optional<ULONGLONG>();
}

wsl::windows::common::hcs::unique_hcn_network NatNetworking::CreateNetwork(wsl::core::Config& config)
{
    wsl::windows::common::hcs::unique_hcn_network natNetwork;
    wil::ResultFromException(WI_DIAGNOSTICS_INFO, [&] {
        try
        {
            wsl::core::networking::ConfigureHyperVFirewall(config.FirewallConfig, wsl::windows::common::wslutil::c_vmOwner);
            natNetwork = CreateNetworkInternal(config);
        }
        catch (...)
        {
            // Don't retry if no constraints were set.
            if (config.NatNetwork.empty() && config.NatGateway.empty())
            {
                LOG_CAUGHT_EXCEPTION();
                throw;
            }

            LOG_CAUGHT_EXCEPTION_MSG(
                "Failed to create network: '%ls' with gateway: '%ls', retrying without constraints",
                config.NatNetwork.c_str(),
                config.NatGateway.c_str());

            const auto error = wil::ResultFromCaughtException();
            WSL_LOG(
                "ConstrainedNetworkCreationFailed",
                TraceLoggingHexUInt32(error, "result"),
                TraceLoggingValue("NAT", "networkingMode"),
                TraceLoggingValue(config.EnableDnsTunneling, "DnsTunnelingEnabled"),
                TraceLoggingValue(config.FirewallConfig.Enabled(), "HyperVFirewallEnabled"),
                TraceLoggingValue(config.EnableAutoProxy, "AutoProxyFeatureEnabled") // the feature is enabled, but we don't know if proxy settings are actually configured
            );

            const auto previousRange = std::move(config.NatNetwork);
            config.NatGateway = {};
            // Note that the firewall config is NOT cleared here as we MUST always configure firewall if it has been requested
            natNetwork = CreateNetworkInternal(config);

            EMIT_USER_WARNING(wsl::shared::Localization::MessageFailedToCreateNetwork(
                previousRange.c_str(), config.NatNetwork.c_str(), wsl::windows::common::wslutil::GetSystemErrorString(error).c_str()));
        }
    });

    return natNetwork;
}

wsl::windows::common::hcs::unique_hcn_network NatNetworking::CreateNetworkInternal(wsl::core::Config& config)
{
    HRESULT hr = S_OK;
    PCSTR executionStep = "";

    // Log telemetry to determine how long it takes to create the network.
    const auto startTimeMs = GetTickCount64();

    // Log how long it takes for networking to be created
    WSL_LOG_TELEMETRY(
        "CreateNetworkBegin",
        PDT_ProductAndServicePerformance,
        TraceLoggingValue(config.NatNetworkName(), "NetworkName"),
        TraceLoggingGuid(config.NatNetworkId(), "NetworkGuid"),
        TraceLoggingValue("NAT", "networkingMode"),
        TraceLoggingValue(config.EnableDnsTunneling, "DnsTunnelingEnabled"),
        TraceLoggingValue(config.FirewallConfig.Enabled(), "HyperVFirewallEnabled"),
        TraceLoggingValue(config.EnableAutoProxy, "AutoProxyFeatureEnabled")); // the feature is enabled, but we don't know if proxy settings are actually configured

    auto createEnd = wil::scope_exit([&] {
        const auto TimeToCreateNetworkMs = GetTickCount64() - startTimeMs;
        WSL_LOG_TELEMETRY(
            "CreateNetworkEnd",
            PDT_ProductAndServicePerformance,
            TraceLoggingValue(config.NatNetworkName(), "NetworkName"),
            TraceLoggingGuid(config.NatNetworkId(), "NetworkGuid"),
            TraceLoggingValue(TimeToCreateNetworkMs, "TimeToCreateNetworkMs"),
            TraceLoggingHResult(hr, "hr"),
            TraceLoggingValue(executionStep, "executionStep"),
            TraceLoggingValue("NAT", "networkingMode"),
            TraceLoggingValue(config.EnableDnsTunneling, "DnsTunnelingEnabled"),
            TraceLoggingValue(config.FirewallConfig.Enabled(), "HyperVFirewallEnabled"),
            TraceLoggingValue(config.EnableAutoProxy, "AutoProxyFeatureEnabled")); // the feature is enabled, but we don't know if proxy settings are actually configured
    });

    auto runAsSelf = wil::run_as_self();

    // Send a HNS request to create the network.
    hns::Network settings{};
    settings.Name = config.NatNetworkName();
    settings.Type = hns::NetworkMode::ICS;
    settings.IsolateSwitch = true;
    settings.Flags = hns::NetworkFlags::EnableDns | hns::NetworkFlags::EnableNonPersistent;
    WI_SetFlagIf(settings.Flags, hns::NetworkFlags::EnableFirewall, config.FirewallConfig.Enabled());

    if (!config.NatNetwork.empty())
    {
        hns::IpSubnet netIpSubnet{};
        netIpSubnet.IpAddressPrefix = config.NatNetwork;

        hns::Subnet subnet{};
        subnet.AddressPrefix = config.NatNetwork;
        subnet.GatewayAddress = config.NatGateway;
        subnet.IpSubnets.emplace_back(std::move(netIpSubnet));
        settings.Subnets.emplace_back(std::move(subnet));
    }

    // Determine if the virtual network should be constrained by an external interface on the host.
    // For example, if the user only wants traffic to be routed if a VPN is connected.
    try
    {
        const auto lxssKey = windows::common::registry::OpenKey(HKEY_LOCAL_MACHINE, LXSS_REGISTRY_PATH, KEY_READ);
        const auto interfaceConstraint =
            windows::common::registry::ReadString(lxssKey.get(), nullptr, c_interfaceConstraintKey, L"");

        if (!interfaceConstraint.empty())
        {
            settings.Type = hns::NetworkMode::ConstrainedICS;
            settings.InterfaceConstraint.InterfaceAlias = interfaceConstraint;
        }
    }
    CATCH_LOG()

    wsl::windows::common::hcs::unique_hcn_network network{};
    try
    {
        auto retryCount = 0ul;
        wsl::shared::retry::RetryWithTimeout<void>(
            [&] {
                executionStep = "HcnCreateNetwork";
                ExecutionContext context(Context::HNS);
                wil::unique_cotaskmem_string error;
                HRESULT hns_hr = HcnCreateNetwork(config.NatNetworkId(), ToJsonW(settings).c_str(), &network, &error);
                WSL_LOG(
                    "NatNetworking::CreateNetwork [HcnCreateNetwork]",
                    TraceLoggingValue(config.NatNetworkId(), "networkGuid"),
                    TraceLoggingValue(settings.Name.c_str(), "settingsName"),
                    TraceLoggingValue(JsonEnumToString(settings.Type).c_str(), "settingsType"),
                    TraceLoggingValue(
                        settings.InterfaceConstraint.InterfaceAlias.c_str(), "settingsInterfaceConstraintInterfaceAlias"),
                    TraceLoggingValue(settings.IsolateSwitch, "settingsIsolateSwitch"),
                    TraceLoggingValue(static_cast<uint32_t>(settings.Flags), "settingsFlags"),
                    TraceLoggingValue(hns_hr, "hr"),
                    TraceLoggingValue(retryCount, "retryCount"));

                ++retryCount;

                // Open the existing network if it already exists.
                if (hns_hr == HCN_E_NETWORK_ALREADY_EXISTS)
                {
                    executionStep = "HcnOpenNetwork";
                    network = wsl::core::networking::OpenNetwork(config.NatNetworkId());
                }
                else
                {
                    // Throw other errors to allow for retries
                    THROW_IF_FAILED_MSG(hns_hr, "HcnCreateNetwork %ls", error.get());
                }

                executionStep = "HcnQueryNetworkProperties";
                // Save the networks settings in the configuration (used for WSL to save the NAT network configuration)
                auto [properties, propertiesString] = wsl::core::networking::QueryNetworkProperties(network.get());
                THROW_HR_IF_MSG(
                    E_UNEXPECTED, properties.Subnets.size() != 1, "Unexpected number of subnets in network: %ls", propertiesString.get());

                config.NatGateway = properties.Subnets[0].GatewayAddress;
                config.NatNetwork = properties.Subnets[0].AddressPrefix;
            },
            std::chrono::milliseconds(100),
            std::chrono::seconds(3));
    }
    catch (...)
    {
        hr = wil::ResultFromCaughtException();
        throw;
    }

    return network;
}
