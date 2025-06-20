// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include "INetworkingEngine.h"
#include "GnsChannel.h"
#include "WslCoreHostDnsInfo.h"
#include "GnsPortTrackerChannel.h"

namespace wsl::core {

using AddGuestDeviceRoutine = std::function<GUID(const GUID& clsid, const GUID& deviceId, PCWSTR tag, PCWSTR options)>;
using ModifyOpenPortsCallback = std::function<int(const GUID& clsid, PCWSTR tag, const SOCKADDR_INET& addr, int protocol, bool isOpen)>;
using GuestInterfaceStateChangeCallback = std::function<void(const std::string& name, bool isUp)>;

class VirtioNetworking : public INetworkingEngine
{
public:
    VirtioNetworking(GnsChannel&& gnsChannel, const Config& config);
    ~VirtioNetworking() = default;

    VirtioNetworking& OnAddGuestDevice(const AddGuestDeviceRoutine& addGuestDeviceRoutine);
    VirtioNetworking& OnModifyOpenPorts(const ModifyOpenPortsCallback& modifyOpenPortsCallback);
    VirtioNetworking& OnGuestInterfaceStateChanged(const GuestInterfaceStateChangeCallback& guestInterfaceStateChangedCallback);

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
    void RefreshGuestConnection(NL_NETWORK_CONNECTIVITY_HINT hint) noexcept;
    void SetupLoopbackDevice();
    void UpdateDns(wsl::shared::hns::DNS&& dnsSettings);
    void UpdateMtu();

    mutable wil::srwlock m_lock;

    std::optional<AddGuestDeviceRoutine> m_addGuestDeviceRoutine;
    GnsChannel m_gnsChannel;
    std::optional<GnsPortTrackerChannel> m_gnsPortTrackerChannel;
    std::shared_ptr<networking::NetworkSettings> m_networkSettings;
    const Config& m_config;
    GUID m_localhostAdapterId;
    GUID m_adapterId;
    std::optional<NL_NETWORK_CONNECTIVITY_LEVEL_HINT> m_connectivityLevel;
    std::optional<NL_NETWORK_CONNECTIVITY_COST_HINT> m_connectivityCost;
    std::optional<ModifyOpenPortsCallback> m_modifyOpenPortsCallback;
    std::optional<GuestInterfaceStateChangeCallback> m_guestInterfaceStateChangeCallback;

    std::optional<ULONGLONG> m_interfaceLuid;
    ULONG m_networkMtu = 0;
    std::optional<wsl::core::networking::HostDnsInfo> m_dnsInfo;

    // Note: this field must be destroyed first to stop the callbacks before any other field is destroyed.
    networking::unique_notify_handle m_networkNotifyHandle;

    // 16479D2E-F0C3-4DBA-BF7A-04FFF0892B07
    static constexpr GUID c_virtioNetworkClsid = {0x16479D2E, 0xF0C3, 0x4DBA, {0xBF, 0x7A, 0x04, 0xFF, 0xF0, 0x89, 0x2B, 0x07}};
    // F07010D0-0EA9-447F-88EF-BD952A4D2F14
    static constexpr GUID c_virtioNetworkDeviceId = {0xF07010D0, 0x0EA9, 0x447F, {0x88, 0xEF, 0xBD, 0x95, 0x2A, 0x4D, 0x2F, 0x14}};
};

} // namespace wsl::core
