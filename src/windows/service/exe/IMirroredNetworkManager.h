// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include "precomp.h"
#include "WslCoreNetworkEndpoint.h"
#include "WslCoreNetworkEndpointSettings.h"

namespace wsl::core::networking {

enum class GnsCallbackFlags
{
    DontWait = 0,
    Wait = 1
};

using GnsMessageCallback = std::function<HRESULT(LX_MESSAGE_TYPE, const std::wstring&, GnsCallbackFlags)>;
using GnsMessageCallbackWithCallbackResult =
    std::function<HRESULT(LX_MESSAGE_TYPE, const std::wstring&, GnsCallbackFlags, _Out_opt_ int*)>;
using AddNetworkEndpointCallback = std::function<void(GUID)>;

DEFINE_ENUM_FLAG_OPERATORS(GnsCallbackFlags);

class IMirroredNetworkManager
{
public:
    IMirroredNetworkManager() noexcept = default;
    virtual ~IMirroredNetworkManager() noexcept = default;

    // Disable copy and assign.
    IMirroredNetworkManager(const IMirroredNetworkManager&) = delete;
    IMirroredNetworkManager& operator=(const IMirroredNetworkManager&) = delete;

    // Disable move semantics.
    IMirroredNetworkManager(IMirroredNetworkManager&&) noexcept = delete;
    IMirroredNetworkManager& operator=(IMirroredNetworkManager&&) = delete;

    enum class HnsStatus
    {
        NoNetworkEverConnected,
        NetworkConnectedWithHnsNotification,
        NetworkConnectedNoHnsNotification
    };

    virtual HnsStatus Stop() noexcept = 0;

    virtual _Check_return_ HRESULT EnumerateNetworks(_Out_ std::vector<GUID>& NetworkIds) const noexcept = 0;

    virtual void AddEndpoint(networking::NetworkEndpoint&& newEndpoint, wsl::shared::hns::HNSEndpoint&& endpointProperties) noexcept = 0;

    virtual void SendCreateNotificationsForInitialEndpoints() noexcept = 0;

    // This API is not serialized with other API calls.
    virtual HRESULT WaitForMirroredGoalState() noexcept = 0;

    virtual _Check_return_ bool DoesEndpointExist(GUID networkId) const noexcept = 0;

    virtual void OnNetworkConnectivityHintChange() noexcept = 0;
    virtual void OnNetworkEndpointChange() noexcept = 0;
    virtual void OnDnsSuffixChange() noexcept = 0;

    virtual void TunAdapterStateChanged(_In_ const std::string& interfaceName, _In_ bool up) noexcept = 0;

    // Client should call this if they detect the network is in a bad state and needs to be reconnected
    virtual void ReconnectGuestNetwork() = 0;

    // Returns the network settings of the endpoint.
    virtual std::shared_ptr<NetworkSettings> GetEndpointSettings(const wsl::shared::hns::HNSEndpoint& endpointProperties) const = 0;

    virtual void TraceLoggingRundown() const = 0;
};

} // namespace wsl::core::networking
