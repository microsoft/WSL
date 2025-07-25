// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once
#include <optional>
#include <variant>
#include <vector>

#include <windows.h>
#include <ComputeNetwork.h>
#include <mstcpip.h>
#include <netioapi.h>
#include <netlistmgr.h>
#include <nldef.h>
#include <wlantypes.h>
#include <ws2ipdef.h>

#include "WslCoreConfig.h"
#include "WslTelemetry.h"
#include "hcs.hpp"

#include <wil/resource.h>

// Global operator overloads
// enabling usage of common Networking data structures within STL containers

inline bool operator==(const DOT11_SSID& lhs, const DOT11_SSID& rhs) noexcept
{
    if (lhs.uSSIDLength == rhs.uSSIDLength)
    {
        return (0 == memcmp(lhs.ucSSID, rhs.ucSSID, lhs.uSSIDLength));
    }
    return false;
}

inline bool operator!=(const DOT11_SSID& lhs, const DOT11_SSID& rhs) noexcept
{
    return !(lhs == rhs);
}

inline bool operator==(const NL_NETWORK_CONNECTIVITY_HINT& lhs, const NL_NETWORK_CONNECTIVITY_HINT& rhs) noexcept
{
    return lhs.ApproachingDataLimit == rhs.ApproachingDataLimit && lhs.ConnectivityCost == rhs.ConnectivityCost &&
           lhs.ConnectivityLevel == rhs.ConnectivityLevel && lhs.OverDataLimit == rhs.OverDataLimit && lhs.Roaming == rhs.Roaming;
}

inline bool operator!=(const NL_NETWORK_CONNECTIVITY_HINT& lhs, const NL_NETWORK_CONNECTIVITY_HINT& rhs) noexcept
{
    return !(lhs == rhs);
}

inline bool operator==(const SOCKADDR_INET& lhs, const SOCKADDR_INET& rhs) noexcept
{
    // not using INETADDR_ISEQUAL, because we can't compare the scopeId value from the v6 address
    // that's the interface index on the host

    if (lhs.si_family != rhs.si_family)
    {
        return false;
    }
    if (lhs.si_family == AF_INET)
    {
        return IN4_ADDR_EQUAL(&lhs.Ipv4.sin_addr, &rhs.Ipv4.sin_addr);
    }
    return IN6_ADDR_EQUAL(&lhs.Ipv6.sin6_addr, &rhs.Ipv6.sin6_addr);
}

inline bool operator<(const SOCKADDR_INET& lhs, const SOCKADDR_INET& rhs) noexcept
{
    if (lhs.si_family == rhs.si_family)
    {
        if (lhs.si_family == AF_INET)
        {
            return lhs.Ipv4.sin_addr.S_un.S_addr < rhs.Ipv4.sin_addr.S_un.S_addr;
        }

        // implementing the comparison operation following the shortcut from mstcpip.h IN6_ADDR_EQUAL
        const __int64 UNALIGNED* lhsRawPointer = (__int64 UNALIGNED*)(&lhs.Ipv6.sin6_addr);
        const __int64 UNALIGNED* rhsRawPointer = (__int64 UNALIGNED*)(&rhs.Ipv6.sin6_addr);
        if (lhsRawPointer[0] == rhsRawPointer[0])
        {
            return lhsRawPointer[1] < rhsRawPointer[1];
        }
        return lhsRawPointer[0] < rhsRawPointer[0];
    }
    return lhs.si_family < rhs.si_family;
}

