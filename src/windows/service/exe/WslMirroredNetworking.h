/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WslMirroredNetworking.h

Abstract:

    This file contains WSL mirrored networking function declarations.

--*/

#pragma once
#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include <mstcpip.h>
#include <ws2ipdef.h>
#include <netlistmgr.h>
#include <ComputeNetwork.h>
#include <wil/winrt.h>

#include "WslCoreMessageQueue.h"
#include "WslCoreAdviseHandler.h"
#include "WslCoreNetworkEndpoint.h"
#include "WslCoreNetworkEndpointSettings.h"
#include "WslCoreNetworkingSupport.h"
#include "WslCoreTcpIpStateTracking.h"
#include "WslCoreHostDnsInfo.h"
#include "hcs.hpp"
#include "IMirroredNetworkManager.h"

/// <summary>
/// Creates network-related information for WSL.
/// </summary>
namespace wsl::core::networking {

class WslMirroredNetworkManager final : public wsl::core::networking::IMirroredNetworkManager
{
public:
    WslMirroredNetworkManager(
        HCS_SYSTEM hcsSystem,
        const Config& config,
        GnsMessageCallbackWithCallbackResult&& GnsMessageCallbackWithCallbackResult,
        AddNetworkEndpointCallback&& addNetworkEndpointCallback,
        const std::pair<uint16_t, uint16_t>& ephemeralPortRange);

    ~WslMirroredNetworkManager() noexcept override;

    // Disable copy and assign.
    WslMirroredNetworkManager(const WslMirroredNetworkManager&) = delete;
    WslMirroredNetworkManager& operator=(const WslMirroredNetworkManager&) = delete;

    // Disable move semantics.
    WslMirroredNetworkManager(WslMirroredNetworkManager&&) noexcept = delete;
    WslMirroredNetworkManager& operator=(WslMirroredNetworkManager&&) = delete;

    HnsStatus Stop() noexcept override;

    _Check_return_ HRESULT EnumerateNetworks(_Out_ std::vector<GUID>& NetworkIds) const noexcept override;

    void AddEndpoint(NetworkEndpoint&& newEndpoint, wsl::shared::hns::HNSEndpoint&& endpointProperties) noexcept override;

    void SendCreateNotificationsForInitialEndpoints() noexcept override;

    HRESULT WaitForMirroredGoalState() noexcept override;

    _Check_return_ bool DoesEndpointExist(GUID networkId) const noexcept override;

    void OnNetworkConnectivityHintChange() noexcept override;
    void OnNetworkEndpointChange() noexcept override;
    void OnDnsSuffixChange() noexcept override;

    void TunAdapterStateChanged(_In_ const std::string& interfaceName, _In_ bool up) noexcept override;

    // Client should call this if they detect the network is in a bad state and needs to be reconnected
    void ReconnectGuestNetwork() override;

    std::shared_ptr<NetworkSettings> GetEndpointSettings(const wsl::shared::hns::HNSEndpoint& endpointProperties) const override;

    void TraceLoggingRundown() const override;

private:
    enum class State
    {
        Stopped = 0,
        Started,
        Starting,
    };

    static const char* StateToString(State state) noexcept;

    _Requires_lock_held_(m_networkLock)
    std::vector<GUID> EnumerateMirroredNetworks() const noexcept;

    _Requires_lock_held_(m_networkLock)
    _Check_return_ HRESULT AddNetwork(const GUID& networkId) noexcept;

    _Requires_lock_held_(m_networkLock)
    _Check_return_ HRESULT RemoveNetwork(const GUID& networkId) noexcept;

    _Requires_lock_held_(m_networkLock)
    _Check_return_ HRESULT RemoveEndpoint(const GUID& endpointId) noexcept;

