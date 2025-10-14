// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"

#include "WslCoreGuestNetworkService.h"
#include "WslCoreNetworkingSupport.h"

#include <TraceLoggingProvider.h>

#include "Stringify.h"
#include "WslTelemetry.h"
#include "hns_schema.h"

static constexpr auto c_computeNetworkModuleName = L"ComputeNetwork.dll";
static constexpr auto c_dnsPortNumber = 53;
static constexpr auto c_mdnsPortNumber = 5353;
static constexpr auto c_llmnrPortNumber = 5355;

using namespace wsl::shared;

constexpr IN_ADDR c_ipv4LoopbackAddr = IN4ADDR_LOOPBACK_INIT;

std::optional<LxssDynamicFunction<decltype(HcnReserveGuestNetworkServicePortRange)>> wsl::core::networking::GuestNetworkService::m_allocatePortRange;
std::optional<LxssDynamicFunction<decltype(HcnReserveGuestNetworkServicePort)>> wsl::core::networking::GuestNetworkService::m_allocatePort;
std::optional<LxssDynamicFunction<decltype(HcnReleaseGuestNetworkServicePortReservationHandle)>> wsl::core::networking::GuestNetworkService::m_releasePort;

wsl::core::networking::GuestNetworkService::GuestNetworkService() noexcept
{
    if (wsl::core::networking::IsFlowSteeringSupportedByHns())
    {

        static std::once_flag flag;
        std::call_once(flag, [&]() {
            try
            {
                m_allocatePortRange.emplace(c_computeNetworkModuleName, "HcnReserveGuestNetworkServicePortRange");
                m_allocatePort.emplace(c_computeNetworkModuleName, "HcnReserveGuestNetworkServicePort");
                m_releasePort.emplace(c_computeNetworkModuleName, "HcnReleaseGuestNetworkServicePortReservationHandle");
            }
            CATCH_LOG()
        });
    }
}

void wsl::core::networking::GuestNetworkService::CreateGuestNetworkService(
    const bool firewallEnabled, const std::set<uint16_t>& IgnoredPorts, const GUID& VmId, const UUID& ServerUuid, HCN_NOTIFICATION_CALLBACK Callback, void* CallbackContext)
{
    // we must first enable mirrored networking - which must by done by indirectly issuing a query with these special flags
    wsl::core::networking::EnumerateMirroredNetworksAndHyperVFirewall(firewallEnabled);

    m_ignoredPorts = IgnoredPorts;
    // Always allow binds for 53. This is a workaround to unblock Docker Desktop and needs to be revisited in the future.
    m_ignoredPorts.insert(c_dnsPortNumber);

    hns::GuestNetworkService request{};
    request.VirtualMachineId = VmId;
    request.MirrorHostNetworking = true;
    request.SchemaVersion = {2, 0};

    request.GnsRpcServerInformation.EndpointType = hns::RpcEndpointType::LRpc;
    request.GnsRpcServerInformation.ObjectUuid = ServerUuid;
    WI_SetFlag(request.Flags, hns::GuestNetworkServiceFlags::IsFlowsteered);
    WI_SetFlag(request.Flags, hns::GuestNetworkServiceFlags::IsFlowsteeredSelfManaged);

    wil::unique_cotaskmem_string error;
    const auto result = ::HcnCreateGuestNetworkService(VmId, ToJsonW(request).c_str(), &m_service, &error);
    WSL_LOG(
        "GuestNetworkService::CreateGuestNetworkService [HcnCreateGuestNetworkService]",
        TraceLoggingValue(request.VirtualMachineId, "virtualMachineId"),
        TraceLoggingValue(request.MirrorHostNetworking, "mirrorHostNetworking"),
        TraceLoggingValue(request.SchemaVersion.Major, "schemaMajorVersion"),
        TraceLoggingValue(request.SchemaVersion.Minor, "schemaMinorVersion"),
        TraceLoggingValue(JsonEnumToString(request.GnsRpcServerInformation.EndpointType).c_str(), "endpointType"),
        TraceLoggingValue(request.GnsRpcServerInformation.ObjectUuid, "objectUuid"),
        TraceLoggingValue(static_cast<uint32_t>(request.Flags), "flags-value"),
        TraceLoggingHResult(result, "result"),
        TraceLoggingValue(error.is_valid() ? error.get() : L"null", "errorString"));
    THROW_IF_FAILED_MSG(result, "%ls", error.get());

    m_guestNetworkServiceCallback = windows::common::hcs::RegisterGuestNetworkServiceCallback(m_service, Callback, CallbackContext);
    SetGuestNetworkServiceState(hns::GuestNetworkServiceState::Bootstrapping);
}