inline bool operator>(const SOCKADDR_INET& lhs, const SOCKADDR_INET& rhs) noexcept
{
    if (lhs.si_family == rhs.si_family)
    {
        if (lhs.si_family == AF_INET)
        {
            return lhs.Ipv4.sin_addr.S_un.S_addr > rhs.Ipv4.sin_addr.S_un.S_addr;
        }

        // implementing the comparison operation following the shortcut from mstcpip.h IN6_ADDR_EQUAL
        const __int64 UNALIGNED* lhsRawPointer = (__int64 UNALIGNED*)(&lhs.Ipv6.sin6_addr);
        const __int64 UNALIGNED* rhsRawPointer = (__int64 UNALIGNED*)(&rhs.Ipv6.sin6_addr);
        if (lhsRawPointer[0] == rhsRawPointer[0])
        {
            return lhsRawPointer[1] > rhsRawPointer[1];
        }
        return lhsRawPointer[0] > rhsRawPointer[0];
    }
    return lhs.si_family > rhs.si_family;
}

inline bool operator==(const IP_ADDRESS_PREFIX& lhs, const IP_ADDRESS_PREFIX& rhs) noexcept
{
    return lhs.PrefixLength == rhs.PrefixLength && lhs.Prefix == rhs.Prefix;
}

inline bool operator<(const IP_ADDRESS_PREFIX& lhs, const IP_ADDRESS_PREFIX& rhs) noexcept
{
    if (lhs.PrefixLength == rhs.PrefixLength)
    {
        return lhs.Prefix < rhs.Prefix;
    }
    return lhs.PrefixLength < rhs.PrefixLength;
}

inline bool operator>(const IP_ADDRESS_PREFIX& lhs, const IP_ADDRESS_PREFIX& rhs) noexcept
{
    if (lhs.PrefixLength == rhs.PrefixLength)
    {
        return lhs.Prefix > rhs.Prefix;
    }
    return lhs.PrefixLength > rhs.PrefixLength;
}

namespace wsl::core::networking {

inline constexpr auto* c_ipv4TestRequestTarget = L"www.msftconnecttest.com";
inline constexpr auto* c_ipv4TestRequestTargetA = "www.msftconnecttest.com";
inline constexpr auto* c_ipv6TestRequestTarget = L"ipv6.msftconnecttest.com";
inline constexpr auto* c_ipv6TestRequestTargetA = "ipv6.msftconnecttest.com";

inline constexpr GUID c_wslFirewallVmCreatorId = {0x40E0AC32, 0x46A5, 0x438A, {0xA0, 0xB2, 0x2B, 0x47, 0x9E, 0x8F, 0x2E, 0x90}};

inline constexpr auto c_networkAdapterPrefix = L"VirtualMachine/Devices/NetworkAdapters/";
inline constexpr auto c_interfaceConstraintKey = L"ExternalInterfaceConstraint";

// RAII types to manage resources returned from NetIO APIs
using unique_notify_handle = wil::unique_any<HANDLE, decltype(CancelMibChangeNotify2), &CancelMibChangeNotify2>;
using unique_interface_table = wil::unique_any<PMIB_IPINTERFACE_TABLE, decltype(FreeMibTable), &FreeMibTable>;
using unique_address_table = wil::unique_any<PMIB_UNICASTIPADDRESS_TABLE, decltype(FreeMibTable), &FreeMibTable>;
using unique_forward_table = wil::unique_any<PMIB_IPFORWARD_TABLE2, decltype(FreeMibTable), &FreeMibTable>;
using unique_ifstack_table = wil::unique_any<PMIB_IFSTACK_TABLE, decltype(FreeMibTable), &FreeMibTable>;

inline wil::unique_couninitialize_call InitializeCOMState()
{
    // Ensure COM is initialized
    auto coInit = wil::CoInitializeEx(COINIT_MULTITHREADED);
    HRESULT hr = CoInitializeSecurity(
        nullptr, -1, nullptr, nullptr, RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_STATIC_CLOAKING, nullptr);
    // Ignore error if CoInitializeSecurity has already been invoked
    if (hr == RPC_E_TOO_LATE)
    {
        hr = S_OK;
    }
    THROW_IF_FAILED(hr);
    return coInit;
}

inline bool IsInterfaceTypeVpn(IFTYPE type) noexcept
{
    return type == IF_TYPE_PPP || type == IF_TYPE_PROP_VIRTUAL;
}

inline bool IsInterfaceHidden(IF_INDEX InterfaceIndex)
{
    NL_NETWORK_CONNECTIVITY_HINT ConnectivityHint;

    // Return true if we fail to retrieve the interface information
    if (GetNetworkConnectivityHintForInterface(InterfaceIndex, &ConnectivityHint) != NO_ERROR)
    {
        return true;
    }
    return ConnectivityHint.ConnectivityLevel == NetworkConnectivityLevelHintHidden;
}

inline bool IsMulticastOrBroadcastIpAddress(const SOCKADDR_INET& address)
{
    switch (address.si_family)
    {
    case AF_INET:
        return IN4_IS_ADDR_MULTICAST(&address.Ipv4.sin_addr) || IN4_IS_ADDR_BROADCAST(&address.Ipv4.sin_addr);
    case AF_INET6:
        return IN6_IS_ADDR_MULTICAST(&address.Ipv6.sin6_addr);
    }
    return false;
}

inline bool IsLoopbackIpAddress(const SOCKADDR_INET& address)
{
    switch (address.si_family)
    {
    case AF_INET:
        return IN4_IS_ADDR_LOOPBACK(&address.Ipv4.sin_addr);
    case AF_INET6:
        return IN6_IS_ADDR_LOOPBACK(&address.Ipv6.sin6_addr);
    }
    return false;
}

inline bool IsNetworkErrorForMissingServices(HRESULT hr) noexcept
{
    switch (hr)
    {
    case HCS_E_SERVICE_NOT_AVAILABLE:
    case HRESULT_FROM_WIN32(RPC_S_CALL_FAILED):
    case HRESULT_FROM_WIN32(EPT_S_NOT_REGISTERED):
    case HRESULT_FROM_WIN32(ERROR_SERVICE_NOT_FOUND):
    case HRESULT_FROM_WIN32(ERROR_SERVICE_DOES_NOT_EXIST):
    case HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED):
        return true;
    }
    return false;
}

