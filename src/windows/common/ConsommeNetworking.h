// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include "INetworkingEngine.h"
#include "GnsChannel.h"
#include "WslCoreHostDnsInfo.h"
#include "GnsPortTrackerChannel.h"
#include "GuestDeviceManager.h"

namespace wsl::core {

enum class ConsommeNetworkingFlags
{
    None = 0x0,
    LocalhostRelay = 0x1,
    DnsTunneling = 0x2,
    Ipv6 = 0x4,
    LoopbackClientIp = 0x8,
};
DEFINE_ENUM_FLAG_OPERATORS(ConsommeNetworkingFlags);

class ConsommeNetworking : public INetworkingEngine
{
public:
    ConsommeNetworking(GnsChannel&& gnsChannel, ConsommeNetworkingFlags flags, LPCWSTR dnsOptions, std::shared_ptr<GuestDeviceManager> guestDeviceManager, wil::shared_handle userToken);

    ~ConsommeNetworking() override;

    // Note: This class cannot be moved because m_networkNotifyHandle captures a 'this' pointer.
    ConsommeNetworking(const ConsommeNetworking&) = delete;
    ConsommeNetworking(ConsommeNetworking&&) = delete;
    ConsommeNetworking& operator=(const ConsommeNetworking&) = delete;
    ConsommeNetworking& operator=(ConsommeNetworking&&) = delete;

    // INetworkingEngine
    void Initialize() override;
    void TraceLoggingRundown() noexcept override;
    void FillInitialConfiguration(LX_MINI_INIT_NETWORKING_CONFIGURATION& message) override;
    void StartPortTracker(wil::unique_socket&& socket) override;

    HRESULT MapPort(_In_ const SOCKADDR_INET& ListenAddress, _In_ USHORT GuestPort, _In_ int Protocol, _Out_ USHORT* AllocatedHostPort) const;

    HRESULT UnmapPort(_In_ const SOCKADDR_INET& ListenAddress, _In_ USHORT GuestPort, _In_ int Protocol) const;

private:
    static void NETIOAPI_API_ OnNetworkConnectivityChange(PVOID context, NL_NETWORK_CONNECTIVITY_HINT hint);

    uint16_t HandlePortNotification(const SOCKADDR_INET& addr, int protocol, uint16_t guestPort, bool allocate) const;
    uint16_t ModifyOpenPorts(
        _In_ PCWSTR tag, _In_ const SOCKADDR_INET& hostAddress, _In_ uint16_t HostPort, _In_ uint16_t GuestPort, _In_ int protocol, _In_ bool isOpen) const;
    void RefreshGuestConnection();
    void SetupLoopbackDevice();
    void SendDefaultRoute(const std::wstring& gateway, wsl::shared::hns::ModifyRequestType requestType);
    void SendIpv6Address(const networking::EndpointIpAddress& ipAddress, wsl::shared::hns::ModifyRequestType requestType);
    void UpdateDefaultRoute(const std::wstring& gateway);
    void UpdateDnsSettings(const networking::DnsInfo& dns);
    void UpdateIpv4Address(const networking::EndpointIpAddress& ipAddress);
    void UpdateIpv6Address(const networking::EndpointIpAddress& ipAddress);
    void UpdateMtu(std::optional<ULONG> mtu);

    mutable wil::srwlock m_lock;

    std::shared_ptr<GuestDeviceManager> m_guestDeviceManager;
    wil::shared_handle m_userToken;
    GnsChannel m_gnsChannel;
    std::optional<GnsPortTrackerChannel> m_gnsPortTrackerChannel;
    std::shared_ptr<networking::NetworkSettings> m_networkSettings;
    ConsommeNetworkingFlags m_flags = ConsommeNetworkingFlags::None;
    LPCWSTR m_dnsOptions = nullptr;
    std::optional<GUID> m_localhostAdapterId;
    std::optional<GUID> m_adapterId;

    ULONG m_networkMtu = 0;
    networking::EndpointIpAddress m_trackedIpv4Address{};
    networking::EndpointIpAddress m_trackedIpv6Address{};
    std::wstring m_trackedDefaultRoute;
    networking::DnsInfo m_trackedDnsSettings{};

    // Note: this field must be destroyed first to stop the callbacks before any other field is destroyed.
    networking::unique_notify_handle m_networkNotifyHandle;
};

} // namespace wsl::core
