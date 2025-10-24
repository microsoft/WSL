// Copyright (C) Microsoft Corporation. All rights reserved.

#include "MirroredNetworking.h"
#include "Stringify.h"
#include "WslCoreFirewallSupport.h"
#include "WslCoreNetworkingSupport.h"
#include "WslMirroredNetworking.h"
#include "WslCoreVm.h"

using wsl::core::MirroredNetworking;
using wsl::core::networking::NetworkEndpoint;
using namespace wsl::shared;

MirroredNetworking::MirroredNetworking(HCS_SYSTEM system, GnsChannel&& gnsChannel, const Config& config, GUID runtimeId, wil::unique_socket&& dnsHvsocket) :
    m_system(system), m_runtimeId(runtimeId), m_config(config), m_gnsChannel(std::move(gnsChannel))
{
    // ensure the MTA apartment stays alive for the lifetime of this object in this process for our callback
    THROW_IF_FAILED(CoIncrementMTAUsage(&m_mtaCookie));

    // Create the DNS resolver used for DNS tunneling
    if (dnsHvsocket)
    {
        networking::DnsResolverFlags resolverFlags{};
        WI_SetFlagIf(resolverFlags, networking::DnsResolverFlags::BestEffortDnsParsing, m_config.BestEffortDnsParsing);

        m_dnsTunnelingResolver.emplace(std::move(dnsHvsocket), resolverFlags);
    }
}

MirroredNetworking::~MirroredNetworking()
{
    // Unblock GNSChannel if any calls are pended to unblock all the threadpools
    // will also unblock m_networkManager, if that's waiting for calls through the GNS channel into Linux
    m_gnsChannel.Stop();

    // Stop DNS suffix change notifications before stopping m_networkManager and m_networkingQueue, as they can call into those objects.
    m_dnsSuffixRegistryWatcher.reset();

    // Gns must unregister all callbacks first (which could call into m_networkManager)
    // Then we must shutdown the entire m_networkManager
    // Accessing m_gnsRpcServer here is safe because it's only written to in the
    // constructor, which is protected by m_instanceLock in LxssUserSessionImpl
    if (m_gnsRpcServer)
    {
        // Unregister for GNS notifications
        m_guestNetworkService.Stop();

        if (m_networkManager)
        {
            m_networkManager->Stop();
        }
    }

    // stop the TCPIP network change notifications - then stop all queued network work
    m_addressNotificationHandle.reset();
    m_routeNotificationHandle.reset();
    m_interfaceNotificationHandle.reset();
    m_networkNotificationHandle.reset();

    m_gnsPortTrackerChannel.reset();
    m_networkingQueue.cancel();
    m_gnsMessageQueue.cancel();
}

// static
bool MirroredNetworking::IsHyperVFirewallSupported(const wsl::core::Config& vmConfig) noexcept
{
    PCSTR executionStep = "";
    try
    {
        const auto hyperVFirewallSupport = wsl::core::networking::GetHyperVFirewallSupportVersion(vmConfig.FirewallConfig);

        if (hyperVFirewallSupport == wsl::core::networking::HyperVFirewallSupport::None)
        {
            WSL_LOG("IsHyperVFirewallSupported returning false: No Hyper-V Firewall API present");
            return false;
        }

        if (hyperVFirewallSupport == wsl::core::networking::HyperVFirewallSupport::Version1)
        {
            // not allowing Hyper-V Firewall support with WSL with just the Version1 Hyper-V Firewall API
            WSL_LOG("IsHyperVFirewallSupported returning false: WSL requires Hyper-V Firewall version2 but version1 is present");
            return false;
        }

        executionStep = "HcnEnumerateNetworks";
        // Check to see if the network is already created without Hyper-V Firewall.
        // HNS only supports one networking configuration per boot cycle, so if it was configured with the
        // Mirrored flag but without the Hyper-V Firewall flag, then we MUST NOT attempt to enable Hyper-V Firewall.
        for (const auto& id : wsl::core::networking::EnumerateNetworks())
        {
            executionStep = "HcnOpenNetwork";
            auto network = wsl::core::networking::OpenNetwork(id);

            executionStep = "HcnQueryNetworkProperties";
            auto [networkProperties, propertiesString] = wsl::core::networking::QueryNetworkProperties(network.get());
            if (WI_IsFlagSet(static_cast<uint32_t>(networkProperties.Flags), WI_EnumValue(wsl::shared::hns::NetworkFlags::EnableFlowSteering)) &&
                !WI_IsFlagSet(static_cast<uint32_t>(networkProperties.Flags), WI_EnumValue(wsl::shared::hns::NetworkFlags::EnableFirewall)))
            {
                WSL_LOG(
                    "IsHyperVFirewallSupported returning false: HNS Mirrored-network already created without Hyper-V Firewall "
                    "support, cannot enable Hyper-V Firewall");
                return false;
            }
        }

        return true;
    }
    catch (...)
    {
        const auto hr = wil::ResultFromCaughtException();
        WSL_LOG(
            "IsHyperVFirewallSupportedFailed",
            TraceLoggingHResult(hr, "result"),
            TraceLoggingValue(executionStep, "executionStep"),
            TraceLoggingValue("Mirrored", "networkingMode"));

        return false;
    }
}