inline std::string ToString(const NLM_CONNECTIVITY& nlmConnectivity)
{
    if (nlmConnectivity == NLM_CONNECTIVITY_DISCONNECTED)
    {
        return "Disconnected";
    }

    std::string returnString;
    if (nlmConnectivity & NLM_CONNECTIVITY_IPV4_NOTRAFFIC)
    {
        returnString += " IPv4NoTraffic";
    }
    if (nlmConnectivity & NLM_CONNECTIVITY_IPV6_NOTRAFFIC)
    {
        returnString += " IPv6NoTraffic";
    }
    if (nlmConnectivity & NLM_CONNECTIVITY_IPV4_SUBNET)
    {
        returnString += " IPv4Subnet";
    }
    if (nlmConnectivity & NLM_CONNECTIVITY_IPV4_LOCALNETWORK)
    {
        returnString += " IPv4Local";
    }
    if (nlmConnectivity & NLM_CONNECTIVITY_IPV4_INTERNET)
    {
        returnString += " IPv4Internet";
    }
    if (nlmConnectivity & NLM_CONNECTIVITY_IPV6_SUBNET)
    {
        returnString += " IPv6Subnet";
    }
    if (nlmConnectivity & NLM_CONNECTIVITY_IPV6_LOCALNETWORK)
    {
        returnString += " IPv6Local";
    }
    if (nlmConnectivity & NLM_CONNECTIVITY_IPV6_INTERNET)
    {
        returnString += " IPv6Internet";
    }

    return returnString;
}

enum class UpdateEndpointFlag
{
    None,
    Default,
    ResendInitialUpdate,
    ForceUpdate,
    ForceIpUpdate,
    BlockClientUpdates,
};

