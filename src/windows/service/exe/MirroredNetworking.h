// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include "precomp.h"
#include "INetworkingEngine.h"
#include "GnsChannel.h"
#include "DnsResolver.h"
#include "WslCoreConfig.h"
#include "WslCoreNetworkEndpointSettings.h"
#include "WslCoreMessageQueue.h"
#include "GnsPortTrackerChannel.h"
#include "GnsRpcServer.h"
#include "WslCoreGuestNetworkService.h"
#include "IMirroredNetworkManager.h"

namespace wsl::core {

class MirroredNetworking : public INetworkingEngine
{
public:
    MirroredNetworking(HCS_SYSTEM system, GnsChannel&& gnsChannel, const Config& config, GUID runtimeId, wil::unique_socket&& dnsHvsocket);
    ~MirroredNetworking() override;

    MirroredNetworking(const MirroredNetworking&) = delete;
    MirroredNetworking& operator=(const MirroredNetworking) = delete;
    MirroredNetworking(MirroredNetworking&&) = delete;
    MirroredNetworking& operator=(MirroredNetworking&&) = delete;

    void Initialize() override;

    void TraceLoggingRundown() noexcept override;

    void FillInitialConfiguration(LX_MINI_INIT_NETWORKING_CONFIGURATION& message) override;

    void StartPortTracker(wil::unique_socket&& socket) override;

    /// <summary>
    /// Returns true if the interface should be constrained, false otherwise.
    ///
    /// This function determines if the input InterfaceGuid corresponds to an interface
    /// that should be constrained. One can configure the ExternalInterfaceConstraint, which means
    /// that all interfaces OTHER than the ExternalInterfaceConstraint will have its traffic constrained
    /// (i.e restricted to only local subnet access).
    ///
    /// This function returns TRUE if there is an ExternalInterfaceConstraint configured AND
    /// this interface does not match the ExternalInterfaceConstraint (which means that this interface is
    /// restricted to communicate ONLY over the local subnet)
    /// This function returns FALSE otherwise (which means that this interface has no restrictions on it)
    ///
    /// If any errors occur while trying to determine the ExternalInterfaceConstraint, this function will
    /// default to returning FALSE (i.e non-constrained, normal traffic allowed interface)
    /// </summary>
    static bool IsExternalInterfaceConstrained(const HCN_NETWORK network) noexcept;

    static bool IsHyperVFirewallSupported(const wsl::core::Config& vmConfig) noexcept;

private:
    void AddNetworkEndpoint(const GUID& NetworkId) noexcept;

    HRESULT OnNetworkEndpointChange(const GUID& Endpoint, _In_ LPCWSTR Settings) const noexcept;

    // callbacks
    HRESULT NetworkManagerGnsMessageCallback(
        LX_MESSAGE_TYPE messageType, std::wstring notificationString, networking::GnsCallbackFlags callbackFlags, _Out_opt_ int* returnedValueFromGns) noexcept;
    static void GuestNetworkServiceCallback(DWORD NotificationType, HRESULT NotificationStatus, _In_opt_ PCWSTR NotificationData) noexcept;
    static void CALLBACK s_GuestNetworkServiceCallback(DWORD NotificationType, _In_ void* Context, HRESULT NotificationStatus, _In_opt_ PCWSTR NotificationData);

    // Handle owned by WslCoreVm
    const HCS_SYSTEM m_system{};
    const GUID m_runtimeId;
    const Config& m_config;

    // holding the MTA for our COM callback
    wsl::windows::common::helpers::unique_mta_cookie m_mtaCookie{};
    std::optional<GnsPortTrackerChannel> m_gnsPortTrackerChannel;
    std::shared_ptr<GnsRpcServer> m_gnsRpcServer;
    // mutable allows m_gnsMessageQueue to submit from const methods
    mutable WslCoreMessageQueue m_gnsMessageQueue;
    networking::GuestNetworkService m_guestNetworkService;

    // m_network* and m_gnsChannel must be accessed only from within the m_networkingQueue
    // which serializes all workitems through a single-threaded queue
    // This unwinds the locking/dependencies with the GNS channel (and its callbacks) and HNS APIs (Hcn*)
    GnsChannel m_gnsChannel;
    std::unique_ptr<networking::IMirroredNetworkManager> m_networkManager;
    mutable WslCoreMessageQueue m_networkingQueue;

    // Optional DNS resolver used for DNS tunneling
    std::optional<networking::DnsResolver> m_dnsTunnelingResolver;

    std::optional<networking::DnsSuffixRegistryWatcher> m_dnsSuffixRegistryWatcher;

    std::optional<networking::NetworkSettings> m_networkPreferredSettings;
    ULONG m_networkNatMtu = ULONG_MAX;
    networking::unique_notify_handle m_networkNotificationHandle{};
    networking::unique_notify_handle m_interfaceNotificationHandle{};
    networking::unique_notify_handle m_routeNotificationHandle{};
    networking::unique_notify_handle m_addressNotificationHandle{};

    // track network-id to endpoint-id
    // we can avoid recreating vmNICs by reusing the same endpoint-id values
    std::map<GUID, GUID, wsl::windows::common::helpers::GuidLess> m_networkIdMappings;

    // Ephemeral port range allocated for the VM.
    std::pair<uint16_t, uint16_t> m_ephemeralPortRange;
};

} // namespace wsl::core