// static
bool MirroredNetworking::IsExternalInterfaceConstrained(const HCN_NETWORK network) noexcept
{
    try
    {
        // Read interface constraint
        const auto lxssKey = windows::common::registry::OpenLxssMachineKey(KEY_READ);
        const auto interfaceConstraint =
            windows::common::registry::ReadString(lxssKey.get(), nullptr, networking::c_interfaceConstraintKey, L"");

        if (!interfaceConstraint.empty())
        {
            // The user has configured an ExternalInterfaceConstraint

            // Use GetAdapterAddresses to obtain the InterfaceGuid of the interface corresponding to the constraint
            constexpr auto GET_ADAPTER_ADDRESSES_BUFFER_SIZE_INITIAL = (15 * 1024);
            constexpr auto GAA_FLAGS = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_UNICAST | GAA_FLAG_SKIP_DNS_SERVER;

            ULONG result = ERROR_SUCCESS;
            ULONG bufferSize = GET_ADAPTER_ADDRESSES_BUFFER_SIZE_INITIAL;
            std::vector<std::byte> buffer;
            PIP_ADAPTER_ADDRESSES adapter;

            do
            {
                buffer.resize(bufferSize);
                adapter = gslhelpers::get_struct<IP_ADAPTER_ADDRESSES>(gsl::make_span(buffer));
                result = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAGS, nullptr, adapter, &bufferSize);
            } while (result == ERROR_BUFFER_OVERFLOW);

            THROW_LAST_ERROR_IF_MSG(result != ERROR_SUCCESS, "GetAdaptersAddresses");

            // Find the external interface constraint adapter (i.e. the adapter which has its friendly name matching the regkey value)
            bool interfaceConstraintPresent = false;
            while (adapter != nullptr)
            {
                if (wsl::shared::string::IsEqual(interfaceConstraint, adapter->FriendlyName, true))
                {
                    interfaceConstraintPresent = true;
                    break;
                }
                adapter = adapter->Next;
            }

            if (interfaceConstraintPresent)
            {
                // Retrieve the interfaceGuid corresponding to this endpoint by querying the HNS network
                GUID endpointInterfaceGuid{};
                wil::unique_cotaskmem_string error;
                wil::unique_cotaskmem_string networkPropertiesString;
                wsl::shared::hns::HNSNetwork networkProperties;
                try
                {
                    std::tie(networkProperties, networkPropertiesString) = wsl::core::networking::QueryNetworkProperties(network);
                }
                catch (...)
                {
                    WSL_LOG(
                        "IsExternalInterfaceConstrainedFailed",
                        TraceLoggingHResult(wil::ResultFromCaughtException(), "result"),
                        TraceLoggingValue("HcnQueryNetworkProperties", "executionStep"),
                        TraceLoggingValue("Mirrored", "networkingMode"));

                    return false;
                }

                // Successfully read the interfaceGuid for this endpoint
                endpointInterfaceGuid = networkProperties.InterfaceConstraint.InterfaceGuid;

                // Obtain ExternalInterfaceConstraint's interfaceGuid to compare against the endpoint's interfaceGuid
                GUID externalInterfaceConstraintGuid{};
                THROW_IF_WIN32_ERROR(ConvertInterfaceLuidToGuid(&(adapter->Luid), &externalInterfaceConstraintGuid));

                if (externalInterfaceConstraintGuid == endpointInterfaceGuid)
                {
                    // This interface matches the one we are looking for.
                    // There is an external interface constraint configured, the constrained
                    // interface is present, and the interface in question is the ExternalInterfaceConstraint.
                    // This interface is allowed to operate normally and must not be constrained.
                    WSL_LOG(
                        "IsExternalInterfaceConstrainedInterface",
                        TraceLoggingValue(endpointInterfaceGuid, "InterfaceGuid"),
                        TraceLoggingValue(
                            "ExternalInterfaceConstraint is configured and this interface is the "
                            "ExternalInterfaceConstraint. This interface must NOT be constrained",
                            "state"));
                    return false;
                }

                // There is an external interface constraint configured and the constrained
                // interface is present, but this is not the ExternalInterfaceConstraint.
                // Thus, this interface must be constrained.
                WSL_LOG(
                    "IsExternalInterfaceConstrainedInterface",
                    TraceLoggingValue(endpointInterfaceGuid, "InterfaceGuid"),
                    TraceLoggingValue(
                        "ExternalInterfaceConstraint is configured and the ExternalInterfaceConstraint is "
                        "found. This interface must be constrained.",
                        "state"));
                return true;
            }
            // There is an ExternalInterfaceConstraint configured, but it is not present/up.
            // Thus, this interface must be constrained.
            WSL_LOG(
                "IsExternalInterfaceConstrainedInterface",
                TraceLoggingValue(
                    "ExternalInterfaceConstraint is configured and the ExternalInterfaceConstraint is NOT "
                    "found. All interfaces must be constrained.",
                    "state"));
            return true;
        }

        // There is no ExternalInterfaceConstraint configured.
        // This, this interface must NOT be constrained.
        WSL_LOG(
            "IsExternalInterfaceConstrainedInterface",
            TraceLoggingValue("ExternalInterfaceConstraint is not configured. All interfaces must NOT be constrained.", "state"));
        return false;
    }
    CATCH_LOG()

    // If we reached here, we hit caught an unexpected error. Default to non-constrained
    return false;
}

