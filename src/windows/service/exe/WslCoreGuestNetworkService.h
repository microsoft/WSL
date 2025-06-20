// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once
#include <optional>
#include <map>
#include <utility>

#include <ComputeNetwork.h>
#include <LxssDynamicFunction.h>

#include "hcs.hpp"

#include <wil/resource.h>

namespace wsl::core::networking {
class GuestNetworkService
{
public:
    GuestNetworkService() noexcept;

    ~GuestNetworkService() noexcept
    {
        Stop();
    }

    GuestNetworkService(const GuestNetworkService& other) = delete;
    GuestNetworkService(GuestNetworkService&& other) = delete;
    GuestNetworkService& operator=(const GuestNetworkService& other) = delete;
    GuestNetworkService& operator=(GuestNetworkService&& other) = delete;

    void CreateGuestNetworkService(
        const bool firewallEnabled,
        const std::set<USHORT>& IgnoredPorts,
        const GUID& VmId,
        const UUID& ServerUuid,
        HCN_NOTIFICATION_CALLBACK Callback,
        void* CallbackContext);

    void SetGuestNetworkServiceState(_In_ wsl::shared::hns::GuestNetworkServiceState State) const;

    std::pair<uint16_t, uint16_t> AllocateEphemeralPortRange();

    int OnPortAllocationRequest(const SOCKADDR_INET& Address, _In_ int Protocol, _In_ bool Allocate) noexcept;

    void Stop() noexcept;

private:
    struct HcnPortReservation
    {
        // The consumer of the port reservations requests reservations at a {SOCKADDR_INET, Protocol} granularity.
        // The HCN port reservation API allows reservations at a {PortNumber, Protocol} granularity.
        // Using a reference count to coalesce consumer requests to their appropriate HCN requests.
        HANDLE Handle;
        ULONG ReferenceCount;
    };

    // Returns true if the port allocation should be always allowed, without Windows.
    static bool IsPortAllocationLoopbackException(const SOCKADDR_INET& Address) noexcept;

    static bool IsPortAllocationMulticast(const SOCKADDR_INET& Address, _In_ int Protocol) noexcept;

    static std::optional<LxssDynamicFunction<decltype(HcnReserveGuestNetworkServicePortRange)>> m_allocatePortRange;
    static std::optional<LxssDynamicFunction<decltype(HcnReserveGuestNetworkServicePort)>> m_allocatePort;
    static std::optional<LxssDynamicFunction<decltype(HcnReleaseGuestNetworkServicePortReservationHandle)>> m_releasePort;

    wsl::windows::common::hcs::unique_hcn_guest_network_service m_service;
    wsl::windows::common::hcs::unique_hcn_guest_network_service_callback m_guestNetworkServiceCallback;
    GUID m_id{};
    wil::srwlock m_dataLock;
    _Guarded_by_(m_dataLock) std::set<uint16_t> m_ignoredPorts;
    _Guarded_by_(m_dataLock) std::map<std::pair<HCN_PORT_PROTOCOL, USHORT>, HcnPortReservation> m_reservedPorts;
    _Guarded_by_(m_dataLock) HCN_PORT_RANGE_RESERVATION m_reservedPortRange {};
};
} // namespace wsl::core::networking