    struct EndpointTracking
    {
        EndpointTracking(NetworkEndpoint&& networkEndpoint, wsl::shared::hns::HNSEndpoint&& hnsEndpoint, uint32_t retryCount) :
            m_networkEndpoint{std::move(networkEndpoint)}, m_hnsEndpoint{std::move(hnsEndpoint)}, m_retryCount{retryCount}
        {
        }
        ~EndpointTracking() noexcept = default;
        EndpointTracking(const EndpointTracking&) = delete;
        EndpointTracking& operator=(const EndpointTracking&) = delete;
        EndpointTracking(EndpointTracking&&) = default;
        EndpointTracking& operator=(EndpointTracking&&) = default;

        NetworkEndpoint m_networkEndpoint;
        wsl::shared::hns::HNSEndpoint m_hnsEndpoint;
        uint32_t m_retryCount = 0;
    };

    _Requires_lock_held_(m_networkLock)
    void AddEndpointImpl(EndpointTracking&& endpointTrackingObject) noexcept;

    _Requires_lock_held_(m_networkLock)
    void ProcessConnectivityChange();

    _Requires_lock_held_(m_networkLock)
    void ProcessInterfaceChange();

    _Requires_lock_held_(m_networkLock)
    void ProcessIpAddressChange();

    _Requires_lock_held_(m_networkLock)
    void ProcessRouteChange();

    _Requires_lock_held_(m_networkLock)
    void ProcessDNSChange();

    _Requires_lock_held_(m_networkLock)
    _Check_return_ HRESULT SendAddressRequestToGns(
        const NetworkEndpoint& endpoint, const TrackedIpAddress& address, wsl::shared::hns::ModifyRequestType requestType) noexcept;

    _Requires_lock_held_(m_networkLock)
    _Check_return_ HRESULT SendRouteRequestToGns(const NetworkEndpoint& endpoint, const TrackedRoute& route, wsl::shared::hns::ModifyRequestType requestType) noexcept;

    _Requires_lock_held_(m_networkLock)
    _Check_return_ HRESULT SendLoopbackRequestToGns(
        const NetworkEndpoint& endpoint, const TrackedIpAddress& address, wsl::shared::hns::OperationType operation) noexcept;

    _Requires_lock_held_(m_networkLock)
    _Check_return_ HRESULT SendDnsRequestToGns(const NetworkEndpoint& endpoint, const DnsInfo& dnsInfo, wsl::shared::hns::ModifyRequestType requestType) noexcept;

    _Requires_lock_held_(m_networkLock)
    _Check_return_ HRESULT SendInterfaceRequestToGns(const NetworkEndpoint& endpoint) noexcept;

    _Requires_lock_not_held_(m_networkLock)
    void UpdateAllEndpoints(_In_ PCSTR sourceName) noexcept;

    _Requires_lock_held_(m_networkLock)
    void UpdateAllEndpointsImpl(UpdateEndpointFlag updateFlag, _In_ PCSTR callingSource) noexcept;

    _Requires_lock_held_(m_networkLock)
    _Check_return_ HRESULT UpdateHcnServiceTimer() noexcept;

    _Requires_lock_held_(m_networkLock)
    _Check_return_ HRESULT ResetHcnServiceSession() noexcept;

    _Requires_lock_held_(m_networkLock)
    _Check_return_ bool SyncIpStateWithLinux(NetworkEndpoint& endpoint);

    _Requires_lock_held_(m_networkLock)
    NetworkSettings GetNetworkSettingsOfInterface(DWORD ifIndex) const;

    void TelemetryConnectionCallback(NLM_CONNECTIVITY hostConnectivity, uint32_t telemetryCounter) noexcept;

    // protects access to member variables as well as operations that generate callback messages
    // methods which lead to GNS messages being sent must maintain the order in which the caller invoked them
    // thus exclusive access will be guaranteed for these methods, even if we don't need write-protection to member variables
    mutable wil::srwlock m_networkLock;