void MirroredNetworking::Initialize()
{
    // Configure IPV6 before anything else happens (IPV6 configuration needs to be done early).
    m_networkingQueue.submit([this] {
        return wil::ResultFromException([&]() { m_gnsChannel.SendNetworkDeviceMessage(LxGnsMessageSetupIpv6, L"{}"); });
    });

    m_gnsRpcServer = GnsRpcServer::GetOrCreate();
    m_guestNetworkService.CreateGuestNetworkService(
        m_config.FirewallConfig.Enabled(), m_config.IgnoredPorts, m_runtimeId, m_gnsRpcServer->GetServerUuid(), s_GuestNetworkServiceCallback, this);
    m_ephemeralPortRange = m_guestNetworkService.AllocateEphemeralPortRange();

    networking::ConfigureHyperVFirewall(m_config.FirewallConfig, wsl::windows::common::wslutil::c_vmOwner);

    // must keep all m_networkManager interactions (including) creation queued
    // also must queue GNS callbacks to keep them serialized
    // the queue also prevents losing change notifications while we are still processing add notifications
    // calling submit_with_results to get a WslThreadPoolWaitableResult so we can conditionally wait for this workitem to complete to determine if it succeeded
    const auto workItemTracker = m_networkingQueue.submit_with_results<HRESULT>([this] {
        try
        {
            auto addNetworkEndpointCallback = [this](const GUID& networkId) {
                m_networkingQueue.submit([this, networkId] { this->AddNetworkEndpoint(networkId); });
            };

            // Create and start the network manager.
            //
            // N.B. Mirrored networks may not yet exist and the NetworkManager c'tor will cause HCS to create them asynchronously.
            //      This is done by the query submitted to HcnEnumerateNetworks.
            //      Once the networks are created, the network change callback will be invoked and endpoints will be hot-added.
            // implement wsl::core::networking::GnsMessageCallbackWithCallbackResult so WSL can serialize messages to Linux
            auto networkManagerGnsMessageCallbackWithCallbackResult = [this](
                                                                          LX_MESSAGE_TYPE messageType,
                                                                          const std::wstring& notificationString,
                                                                          networking::GnsCallbackFlags callbackFlags,
                                                                          _Out_opt_ int* returnedResult) -> HRESULT {
                // NetworkManagerGnsMessageCallback queues the actual work to the m_gnsMessageQueue
                return NetworkManagerGnsMessageCallback(messageType, notificationString, callbackFlags, returnedResult);
            };

            m_networkManager = std::make_unique<wsl::core::networking::WslMirroredNetworkManager>(
                m_system, m_config, std::move(networkManagerGnsMessageCallbackWithCallbackResult), std::move(addNetworkEndpointCallback), m_ephemeralPortRange);

            // Register notifications for DNS suffix changes
            m_dnsSuffixRegistryWatcher.emplace(
                [this] { m_networkingQueue.submit([this] { m_networkManager->OnDnsSuffixChange(); }); });

            // Send the requisite notifications for the required network devices
            m_networkManager->SendCreateNotificationsForInitialEndpoints();

            // HNS now has all host interfaces that will be mirrored mapped into NetworkIds
            std::vector<GUID> networkIds;
            THROW_IF_FAILED(m_networkManager->EnumerateNetworks(networkIds));

            // Create an endpoint on each mirrored network.
            for (auto& networkId : networkIds)
            {
                AddNetworkEndpoint(networkId);
            }

            // At this point all endpoints are configured, mark the GuestNetworkService as 'Synchronized'
            m_guestNetworkService.SetGuestNetworkServiceState(hns::GuestNetworkServiceState::Synchronized);
        }
        catch (...)
        {
            const auto hr = wil::ResultFromCaughtException();
            WSL_LOG(
                "FailedToStartNetworkManager",
                TraceLoggingValue(m_runtimeId, "vmId"),
                TraceLoggingValue(hr, "error"),
                TraceLoggingValue(ToString(m_config.NetworkingMode), "networkConfiguration"));

            return hr;
        }
        return S_OK;
    });

    // Wait for initial mirroring to give users a consistent experience.
    // the wait should not timeout - we are waiting infinite
    const auto waitResult = workItemTracker->wait(INFINITE);
    WI_ASSERT(ERROR_SUCCESS == waitResult);
    // now we can read the HRESULT returned from the work item
    const auto hr = workItemTracker->read_result();
    if (SUCCEEDED(hr))
    {
        // We must wait for the goal state to be reached outside of the queue, since operations
        // required to reach the goal state require processing in the queue.
        const auto goalStateHr = m_networkManager->WaitForMirroredGoalState();
        if (FAILED(goalStateHr))
        {
            WSL_LOG(
                "WaitForMirroredGoalStateFailed",
                TraceLoggingHResult(goalStateHr, "hr"),
                TraceLoggingValue(m_config.EnableDnsTunneling, "DnsTunnelingEnabled"),
                TraceLoggingValue(m_config.FirewallConfig.Enabled(), "HyperVFirewallEnabled"),
                TraceLoggingValue(m_config.EnableAutoProxy, "AutoProxyFeatureEnabled"));
        }
    }

    THROW_IF_FAILED(hr);
    // else we don't need to wait on the result
    // it can safely go out of scope and we can exit (it's a shared_ptr)
}