inline PCSTR ToString(UpdateEndpointFlag flag) noexcept
{
    switch (flag)
    {
    case UpdateEndpointFlag::None:
        return "None";
    case UpdateEndpointFlag::Default:
        return "Default";
    case UpdateEndpointFlag::ResendInitialUpdate:
        return "ResendInitialUpdate";
    case UpdateEndpointFlag::ForceUpdate:
        return "ForceUpdate";
    case UpdateEndpointFlag::ForceIpUpdate:
        return "ForceIpUpdate";
    case UpdateEndpointFlag::BlockClientUpdates:
        return "BlockClientUpdates";
    default:
        return "<unknown UpdateEndpointFlag>";
    }
}

// mapping wsl::shared::hns::* structures to the corresponding message type to send to GNS
constexpr LX_MESSAGE_TYPE GnsMessageType(const wsl::shared::hns::VmNicCreatedNotification&) noexcept
{
    return LxGnsMessageVmNicCreatedNotification;
}

constexpr LX_MESSAGE_TYPE GnsMessageType(const wsl::shared::hns::CreateDeviceRequest&) noexcept
{
    return LxGnsMessageCreateDeviceRequest;
}

constexpr LX_MESSAGE_TYPE GnsMessageType(const wsl::shared::hns::LoopbackRoutesRequest&) noexcept
{
    return LxGnsMessageLoopbackRoutesRequest;
}

constexpr LX_MESSAGE_TYPE GnsMessageType(const wsl::shared::hns::ModifyGuestDeviceSettingRequest&) noexcept
{
    return LxGnsMessageModifyGuestDeviceSettingRequest;
}

constexpr LX_MESSAGE_TYPE GnsMessageType(const wsl::shared::hns::InitialIpConfigurationNotification&) noexcept
{
    return LxGnsMessageInitialIpConfigurationNotification;
}

inline bool IsInterfaceIndexOfGelnic(DWORD InterfaceIndex) noexcept
{
    // Currently the GELNIC is indicated from HNS as an endpoint with interface index 0.
    static constexpr DWORD c_InterfaceIndexGelnic = 0;
    return InterfaceIndex == c_InterfaceIndexGelnic;
}

struct CurrentInterfaceInformation
{
    CurrentInterfaceInformation() = default;

    CurrentInterfaceInformation(
        const GUID& preferredGuid, const NET_LUID& preferredLuid, IFTYPE preferredType, std::wstring preferredName, std::wstring interfaceDescription, bool metered) :
        m_interfaceType(preferredType),
        m_interfaceName(std::move(preferredName)),
        m_interfaceDescription(std::move(interfaceDescription)),
        m_interfaceGuid(preferredGuid),
        m_interfaceLuid(preferredLuid),
        m_metered(metered)
    {
    }

    IFTYPE m_interfaceType{IF_TYPE_OTHER}; // == 1 == minimum iftype
    std::wstring m_interfaceName;
    std::wstring m_interfaceDescription;
    std::optional<GUID> m_interfaceGuid{std::nullopt};
    std::optional<NET_LUID> m_interfaceLuid{std::nullopt};
    bool m_metered{false};
};

inline std::vector<GUID> EnumerateNetworks(std::optional<wsl::shared::hns::NetworkFlags> queryFlags = {})
{
    std::wstring queryString;
    if (queryFlags)
    {
        wsl::shared::hns::HostComputeQuery query{};
        query.Filter = std::format("{{\"Flags\": {}}}", static_cast<uint32_t>(queryFlags.value()));
        queryString = wsl::shared::ToJsonW(query);
    }

    wil::unique_cotaskmem_string response;
    wil::unique_cotaskmem_string error;
    const auto result = ::HcnEnumerateNetworks(queryString.empty() ? nullptr : queryString.c_str(), &response, &error);
    THROW_IF_FAILED_MSG(result, "HcnEnumerateNetworks(%ls) %ls", queryString.empty() ? nullptr : queryString.c_str(), error.get());

    return wsl::shared::FromJson<std::vector<GUID>>(response.get());
}