    // Member variables used to limit calls through UpdatePreferredEndpoint(UpdateEndpointFlag::Default) to every 350ms.
    // This is because WslMirroredNetworkManager uses an eventing model where we will often see many 10s of events fired back-to-back
    static constexpr uint32_t m_debounceUpdateAllEndpointsTimerMs = 350;
    _Guarded_by_(m_networkLock) uint64_t m_lastUpdateAllEndpointsDefaultTime = 0;
    bool m_IsDebounceUpdateAllEndpointsDefaultTimerSet = false;
    _Requires_lock_held_(m_networkLock)
    wil::unique_threadpool_timer m_debounceUpdateAllEndpointsDefaultTimer;
    static void __stdcall DebounceUpdateAllEndpointsDefaultTimerFired(_Inout_ PTP_CALLBACK_INSTANCE, _Inout_opt_ PVOID Context, _Inout_ PTP_TIMER);

    // Member variables tracking resiliency attempts to create endpoints in the container for indicated networkIds from HNS
    static constexpr uint32_t m_maxAddEndpointRetryCount = 3;
    static constexpr uint32_t m_debounceCreateEndpointFailureTimerMs = 1000;
    _Requires_lock_held_(m_networkLock)
    std::vector<EndpointTracking> m_failedEndpointProperties;
    _Requires_lock_held_(m_networkLock)
    wil::unique_threadpool_timer m_debounceCreateEndpointFailureTimer;
    static void __stdcall DebounceCreateEndpointFailureTimerFired(_Inout_ PTP_CALLBACK_INSTANCE, _Inout_opt_ PVOID Context, _Inout_ PTP_TIMER);

    // Member variables tracking the WinRT and COM networking APIs required
    wsl::windows::common::helpers::unique_mta_cookie m_mtaCookie;
    wil::com_ptr<ABI::Windows::Networking::Connectivity::INetworkInformationStatics> m_networkInformationStatics;
    wil::com_ptr<INetworkListManager> m_netListManager;
    wil::com_ptr<INetworkEvents> m_netListManagerEventSink;
    WslCoreAdviseHandler m_netListManagerAdviseHandler;

    _Guarded_by_(m_networkLock) HnsStatus m_latestHnsStatus { HnsStatus::NoNetworkEverConnected };

    // Members tracking all endpoints created in the container, and the current networks connected
    _Guarded_by_(m_networkLock) std::vector<NetworkEndpoint> m_networkEndpoints;
    _Guarded_by_(m_networkLock) std::set<GUID, wsl::windows::common::helpers::GuidLess> m_hostConnectedInterfaces;

    // Members tracking callback functors back through the parent
    _Guarded_by_(m_networkLock) GnsMessageCallbackWithCallbackResult m_callbackForGnsMessage;
    // the AddNetworkEndpointCallback is called through the m_gnsMessageQueue
    // so we don't risk deadlocks if the callback chooses to call back into WslMirroredNetworkManager
    _Guarded_by_(m_networkLock) AddNetworkEndpointCallback m_addNetworkEndpointCallback;

    // The DNS info synced into the guest
    _Guarded_by_(m_networkLock) DnsInfo m_trackedDnsInfo;
    // The current DNS info on the host
    _Guarded_by_(m_networkLock) DnsInfo m_dnsInfo;
    // m_hostDnsInfo is an optimization used to avoid allocating a large buffer every time we call
    // GetAdaptersAddresses when querying host DNS info
    _Guarded_by_(m_networkLock) HostDnsInfo m_hostDnsInfo;

    std::wstring m_dnsTunnelingIpAddress;

    // Tracks whether we are in the mirrored goal state or not.
    _Guarded_by_(m_networkLock) wil::unique_event m_inMirroredGoalState { wil::EventOptions::ManualReset };

    ConnectivityTelemetry m_connectivityTelemetry;

    // Used for telemetry to see how long it takes to reach the mirrored goal state for the first time.
    std::chrono::time_point<std::chrono::steady_clock> m_objectCreationTime = std::chrono::steady_clock::now();
    std::chrono::time_point<std::chrono::steady_clock> m_initialMirroredGoalStateEndTime;