void MirroredNetworking::FillInitialConfiguration(LX_MINI_INIT_NETWORKING_CONFIGURATION& message)
{
    message.NetworkingMode = LxMiniInitNetworkingModeMirrored;

    std::tie(message.EphemeralPortRangeStart, message.EphemeralPortRangeEnd) = m_ephemeralPortRange;
    message.PortTrackerType = LxMiniInitPortTrackerTypeMirrored;
    message.EnableDhcpClient = false;
    message.DisableIpv6 = false;
}

void MirroredNetworking::StartPortTracker(wil::unique_socket&& socket)
{
    WI_ASSERT(!m_gnsPortTrackerChannel.has_value());

    m_gnsPortTrackerChannel.emplace(
        std::move(socket),
        [&](const SOCKADDR_INET& Address, int Protocol, bool Allocate) {
            return m_guestNetworkService.OnPortAllocationRequest(Address, Protocol, Allocate);
        },
        [&](_In_ const std::string& InterfaceName, _In_ bool Up) {
            m_networkingQueue.submit([=, this] {
                if (m_networkManager)
                {
                    m_networkManager->TunAdapterStateChanged(InterfaceName, Up);
                }
            });
        });
}

void MirroredNetworking::TraceLoggingRundown() noexcept
{
    m_networkingQueue.submit([this] {
        if (m_networkManager)
        {
            m_networkManager->TraceLoggingRundown();
        }
    });
}