void wsl::core::networking::GuestNetworkService::SetGuestNetworkServiceState(_In_ hns::GuestNetworkServiceState State) const
{
    hns::ModifyGuestNetworkServiceSettingRequest modifyRequest{};
    modifyRequest.RequestType = hns::ModifyRequestType::Update;
    modifyRequest.ResourceType = hns::GuestNetworkServiceResourceType::State;
    modifyRequest.Settings.State = State;

    const auto result = ::HcnModifyGuestNetworkService(m_service.get(), ToJsonW(modifyRequest).c_str(), nullptr);
    WSL_LOG(
        "GuestNetworkService::SetGuestNetworkServiceState [HcnModifyGuestNetworkService]",
        TraceLoggingValue(JsonEnumToString(modifyRequest.Settings.State).c_str(), "state"));
    THROW_IF_FAILED(result);
}

std::pair<uint16_t, uint16_t> wsl::core::networking::GuestNetworkService::AllocateEphemeralPortRange()
{
    FAIL_FAST_IF(!IsFlowSteeringSupportedByHns());

    const auto lock = m_dataLock.lock_exclusive();

    HANDLE port{nullptr};
    auto releasePortOnError = wil::scope_exit([&] {
        if (port)
        {
            m_releasePort.value()(port);
        }
    });

    // N.B. Use an odd number of ports to avoid Linux kernel warning about preferring different parity for start / end values.
    static constexpr auto c_ephemeralPortRangeSize = 4095;
    THROW_IF_FAILED(m_allocatePortRange.value()(m_service.get(), c_ephemeralPortRangeSize, &m_reservedPortRange, &port));

    WI_ASSERT(m_reservedPortRange.endingPort - m_reservedPortRange.startingPort == c_ephemeralPortRangeSize);

    // setting the port to zero as we do not expect any bind requests to be sent to wslcore for ports in this range
    m_reservedPorts.emplace(std::make_pair(HCN_PORT_PROTOCOL_TCP, static_cast<uint16_t>(0)), HcnPortReservation{port, 1});

    // ownership of the port was transferred successfully
    releasePortOnError.release();

    WSL_LOG(
        "GuestNetworkService::AllocateEphemeralPortRange",
        TraceLoggingValue(m_reservedPortRange.startingPort, "startingPort"),
        TraceLoggingValue(m_reservedPortRange.endingPort, "endingPort"));

    return std::make_pair(m_reservedPortRange.startingPort, m_reservedPortRange.endingPort);
}

bool wsl::core::networking::GuestNetworkService::IsPortAllocationLoopbackException(const SOCKADDR_INET& Address) noexcept
{
    // Out of IPv4 loopback address range 127.0.0.0/8, only 127.0.0.1 is used by host<->guest loopback networking scenarios.
    // FSE needs to be aware of binds using address 127.0.0.1, but can ignore binds for other IPv4 loopback addresses.
    //
    // Loopback traffic from the guest to the other IPv4 loopback addresses will stay in the guest.
    //
    // This also solves the issue of someone wanting to bind on the host to port 53 (known scenario is ICS)
    // at the same time with someone binding to port 53 in the guest - known scenarios are:
    // - DNS tunneling server that uses IP 127.0.0.42, port 53
    // - systemd DNS resolver that uses IP 127.0.0.53, port 53
    return (Address.si_family == AF_INET && IN4_IS_ADDR_LOOPBACK(&Address.Ipv4.sin_addr) && !IN4_ADDR_EQUAL(&Address.Ipv4.sin_addr, &c_ipv4LoopbackAddr));
}