inline std::vector<GUID> EnumerateEndpointsByNetworkId(const GUID& networkId)
{
    const std::wstring queryString = std::format(
        L"{{\"Filter\": \"{{\\\"VirtualNetwork\\\": \\\"{}\\\"}}\"}}",
        wsl::shared::string::GuidToString<wchar_t>(networkId, wsl::shared::string::GuidToStringFlags::None));

    wil::unique_cotaskmem_string endpointsJson;
    wil::unique_cotaskmem_string errorJson;
    const auto result = HcnEnumerateEndpoints(queryString.c_str(), &endpointsJson, &errorJson);
    THROW_IF_FAILED_MSG(result, "HcnEnumerateEndpoints failed: %ls, query: '%ls'", errorJson.get(), queryString.c_str());
    return wsl::shared::FromJson<std::vector<GUID>>(endpointsJson.get());
}

inline std::vector<GUID> EnumerateMirroredNetworksAndHyperVFirewall(bool enableFirewall)
{
    auto flags = wsl::shared::hns::NetworkFlags::EnableNonPersistent | wsl::shared::hns::NetworkFlags::EnableFlowSteering;
    WI_SetFlagIf(flags, wsl::shared::hns::NetworkFlags::EnableFirewall, enableFirewall);
    std::vector<GUID> networkIds = EnumerateNetworks(flags);
    for (auto& id : networkIds)
    {
        WSL_LOG(
            "EnumerateMirroredNetworksAndHyperVFirewall",
            TraceLoggingValue(static_cast<uint32_t>(flags), "flags"),
            TraceLoggingValue(id, "networkId"));
    }

    return networkIds;
}

inline wsl::windows::common::hcs::unique_hcn_network OpenNetwork(const GUID& networkId)
{
    wsl::windows::common::hcs::unique_hcn_network network;
    wil::unique_cotaskmem_string error;
    const auto result = ::HcnOpenNetwork(networkId, &network, &error);
    THROW_IF_FAILED_MSG(result, "HcnOpenNetwork %ls", error.get());

    return network;
}

inline std::pair<wsl::shared::hns::HNSNetwork, wil::unique_cotaskmem_string> QueryNetworkProperties(HCN_NETWORK network)
{
    wil::unique_cotaskmem_string properties;
    wil::unique_cotaskmem_string error;
    const auto result = ::HcnQueryNetworkProperties(network, nullptr, &properties, &error);
    THROW_IF_FAILED_MSG(result, "HcnQueryNetworkProperties %ls", error.get());

    auto parsed = wsl::shared::FromJson<wsl::shared::hns::HNSNetwork>(properties.get());
    return {std::move(parsed), std::move(properties)};
}

struct EphemeralHcnEndpoint
{
    EphemeralHcnEndpoint()
    {
        THROW_IF_FAILED(CoCreateGuid(&Id));
    }

    EphemeralHcnEndpoint(const EphemeralHcnEndpoint&) = delete;
    EphemeralHcnEndpoint(EphemeralHcnEndpoint&&) = default;

    EphemeralHcnEndpoint& operator=(const EphemeralHcnEndpoint&) = delete;
    EphemeralHcnEndpoint& operator=(EphemeralHcnEndpoint&&) = default;

    windows::common::hcs::unique_hcn_endpoint Endpoint;
    GUID Id{};

    ~EphemeralHcnEndpoint()
    {
        if (Endpoint)
        {
            wil::unique_cotaskmem_string error;
            const auto result = HcnDeleteEndpoint(Id, &error);
            LOG_IF_FAILED_MSG(result, "HcnDeleteEndpoint failed: %ls", error.get());
        }
    }
};

/// <summary>
/// Returns true if the host supports flow steering.
/// </summary>
bool IsFlowSteeringSupportedByHns() noexcept;

EphemeralHcnEndpoint CreateEphemeralHcnEndpoint(HCN_NETWORK network, const wsl::shared::hns::HostComputeEndpoint& endpointSettings);

std::vector<wsl::core::networking::CurrentInterfaceInformation> EnumerateConnectedInterfaces();

bool IsMetered(ABI::Windows::Networking::Connectivity::NetworkCostType cost) noexcept;