// must be called from m_networkingQueue - m_networkManager must be called only from that queue
void MirroredNetworking::AddNetworkEndpoint(const GUID& NetworkId) noexcept
{
    PCSTR executionStep = "";
    try
    {
        WI_ASSERT(m_networkingQueue.isRunningInQueue());
        WI_ASSERT(m_networkManager);

        if (m_networkManager->DoesEndpointExist(NetworkId))
        {
            WSL_LOG(
                "MirroredNetworking::AddNetworkEndpoint - NetworkId already exists", TraceLoggingValue(NetworkId, "networkId"));
            return;
        }

        executionStep = "HcnOpenNetwork";
        auto network = wsl::core::networking::OpenNetwork(NetworkId);
        WSL_LOG("MirroredNetworking::AddNetworkEndpoint [HcnOpenNetwork]", TraceLoggingValue(NetworkId, "networkId"));

        // Query the network properties for diagnostic purposes only.
        wsl::shared::hns::HNSNetwork properties;
        wil::unique_cotaskmem_string networkProperties;
        executionStep = "HcnQueryNetworkProperties";
        std::tie(properties, networkProperties) = wsl::core::networking::QueryNetworkProperties(network.get());
        WSL_LOG(
            "MirroredNetworking::AddNetworkEndpoint [HcnQueryNetworkProperties]",
            TraceLoggingValue(NetworkId, "networkId"),
            TraceLoggingValue(networkProperties.get(), "networkProperties"));

        // Create a network endpoint.
        // first see if we have cached a prior endpoint-id that matches this network-id
        GUID endpointId{};
        const auto existingEndpointValue = m_networkIdMappings.find(NetworkId);
        if (existingEndpointValue != m_networkIdMappings.end())
        {
            endpointId = existingEndpointValue->second;
            WSL_LOG(
                "MirroredNetworking::AddNetworkEndpoint [using existing endpoint id]",
                TraceLoggingValue(NetworkId, "networkId"),
                TraceLoggingValue(endpointId, "endpointId"));
        }
        else
        {
            executionStep = "CoCreateGuid";
            THROW_IF_FAILED(CoCreateGuid(&endpointId));
        }

        std::wstring endpointSettings;
        NetworkEndpoint endpointInfo{};
        endpointInfo.NetworkId = NetworkId;
        endpointInfo.EndpointId = endpointId;

        if (m_config.FirewallConfig.Enabled())
        {
            // Create HNS firewall policy object for the endpoint
            hns::HostComputeEndpoint hnsEndpoint{};
            hns::EndpointPolicy<hns::PortnameEndpointPolicySetting> endpointPortNamePolicy{};
            hns::EndpointPolicy<hns::FirewallPolicySetting> endpointFirewallPolicy{};

            // Assemble the endpoint
            hnsEndpoint.HostComputeNetwork = NetworkId;
            hnsEndpoint.SchemaVersion.Major = 2;
            hnsEndpoint.SchemaVersion.Minor = 16;

            // Port name policy
            endpointPortNamePolicy.Type = hns::EndpointPolicyType::PortName;
            hnsEndpoint.Policies.emplace_back(std::move(endpointPortNamePolicy));

            // Firewall policy
            hns::FirewallPolicySetting firewallPolicyObject{};
            firewallPolicyObject.VmCreatorId = m_config.FirewallConfig.VmCreatorId.value();

            // Set firewall policy flags
            // Currently, only the ConstrainedInterface flag is supported, which is set based on the user configuring an ExternalInterfaceConstraint.
            firewallPolicyObject.PolicyFlags = IsExternalInterfaceConstrained(network.get()) ? hns::FirewallPolicyFlags::ConstrainedInterface
                                                                                             : hns::FirewallPolicyFlags::None;

            endpointFirewallPolicy.Settings = std::move(firewallPolicyObject);
            endpointFirewallPolicy.Type = hns::EndpointPolicyType::Firewall;
            hnsEndpoint.Policies.emplace_back(std::move(endpointFirewallPolicy));
            endpointSettings = ToJsonW(hnsEndpoint);
        }
        else
        {
            // If Hyper-V Firewall is not supported for this scenario, only configure the basic HNS endpoint object
            wsl::shared::hns::HNSEndpoint settings{};
            settings.VirtualNetwork = NetworkId;
            endpointSettings = ToJsonW(settings);
        }

        // Create the endpoint
        executionStep = "HcnCreateEndpoint";
        wil::unique_cotaskmem_string error;
        auto result = HcnCreateEndpoint(network.get(), endpointInfo.EndpointId, endpointSettings.c_str(), &endpointInfo.Endpoint, &error);

        WSL_LOG(
            "MirroredNetworking::AddNetworkEndpoint [HcnCreateEndpoint]",
            TraceLoggingValue(NetworkId, "HNSEndpoint::NetworkId"),
            TraceLoggingValue(result, "result"),
            TraceLoggingValue(error.is_valid() ? error.get() : L"", "errorString"));
        THROW_IF_FAILED_MSG(result, "HcnCreateEndpoint %ls", error.get());

        wil::unique_cotaskmem_string propertiesString;
        executionStep = "HcnQueryEndpointProperties";
        result = HcnQueryEndpointProperties(endpointInfo.Endpoint.get(), nullptr, &propertiesString, &error);
        WSL_LOG(
            "MirroredNetworking::AddNetworkEndpoint [HcnQueryEndpointProperties]",
            TraceLoggingValue(endpointInfo.EndpointId, "endpointId"),
            TraceLoggingValue(result, "result"),
            TraceLoggingValue(error.is_valid() ? error.get() : L"", "errorString"),
            TraceLoggingValue(propertiesString.is_valid() ? propertiesString.get() : L"", "propertiesString"));
        THROW_IF_FAILED_MSG(result, "HcnQueryEndpointProperties %ls", error.get());

        executionStep = "ParsingHcnQueryEndpointProperties";
        auto endpointProperties = FromJson<hns::HNSEndpoint>(propertiesString.get());

        endpointInfo.Network = m_networkManager->GetEndpointSettings(endpointProperties);
        endpointInfo.InterfaceGuid = endpointProperties.InterfaceConstraint.InterfaceGuid;

        WSL_LOG(
            "MirroredNetworking::AddNetworkEndpoint",
            TraceLoggingValue(endpointInfo.EndpointId, "endpointId"),
            TraceLoggingValue(endpointInfo.InterfaceGuid, "endpointInterfaceGuid"),
            TraceLoggingValue(endpointInfo.InterfaceLuid.Value, "endpointInterfaceLuid"),
            TraceLoggingValue(endpointProperties.IPAddress.c_str(), "endpointIpAddress"),
            TraceLoggingValue(endpointProperties.PortFriendlyName.c_str(), "endpointPortFriendlyName"),
            TraceLoggingValue(endpointProperties.Name.c_str(), "endpointName"),
            TraceLoggingValue(endpointProperties.VirtualNetwork, "endpointVirtualNetwork"),
            TraceLoggingValue(endpointProperties.VirtualNetworkName.c_str(), "endpointVirtualNetworkName"));

        m_networkManager->AddEndpoint(std::move(endpointInfo), std::move(endpointProperties));

        if (!m_networkNotificationHandle)
        {
            // Register for network connectivity change notifications to update the MTU.
            LOG_IF_WIN32_ERROR(NotifyNetworkConnectivityHintChange(
                [](PVOID context, NL_NETWORK_CONNECTIVITY_HINT hint) {
                    WSL_LOG(
                        "MirroredNetworking::NotifyNetworkConnectivityHintChange fired",
                        TraceLoggingValue(static_cast<uint32_t>(hint.ConnectivityLevel), "connectivityLevel"),
                        TraceLoggingValue(static_cast<uint32_t>(hint.ConnectivityCost), "connectivityCost"));

                    auto* thisPtr = static_cast<MirroredNetworking*>(context);
                    thisPtr->m_networkingQueue.submit([thisPtr] {
                        if (thisPtr->m_networkManager)
                        {
                            thisPtr->m_networkManager->OnNetworkConnectivityHintChange();
                        }
                    });
                },
                this,
                TRUE,
                &m_networkNotificationHandle));
        }
        if (!m_interfaceNotificationHandle)
        {
            LOG_IF_WIN32_ERROR(NotifyIpInterfaceChange(
                AF_UNSPEC,
                [](PVOID context, PMIB_IPINTERFACE_ROW row, MIB_NOTIFICATION_TYPE) {
                    WSL_LOG(
                        "MirroredNetworking::NotifyIpInterfaceChange fired",
                        TraceLoggingValue(row->Family, "family"),
                        TraceLoggingValue(row->InterfaceIndex, "ifIndex"));

                    auto* thisPtr = static_cast<MirroredNetworking*>(context);
                    thisPtr->m_networkingQueue.submit([thisPtr] {
                        if (thisPtr->m_networkManager)
                        {
                            thisPtr->m_networkManager->OnNetworkConnectivityHintChange();
                        }
                    });
                },
                this,
                FALSE,
                &m_interfaceNotificationHandle));
        }
        if (!m_routeNotificationHandle)
        {
            LOG_IF_WIN32_ERROR(NotifyRouteChange2(
                AF_UNSPEC,
                [](PVOID context, PMIB_IPFORWARD_ROW2 row, MIB_NOTIFICATION_TYPE) {
                    WSL_LOG("MirroredNetworking::NotifyRouteChange2 fired", TraceLoggingValue(row->InterfaceIndex, "ifIndex"));

                    auto* thisPtr = static_cast<MirroredNetworking*>(context);
                    thisPtr->m_networkingQueue.submit([thisPtr] {
                        if (thisPtr->m_networkManager)
                        {
                            thisPtr->m_networkManager->OnNetworkConnectivityHintChange();
                        }
                    });
                },
                this,
                FALSE,
                &m_routeNotificationHandle));
        }
        if (!m_addressNotificationHandle)
        {
            LOG_IF_WIN32_ERROR(NotifyUnicastIpAddressChange(
                AF_UNSPEC,
                [](PVOID context, PMIB_UNICASTIPADDRESS_ROW row, MIB_NOTIFICATION_TYPE) {
                    WSL_LOG(
                        "MirroredNetworking::NotifyUnicastIpAddressChange fired",
                        TraceLoggingValue(row->InterfaceIndex, "ifIndex"));

                    auto* thisPtr = static_cast<MirroredNetworking*>(context);
                    thisPtr->m_networkingQueue.submit([thisPtr] {
                        if (thisPtr->m_networkManager)
                        {
                            thisPtr->m_networkManager->OnNetworkConnectivityHintChange();
                        }
                    });
                },
                this,
                FALSE,
                &m_addressNotificationHandle));
        }

        // we've successfully added a new endpoint - track that Id
        if (existingEndpointValue == m_networkIdMappings.end())
        {
            WSL_LOG(
                "MirroredNetworking::AddNetworkEndpoint [tracking new endpoint]",
                TraceLoggingValue(NetworkId, "networkId"),
                TraceLoggingValue(endpointId, "endpointId"));
            m_networkIdMappings[NetworkId] = endpointId;
        }
    }
    catch (...)
    {
        WSL_LOG(
            "AddNetworkEndpointFailure",
            TraceLoggingHResult(wil::ResultFromCaughtException(), "result"),
            TraceLoggingValue(executionStep, "executionStep"),
            TraceLoggingValue("Mirrored", "networkingMode"),
            TraceLoggingValue(m_config.EnableDnsTunneling, "DnsTunnelingEnabled"),
            TraceLoggingValue(m_config.FirewallConfig.Enabled(), "HyperVFirewallEnabled"),
            TraceLoggingValue(m_config.EnableAutoProxy, "AutoProxyFeatureEnabled") // the feature is enabled, but we don't know if proxy settings are actually configured
        );
    }
}