bool wsl::core::networking::GuestNetworkService::IsPortAllocationMulticast(const SOCKADDR_INET& Address, _In_ int Protocol) noexcept
{
    const auto PortNumber = SS_PORT(&Address);

    if ((Address.si_family == AF_INET && IN4_IS_ADDR_MULTICAST(&Address.Ipv4.sin_addr)) ||
        (Address.si_family == AF_INET6 && IN6_IS_ADDR_MULTICAST(&Address.Ipv6.sin6_addr)))
    {
        return true;
    }
    // multicast DNS (mDNS)
    else if (Protocol == IPPROTO_UDP && PortNumber == c_mdnsPortNumber)
    {
        return true;
    }
    // LLMNR DNS
    else if (Protocol == IPPROTO_UDP && PortNumber == c_llmnrPortNumber)
    {
        return true;
    }

    return false;
}

int wsl::core::networking::GuestNetworkService::OnPortAllocationRequest(const SOCKADDR_INET& Address, _In_ int Protocol, _In_ bool Allocate) noexcept
try
{
    // The Linux and Windows constants conveniently have the same values for TCP & UDP.
    WI_ASSERT(Protocol == IPPROTO_TCP || Protocol == IPPROTO_UDP);
    WI_ASSERT(m_allocatePort.has_value() && m_releasePort.has_value());
    auto HnsProtocol = Protocol == IPPROTO_TCP ? HCN_PORT_PROTOCOL_TCP : HCN_PORT_PROTOCOL_UDP;

    const auto PortNumber = SS_PORT(&Address);
    const auto StringAddress = wsl::windows::common::string::SockAddrInetToString(Address);

    if (IsPortAllocationLoopbackException(Address))
    {
        WSL_LOG(
            "GuestNetworkService::OnPortAllocationRequest - allowing port allocation for loopback without asking FSE",
            TraceLoggingValue(StringAddress.c_str(), "IP address"),
            TraceLoggingValue(Protocol == IPPROTO_TCP ? "TCP" : "UDP", "protocol"),
            TraceLoggingValue(PortNumber, "portNumber"),
            TraceLoggingValue(Address.si_family == AF_INET ? "IPv4" : "IPv6", "address family"),
            TraceLoggingValue(Allocate, "Allocate"));
        return 0;
    }

    if (m_ignoredPorts.find(PortNumber) != m_ignoredPorts.end())
    {

        WSL_LOG(
            "GuestNetworkService::OnPortAllocationRequest - allowing port allocation for ignored port without asking FSE",
            TraceLoggingValue(StringAddress.c_str(), "IP address"),
            TraceLoggingValue(Protocol == IPPROTO_TCP ? "TCP" : "UDP", "protocol"),
            TraceLoggingValue(PortNumber, "portNumber"),
            TraceLoggingValue(Address.si_family == AF_INET ? "IPv4" : "IPv6", "address family"),
            TraceLoggingValue(Allocate, "Allocate"));
        return 0;
    }

    const auto lock = m_dataLock.lock_exclusive();

    if (PortNumber >= m_reservedPortRange.startingPort && PortNumber <= m_reservedPortRange.endingPort)
    {
        WSL_LOG(
            "GuestNetworkService::OnPortAllocationRequest",
            TraceLoggingValue(
                "Guest attempted to allocate a port but it was already allocated through port reservations", "status"),
            TraceLoggingValue(HnsProtocol == HCN_PORT_PROTOCOL_TCP ? "TCP" : "UDP", "protocol"),
            TraceLoggingValue(PortNumber, "portNumber"),
            TraceLoggingValue(StringAddress.c_str(), "IP address"));
        return 0;
    }

    HRESULT result = E_UNEXPECTED;
    const auto it = m_reservedPorts.find(std::make_pair(HnsProtocol, PortNumber));
    if (Allocate)
    {
        if (it != m_reservedPorts.end())
        {
            it->second.ReferenceCount++;
            WSL_LOG(
                "GuestNetworkService::OnPortAllocationRequest - incremented reference",
                TraceLoggingValue(PortNumber, "Port"),
                TraceLoggingValue(Address.si_family, "Family"),
                TraceLoggingValue(StringAddress.c_str(), "IP address"),
                TraceLoggingValue(Protocol, "Protocol"),
                TraceLoggingValue(it->second.ReferenceCount, "ReferenceCount"));
            return 0;
        }

        HANDLE port{nullptr};
        auto releasePortOnError = wil::scope_exit([&] {
            if (port)
            {
                m_releasePort.value()(port);
            }
        });

        bool isMulticast = IsPortAllocationMulticast(Address, Protocol);

        // Multicast port allocations are requested using the "shared" flag.
        result = m_allocatePort.value()(
            m_service.get(), HnsProtocol, isMulticast ? HCN_PORT_ACCESS_SHARED : HCN_PORT_ACCESS_EXCLUSIVE, PortNumber, &port);

        if (SUCCEEDED(result))
        {
            m_reservedPorts.emplace(std::make_pair(HnsProtocol, PortNumber), HcnPortReservation{port, 1});
        }
        // if the port was reserved, we successfully handed over ownership
        releasePortOnError.release();

        WSL_LOG(
            "GuestNetworkService::OnPortAllocationRequest [HcnReserveGuestNetworkServicePort]",
            TraceLoggingValue(HnsProtocol == HCN_PORT_PROTOCOL_TCP ? "TCP" : "UDP", "protocol"),
            TraceLoggingValue(PortNumber, "portNumber"),
            TraceLoggingValue(StringAddress.c_str(), "IP address"),
            TraceLoggingValue(isMulticast, "isMulticast"),
            TraceLoggingValue(result, "result"));
    }
    else
    {
        if (it == m_reservedPorts.end())
        {
            RETURN_HR_MSG(E_UNEXPECTED, "Guest attempted to deallocate port (%i, %i), but it's not allocated", Protocol, PortNumber);
        }

        if (it->second.ReferenceCount == 1)
        {
            result = m_releasePort.value()(it->second.Handle);
            m_reservedPorts.erase(it);
            WSL_LOG(
                "GuestNetworkService::OnPortAllocationRequest - released port",
                TraceLoggingValue(PortNumber, "Port"),
                TraceLoggingValue(Address.si_family, "Family"),
                TraceLoggingValue(StringAddress.c_str(), "IP address"),
                TraceLoggingValue(Protocol, "Protocol"));
        }
        else
        {
            it->second.ReferenceCount--;
            WSL_LOG(
                "GuestNetworkService::OnPortAllocationRequest - decremented reference",
                TraceLoggingValue(PortNumber, "Port"),
                TraceLoggingValue(Address.si_family, "Family"),
                TraceLoggingValue(StringAddress.c_str(), "IP address"),
                TraceLoggingValue(Protocol, "Protocol"),
                TraceLoggingValue(it->second.ReferenceCount, "ReferenceCount"));
            return 0;
        }
    }

    return SUCCEEDED(result) ? 0 : -LX_EADDRINUSE;
}
catch (...)
{
    LOG_CAUGHT_EXCEPTION();
    return -LX_ENOBUFS;
}

void wsl::core::networking::GuestNetworkService::Stop() noexcept
{
    if (m_releasePort)
    {
        const auto lock = m_dataLock.lock_exclusive();

        for (const auto& reservedPort : m_reservedPorts)
        {
            m_releasePort.value()(reservedPort.second.Handle);
        }
        m_reservedPorts.clear();
    }

    m_guestNetworkServiceCallback.reset();

    if (m_service)
    {
        wil::unique_cotaskmem_string error;
        const auto result = ::HcnDeleteGuestNetworkService(m_id, &error);
        LOG_IF_FAILED_MSG(result, "HcnDeleteGuestNetworkService failed, %ls", error.get());
        m_service.reset();
    }
}
