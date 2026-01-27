// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include "INetworkingEngine.h"
#include "GnsChannel.h"
#include "WslCoreHostDnsInfo.h"
#include "GnsPortTrackerChannel.h"
#include "GuestDeviceManager.h"

namespace wsl::core {

class VirtioNetworking : public INetworkingEngine
{
public:
    VirtioNetworking(GnsChannel&& gnsChannel, bool enableLocalhostRelay, std::shared_ptr<GuestDeviceManager> guestDeviceManager, wil::shared_handle userToken);
    ~VirtioNetworking();

    // Note: This class cannot be moved because m_networkNotifyHandle captures a 'this' pointer.
    VirtioNetworking(const VirtioNetworking&) = delete;
    VirtioNetworking(VirtioNetworking&&) = delete;
    VirtioNetworking& operator=(const VirtioNetworking&) = delete;
    VirtioNetworking& operator=(VirtioNetworking&&) = delete;

    // INetworkingEngine
    void Initialize() override;
    void TraceLoggingRundown() noexcept override;
    void FillInitialConfiguration(LX_MINI_INIT_NETWORKING_CONFIGURATION& message) override;
    void StartPortTracker(wil::unique_socket&& socket) override;

    void StartLegacyPortTracker(wil::unique_socket&& socket);

private:
    static void NETIOAPI_API_ OnNetworkConnectivityChange(PVOID context, NL_NETWORK_CONNECTIVITY_HINT hint);
    static std::optional<ULONGLONG> FindVirtioInterfaceLuid(const SOCKADDR_INET& virtioAddress, const NL_NETWORK_CONNECTIVITY_HINT& currentConnectivityHint);

    HRESULT HandlePortNotification(const SOCKADDR_INET& addr, int protocol, bool allocate) const noexcept;
    int ModifyOpenPorts(_In_ PCWSTR tag, _In_ const SOCKADDR_INET& addr, _In_ int protocol, _In_ bool isOpen) const;
    void RefreshGuestConnection(NL_NETWORK_CONNECTIVITY_HINT hint) noexcept;
    void SetupLoopbackDevice();
    void SendDnsUpdate(const networking::DnsInfo& dnsSettings);
    void UpdateMtu();

    mutable wil::srwlock m_lock;

    std::shared_ptr<GuestDeviceManager> m_guestDeviceManager;
    wil::shared_handle m_userToken;
    GnsChannel m_gnsChannel;
    std::optional<GnsPortTrackerChannel> m_gnsPortTrackerChannel;
    std::shared_ptr<networking::NetworkSettings> m_networkSettings;
    bool m_enableLocalhostRelay;
    GUID m_localhostAdapterId;
    GUID m_adapterId;

    std::optional<ULONGLONG> m_interfaceLuid;
    ULONG m_networkMtu = 0;
    networking::DnsUpdateHelper m_dnsUpdateHelper;
    networking::DnsInfo m_trackedDnsSettings;

    // Note: this field must be destroyed first to stop the callbacks before any other field is destroyed.
    networking::unique_notify_handle m_networkNotifyHandle;
};

} // namespace wsl::core