    // Handle for the Hcn* Api. Owned by the caller (WslCoreVm), this is a non-owning copy
    const HCS_SYSTEM m_hcsSystem{};

    // Config of the WslCoreVm.
    const Config& m_vmConfig;

    // Ephemeral port range allocated for the VM.
    std::pair<uint16_t, uint16_t> m_ephemeralPortRange;

    // All guest related messages sent back through callbacks to Linux (GNS)
    // must be queued in order into a single queue.
    WslCoreMessageQueue m_gnsCallbackQueue;

    // All host-configuration messages, either back to the parent MirroredNetworking or to HNS/HCS
    // must have their own queue as to not be blocked by Linux messages.
    WslCoreMessageQueue m_hnsQueue;

    // Callback timer that is invoked when the HNS service goes down to attempt to re-establish contact
    static void __stdcall HcnServiceConnectionTimerCallback(_Inout_ PTP_CALLBACK_INSTANCE, _Inout_opt_ PVOID Context, _Inout_ PTP_TIMER) noexcept;
    wil::unique_threadpool_timer m_retryHcnServiceConnectionTimer;
    _Guarded_by_(m_networkLock) DWORD m_retryHcnServiceConnectionDurationMs = 0;

    // Callback that is invoked by HNS when a service-wide notification
    // is available (e.g. when a HNS network is created or deleted).
    static void __stdcall HcnCallback(_In_ DWORD NotificationType, _In_opt_ void* Context, _In_ HRESULT NotificationStatus, _In_opt_ PCWSTR NotificationData) noexcept;
    windows::common::hcs::unique_hcn_service_callback m_hcnCallback;

    _Guarded_by_(m_networkLock) State m_state { State::Stopped };

    // Callback timer that is invoked when we want to retry syncing the latest Windows IP state with Linux
    static void __stdcall RetryLinuxIpStateSyncTimerCallback(_Inout_ PTP_CALLBACK_INSTANCE, _Inout_opt_ PVOID Context, _Inout_ PTP_TIMER) noexcept;
    wil::unique_threadpool_timer m_retryLinuxIpStateSyncTimer;
    static constexpr uint32_t m_linuxIpStateRetryDebounceTimerMinMilliseconds = 100ul;
    static constexpr uint32_t m_linuxIpStateRetryDebounceTimerMaxMilliseconds = 2000ul;
    uint32_t m_linuxIpStateRetryDebounceTimerMilliseconds = m_linuxIpStateRetryDebounceTimerMinMilliseconds;

    class PublicNLMSink final : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, INetworkEvents>
    {
        WslMirroredNetworkManager* m_parent{};

    public:
        explicit PublicNLMSink(WslMirroredNetworkManager* parent) : m_parent(parent)
        {
        }

        ~PublicNLMSink() override = default;

        PublicNLMSink(const PublicNLMSink&) = delete;
        PublicNLMSink& operator=(const PublicNLMSink&) = delete;
        PublicNLMSink(PublicNLMSink&&) = delete;
        PublicNLMSink& operator=(PublicNLMSink&&) = delete;

        // INetworkEvents
        IFACEMETHODIMP NetworkAdded(GUID networkId) override
        {
            m_parent->UpdateAllEndpoints("INetworkEvents");
            return S_OK;
        }

        IFACEMETHODIMP NetworkDeleted(GUID networkId) override
        {
            m_parent->UpdateAllEndpoints("INetworkEvents");
            return S_OK;
        }

        IFACEMETHODIMP NetworkConnectivityChanged(GUID networkId, NLM_CONNECTIVITY connectivity) override
        {
            m_parent->UpdateAllEndpoints("INetworkEvents");
            return S_OK;
        }

        IFACEMETHODIMP NetworkPropertyChanged(GUID networkId, NLM_NETWORK_PROPERTY_CHANGE property) override
        {
            m_parent->UpdateAllEndpoints("INetworkEvents");
            return S_OK;
        }
    };
};

} // namespace wsl::core::networking