// must be called from m_networkingQueue so all GNS interactions are correctly serialized
// OnNetworkEndpointChange is called from GNS
HRESULT MirroredNetworking::OnNetworkEndpointChange(const GUID& EndpointId, _In_ LPCWSTR Settings) const noexcept
try
{
    WI_ASSERT(m_networkingQueue.isRunningInQueue());

    const auto notification = FromJson<hns::ModifyGuestEndpointSettingRequest<void>>(Settings);

    // not sending Neighbor updates into the container
    if (notification.ResourceType == hns::GuestEndpointResourceType::Neighbor)
    {
        return E_NOTIMPL;
    }

    // a network property changed on some interface that HNS is tracking
    // we're using this notification as a trigger to rediscover the preferred interface
    WSL_LOG(
        "MirroredNetworking::OnNetworkEndpointChange [GNS server notification]",
        TraceLoggingValue(wsl::shared::string::GuidToString<wchar_t>(EndpointId).c_str(), "Endpoint"),
        TraceLoggingValue(Settings, "Payload"));
    m_networkManager->OnNetworkEndpointChange();

    return S_OK;
}
CATCH_RETURN()

HRESULT MirroredNetworking::NetworkManagerGnsMessageCallback(
    LX_MESSAGE_TYPE messageType, std::wstring notificationString, networking::GnsCallbackFlags callbackFlags, _Out_opt_ int* returnedValueFromGns) noexcept