/// <summary>
/// This instance acts as an IP_ADAPTER_ADDRESS pointer.
/// </summary>
class AdapterAddresses;

/// <summary>
/// IP_ADAPTER_ADDRESSES wrapper that maintains a reference to the buffer
/// returned by GetAdaptersAddresses such that this instance always points to
/// valid data.
/// </summary>
class IpAdapterAddress
{
public:
    /// <summary>
    /// Instance constructor.
    /// </summary>
    IpAdapterAddress(const std::shared_ptr<AdapterAddresses>& AddressContainer, const IP_ADAPTER_ADDRESSES* Address) :
        m_container(AddressContainer), m_address(Address)
    {
    }

    /// <summary>
    /// This instance acts as an IP_ADAPTER_ADDRESS pointer.
    /// </summary>
    const IP_ADAPTER_ADDRESSES* operator->() const noexcept
    {
        return m_address;
    }

private:
    /// <summary>
    /// Reference to the buffer holding the IP_ADAPTER_ADDRESSES data.
    /// </summary>
    std::shared_ptr<AdapterAddresses> m_container{};

    /// <summary>
    /// Pointer into the buffer where this specific IP_ADAPTER_ADDRESSES
    /// instance begins.
    /// </summary>
    const IP_ADAPTER_ADDRESSES* m_address{};
};

/// <summary>
/// GetAdaptersAddresses wrapper
/// </summary>
class AdapterAddresses : public std::enable_shared_from_this<AdapterAddresses>
{
public:
    /// <summary>
    /// Calls GetAdaptersAddresses and returns wrapped results.
    /// </summary>
    static std::vector<IpAdapterAddress> GetCurrent()
    {
        const std::shared_ptr<AdapterAddresses> newInstance(new AdapterAddresses());
        return newInstance->Initialize();
    }

private:
    /// <summary>
    /// Default constructor is private as an instance is not directly created
    /// by callers.
    /// </summary>
    AdapterAddresses() = default;

    /// <summary>
    /// Copy constructor is not allowed.
    /// </summary>
    AdapterAddresses(const AdapterAddresses&) = delete;

    /// <summary>
    /// Internal function to do the interesting work.
    /// </summary>
    std::vector<IpAdapterAddress> Initialize()
    {
        // N.B. MSDN recommends starting with a 15K buffer as that will be sufficient on
        // most systems and the call to GetAdaptersAddresses is expensive.
        ULONG Result;
        ULONG BufferSize = (15 * 1024);
        do
        {
            m_buffer.resize(BufferSize);
            Result = GetAdaptersAddresses(
                AF_UNSPEC,
                (GAA_FLAG_SKIP_FRIENDLY_NAME | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST),
                nullptr,
                (PIP_ADAPTER_ADDRESSES)m_buffer.data(),
                &BufferSize);
        } while (Result == ERROR_BUFFER_OVERFLOW);

        THROW_LAST_ERROR_IF_MSG((Result != ERROR_SUCCESS), "GetAdaptersAddresses");
        m_buffer.resize(BufferSize);
        auto AddressBuffer = (PIP_ADAPTER_ADDRESSES)m_buffer.data();
        std::vector<IpAdapterAddress> addresses;
        while (AddressBuffer != nullptr)
        {
            addresses.emplace_back(IpAdapterAddress(shared_from_this(), AddressBuffer));
            AddressBuffer = AddressBuffer->Next;
        }

        return addresses;
    }

    /// <summary>
    /// Buffer to hold the results of GetAdaptersAddresses.
    /// </summary>
    std::vector<BYTE> m_buffer{};
};

class ConnectivityTelemetry
{
public:
    ConnectivityTelemetry() = default;
    ~ConnectivityTelemetry() = default;
    ConnectivityTelemetry(const ConnectivityTelemetry&) = delete;
    ConnectivityTelemetry& operator=(const ConnectivityTelemetry&) = delete;
    ConnectivityTelemetry(ConnectivityTelemetry&&) = delete;
    ConnectivityTelemetry& operator=(ConnectivityTelemetry&&) = delete;