try
{
    // only pass the OUT returnedValueFromGns int* if the callback flags are set to wait
    if (returnedValueFromGns)
    {
        *returnedValueFromGns = ERROR_FATAL_APP_EXIT;
        WI_ASSERT(WI_IsFlagSet(callbackFlags, wsl::core::networking::GnsCallbackFlags::Wait));
    }

    auto sendGnsMessage =
        [this, messageType, capturedNotificationString = std::move(notificationString), callbackFlags, returnedValueFromGns]() mutable {
            try
            {
                auto retryCount = 0ul;
                // RetryWithTimeout throws if fails after the timeout has elapsed - which is caught and returned by m_gnsMessageQueue below
                return wsl::shared::retry::RetryWithTimeout<HRESULT>(
                    [&]() {
                        const auto hr = wil::ResultFromException([&] {
                            if (returnedValueFromGns && WI_IsFlagSet(callbackFlags, wsl::core::networking::GnsCallbackFlags::Wait))
                            {
                                *returnedValueFromGns =
                                    m_gnsChannel.SendNetworkDeviceMessageReturnResult(messageType, capturedNotificationString.c_str());
                            }
                            else
                            {
                                m_gnsChannel.SendNetworkDeviceMessage(messageType, capturedNotificationString.c_str());
                            }
                        });
                        WSL_LOG(
                            "MirroredNetworking::NetworkManagerGnsMessageCallback",
                            TraceLoggingValue(ToString(messageType), "messageType"),
                            TraceLoggingValue(capturedNotificationString.c_str(), "notificationString"),
                            TraceLoggingValue(hr, "hr"),
                            TraceLoggingValue(returnedValueFromGns ? *returnedValueFromGns : 0xFFFFFFFF, "returnedValueFromGns"),
                            TraceLoggingValue(retryCount, "retryCount"));

                        ++retryCount;
                        return hr;
                    },
                    std::chrono::milliseconds(100),
                    std::chrono::seconds(3));
            }
            CATCH_RETURN()
        };

    if (WI_IsFlagSet(callbackFlags, wsl::core::networking::GnsCallbackFlags::Wait))
    {
        return m_gnsMessageQueue.submit_and_wait(std::move(sendGnsMessage));
    }

    m_gnsMessageQueue.submit(std::move(sendGnsMessage));
    return S_OK;
}
CATCH_RETURN()

void MirroredNetworking::GuestNetworkServiceCallback(DWORD NotificationType, HRESULT NotificationStatus, _In_opt_ PCWSTR NotificationData) noexcept
try
{
    WSL_LOG(
        "MirroredNetworking::GuestNetworkServiceCallback",
        TraceLoggingValue(wsl::windows::common::stringify::HcnNotificationsToString(NotificationType), "NotificationType"),
        TraceLoggingValue(NotificationStatus, "NotificationStatus"),
        TraceLoggingValue(NotificationData, "NotificationData"));

    WI_ASSERT(SUCCEEDED(NotificationStatus));

    hns::NotificationBase data{};
    if (ARGUMENT_PRESENT(NotificationData))
    {
        data = FromJson<hns::NotificationBase>(NotificationData);
    }

    switch (NotificationType)
    {
    case HcnNotificationServiceDisconnect:
        break;

    case HcnNotificationGuestNetworkServiceStateChanged:
        break;

    case HcnNotificationGuestNetworkServiceInterfaceStateChanged:
        break;

    default:
        WI_ASSERT(false);
    }

    return;
}
CATCH_LOG()

void CALLBACK MirroredNetworking::s_GuestNetworkServiceCallback(DWORD NotificationType, _In_ void* Context, HRESULT NotificationStatus, _In_opt_ PCWSTR NotificationData)
{
    static_cast<MirroredNetworking*>(Context)->GuestNetworkServiceCallback(NotificationType, NotificationStatus, NotificationData);
}