    void StartTimer(std::function<void(NLM_CONNECTIVITY, uint32_t)>&& callback)
    {
        m_callback = std::move(callback);
        m_telemetryConnectionTimer.reset(CreateThreadpoolTimer(TelemetryConnectionTimerCallback, this, nullptr));
        THROW_IF_NULL_ALLOC(m_telemetryConnectionTimer);
    }

    void UpdateTimer() const noexcept
    {
        if (m_telemetryConnectionTimer)
        {
            FILETIME dueTime =
                wil::filetime::from_int64(static_cast<ULONGLONG>(-1 * wil::filetime_duration::one_millisecond * m_backoffTimeMs));
            SetThreadpoolTimer(m_telemetryConnectionTimer.get(), &dueTime, 0, 1000);
        }
    }

    void Reset() noexcept
    {
        m_telemetryConnectionTimer.reset();
    }

    static uint32_t LinuxIPv4ConnCheckResult(uint32_t returnedLinuxLevel) noexcept
    {
        // v4 is the lower-16 bits
        return returnedLinuxLevel & 0xffff;
    }
    static uint32_t LinuxIPv6ConnCheckResult(uint32_t returnedLinuxLevel) noexcept
    {
        // v6 is the higher-16 bits
        return returnedLinuxLevel >> 16;
    }
    static uint32_t WindowsIPv4NlmConnectivityLevel(NLM_CONNECTIVITY hostConnectivity) noexcept
    {
        if (hostConnectivity == NLM_CONNECTIVITY_DISCONNECTED)
        {
            return NLM_CONNECTIVITY_DISCONNECTED;
        }

        return (hostConnectivity & NLM_CONNECTIVITY_IPV4_NOTRAFFIC) | (hostConnectivity & NLM_CONNECTIVITY_IPV4_SUBNET) |
               (hostConnectivity & NLM_CONNECTIVITY_IPV4_LOCALNETWORK) | (hostConnectivity & NLM_CONNECTIVITY_IPV4_INTERNET);
    }
    static uint32_t WindowsIPv6NlmConnectivityLevel(NLM_CONNECTIVITY hostConnectivity) noexcept
    {
        if (hostConnectivity == NLM_CONNECTIVITY_DISCONNECTED)
        {
            return NLM_CONNECTIVITY_DISCONNECTED;
        }

        return (hostConnectivity & NLM_CONNECTIVITY_IPV6_NOTRAFFIC) | (hostConnectivity & NLM_CONNECTIVITY_IPV6_SUBNET) |
               (hostConnectivity & NLM_CONNECTIVITY_IPV6_LOCALNETWORK) | (hostConnectivity & NLM_CONNECTIVITY_IPV6_INTERNET);
    }

private:
    const uint32_t m_backoffTimeMs = 5000;
    std::function<void(NLM_CONNECTIVITY, uint32_t)> m_callback;
    wil::unique_threadpool_timer m_telemetryConnectionTimer;
    uint32_t m_telemetryCounter = 0;

    static void __stdcall TelemetryConnectionTimerCallback(_Inout_ PTP_CALLBACK_INSTANCE, _Inout_opt_ PVOID context, _Inout_ PTP_TIMER) noexcept
    try
    {
        const auto coInit = wil::CoInitializeEx();
        const wil::com_ptr<INetworkListManager> networkListManager = wil::CoCreateInstance<NetworkListManager, INetworkListManager>();

        NLM_CONNECTIVITY hostConnectivity{};
        THROW_IF_FAILED(networkListManager->GetConnectivity(&hostConnectivity));

        const auto updatedCounter = ++static_cast<ConnectivityTelemetry*>(context)->m_telemetryCounter;
        static_cast<ConnectivityTelemetry*>(context)->m_callback(hostConnectivity, updatedCounter);
    }
    CATCH_LOG()
};
} // namespace wsl::core::networking