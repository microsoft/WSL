/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    hcs_schema.h

Abstract:

    This file contains the host networking service schema definitions.

--*/

#pragma once
#include <variant>
#include "JsonUtils.h"

namespace wsl::shared::hns {

enum NetworkFlags
{
    None = 0,
    EnableDns = 1,
    EnableDhcp = 2,
    EnableMirroring = 4,
    EnableNonPersistent = 8,
    EnablePersistent = 16,
    IsolateVSwitch = 32,
    EnableFlowSteering = 64,
    DisableSharing = 128,
    EnableFirewall = 256,
    SuppressMediaDisconnect = 512,
    DisableHostPort = 1024,
    WeakHostReceiveAdapter = 2048,
    WeakHostSendAdapter = 4096,
    EnableIov = 8192,
};

DEFINE_ENUM_FLAG_OPERATORS(NetworkFlags);

struct HostComputeQuery
{
    std::uint64_t Flags{};
    std::string Filter{};

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(HostComputeQuery, Flags, Filter);
};

struct Version
{
    uint32_t Major{};
    uint32_t Minor{};

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(Version, Major, Minor);
};

enum class EndpointPolicyType
{
    PortName = 9,
    Firewall = 18
};

NLOHMANN_JSON_SERIALIZE_ENUM(
    EndpointPolicyType,
    {
        {EndpointPolicyType::PortName, "PortName"},
        {EndpointPolicyType::Firewall, "Firewall"},
    })

struct PortnameEndpointPolicySetting
{
    std::wstring Name;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(PortnameEndpointPolicySetting, Name);
};

enum class FirewallPolicyFlags
{
    None = 0,
    ConstrainedInterface = 1
};

struct FirewallPolicySetting
{
    GUID VmCreatorId{};
    FirewallPolicyFlags PolicyFlags{};

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_ONLY_SERIALIZE(FirewallPolicySetting, VmCreatorId, PolicyFlags);
};

template <typename T>
struct EndpointPolicy
{
    EndpointPolicyType Type{};
    T Settings{};
};

template <typename T>
inline void to_json(nlohmann::json& j, const EndpointPolicy<T>& policy)
{
    j = nlohmann::json{{"Type", policy.Type}, {"Settings", policy.Settings}};
}

struct IpConfig
{
    std::wstring IpAddress;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_ONLY_SERIALIZE(IpConfig, IpAddress);
};

template <typename... Args>
inline void to_json(nlohmann::json& j, const std::variant<Args...>& variant)
{
    std::visit<void>([&j](auto e) { to_json(j, e); }, variant);
}

struct HostComputeEndpoint
{
    std::vector<std::variant<EndpointPolicy<PortnameEndpointPolicySetting>, EndpointPolicy<FirewallPolicySetting>>> Policies;
    std::vector<IpConfig> IpConfigurations;
    Version SchemaVersion{};
    GUID HostComputeNetwork{};

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_ONLY_SERIALIZE(HostComputeEndpoint, Policies, IpConfigurations, SchemaVersion, HostComputeNetwork);
};

struct InterfaceConstraint
{
    GUID InterfaceGuid{};
    uint32_t InterfaceIndex{};
    uint32_t InterfaceMediaType{};
    std::wstring InterfaceAlias{};

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(InterfaceConstraint, InterfaceGuid, InterfaceIndex, InterfaceMediaType, InterfaceAlias);
};

struct HNSEndpoint
{
    std::wstring IPAddress;
    std::wstring MacAddress;
    std::wstring GatewayAddress;
    std::wstring PortFriendlyName;
    GUID VirtualNetwork{};
    std::wstring VirtualNetworkName;
    std::wstring Name;
    GUID ID{};
    uint8_t PrefixLength{};
    InterfaceConstraint InterfaceConstraint{};
    std::wstring DNSServerList;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(
        HNSEndpoint, IPAddress, MacAddress, GatewayAddress, PortFriendlyName, VirtualNetwork, VirtualNetworkName, Name, ID, PrefixLength, InterfaceConstraint, DNSServerList);
};

struct Route
{
    std::wstring NextHop;
    std::wstring DestinationPrefix;
    uint8_t SitePrefixLength{};
    uint32_t Metric{};
    uint16_t Family{};

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(Route, NextHop, DestinationPrefix, SitePrefixLength, Metric, Family);
};

enum class ModifyRequestType
{
    Add = 0,
    Remove = 1,
    Update = 2,
    Refresh = 3,
    Reset = 4,
};

NLOHMANN_JSON_SERIALIZE_ENUM(
    ModifyRequestType,
    {
        {ModifyRequestType::Add, "Add"},
        {ModifyRequestType::Remove, "Remove"},
        {ModifyRequestType::Update, "Update"},
        {ModifyRequestType::Refresh, "Refresh"},
        {ModifyRequestType::Reset, "Reset"},
    })

enum class GuestEndpointResourceType
{
    Interface = 0,
    Route = 1,
    IPAddress = 2,
    DNS = 3,
    MacAddress = 6,
    Neighbor = 10,
    Port = 15
};

NLOHMANN_JSON_SERIALIZE_ENUM(
    GuestEndpointResourceType,
    {
        {GuestEndpointResourceType::Interface, "Interface"},
        {GuestEndpointResourceType::Route, "Route"},
        {GuestEndpointResourceType::IPAddress, "IPAddress"},
        {GuestEndpointResourceType::DNS, "DNS"},
        {GuestEndpointResourceType::MacAddress, "MacAddress"},
        {GuestEndpointResourceType::Neighbor, "Neighbor"},
        {GuestEndpointResourceType::Port, "Port"},
    })

struct DNS
{
    std::wstring Domain;
    std::wstring Search;
    std::wstring ServerList;
    std::wstring Options;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(DNS, Domain, Search, ServerList, Options);
};

struct IPAddress
{
    std::wstring Address;
    uint16_t Family{};
    uint8_t OnLinkPrefixLength{};
    uint8_t PrefixOrigin{};
    uint8_t SuffixOrigin{};
    uint32_t PreferredLifetime{};

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(IPAddress, Address, Family, OnLinkPrefixLength, PrefixOrigin, SuffixOrigin, PreferredLifetime);
};

enum class OperationType
{
    Create,
    Update,
    Remove
};

NLOHMANN_JSON_SERIALIZE_ENUM(
    OperationType, {{OperationType::Create, "Create"}, {OperationType::Update, "Update"}, {OperationType::Remove, "Remove"}})

struct LoopbackRoutesRequest
{
    std::wstring targetDeviceName;
    OperationType operation{};
    uint32_t family{};
    std::wstring ipAddress;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(LoopbackRoutesRequest, operation, targetDeviceName, family, ipAddress);
};

struct NetworkInterface
{
    bool Connected{};
    uint32_t NlMtu{};
    uint32_t Metric{};

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(NetworkInterface, Connected, NlMtu, Metric);
};

enum class InitialIpConfigurationNotificationFlags
{
    None = 0x0,
    SkipPrimaryRoutingTableUpdate = 0x1,
    SkipLoopbackRouteReset = 0x2
};

DEFINE_ENUM_FLAG_OPERATORS(InitialIpConfigurationNotificationFlags);

struct InitialIpConfigurationNotification
{
    std::wstring targetDeviceName;
    InitialIpConfigurationNotificationFlags flags{};

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(InitialIpConfigurationNotification, targetDeviceName, flags);
};

struct VmNicCreatedNotification
{
    GUID adapterId{};

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(VmNicCreatedNotification, adapterId);
};

enum class DeviceType
{
    Bond,
    Loopback,
    VirtualWifi,
    VirtualTunnel,
    VirtualCellular
};

NLOHMANN_JSON_SERIALIZE_ENUM(
    DeviceType,
    {
        {DeviceType::Bond, "Bond"},
        {DeviceType::Loopback, "Loopback"},
        {DeviceType::VirtualWifi, "VirtualWifi"},
        {DeviceType::VirtualTunnel, "VirtualTunnel"},
        {DeviceType::VirtualCellular, "VirtualCellular"},
    })

struct CreateDeviceRequest
{
    DeviceType type{};
    std::wstring deviceName;
    std::optional<GUID> lowerEdgeAdapterId;
    std::optional<std::wstring> lowerEdgeDeviceName;
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT_FROM_ONLY(CreateDeviceRequest, type, deviceName, lowerEdgeAdapterId, lowerEdgeDeviceName);

inline void to_json(nlohmann::json& j, const CreateDeviceRequest& request)
{
    j = nlohmann::json{{"type", request.type}, {"deviceName", request.deviceName}};

    if (request.lowerEdgeAdapterId.has_value())
    {
        j["lowerEdgeAdapterId"] = request.lowerEdgeAdapterId.value();
    }

    if (request.lowerEdgeDeviceName.has_value())
    {
        j["lowerEdgeDeviceName"] = request.lowerEdgeDeviceName.value();
    }
}

struct ModifyGuestDeviceSettingRequest
{
    std::wstring targetDeviceName;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(ModifyGuestDeviceSettingRequest, targetDeviceName);
};

struct InterfaceNetFilterRequest
{
    std::wstring targetDeviceName;
    OperationType operation{};
    uint16_t ephemeralPortRangeStart{};
    uint16_t ephemeralPortRangeEnd{};

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(InterfaceNetFilterRequest, targetDeviceName, operation, ephemeralPortRangeStart, ephemeralPortRangeEnd);
};

struct MacAddress
{
    std::string PhysicalAddress;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(MacAddress, PhysicalAddress);
};

template <typename T>
struct ModifyGuestEndpointSettingRequest
{
    ModifyRequestType RequestType{};
    GuestEndpointResourceType ResourceType{};
    T Settings{};

    std::optional<std::wstring> targetDeviceName;
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT_FROM_ONLY(
    ModifyGuestEndpointSettingRequest<NetworkInterface>, RequestType, ResourceType, targetDeviceName, Settings);

template <>
struct ModifyGuestEndpointSettingRequest<void>
{
    ModifyRequestType RequestType{};
    GuestEndpointResourceType ResourceType{};
    std::optional<std::wstring> targetDeviceName;
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT_FROM_ONLY(ModifyGuestEndpointSettingRequest<void>, RequestType, ResourceType, targetDeviceName);

template <typename T>
inline void to_json(nlohmann::json& j, const ModifyGuestEndpointSettingRequest<T>& request)
{
    j = nlohmann::json{{"ResourceType", request.ResourceType}, {"RequestType", request.RequestType}};

    if (request.targetDeviceName.has_value())
    {
        j["targetDeviceName"] = request.targetDeviceName.value();
    }

    if constexpr (!std::is_same_v<T, void>)
    {
        j["Settings"] = request.Settings;
    }
}

struct IpSubnet
{
    std::wstring IpAddressPrefix;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(IpSubnet, IpAddressPrefix);
};

struct Subnet
{
    std::wstring GatewayAddress;
    std::wstring AddressPrefix;
    std::vector<IpSubnet> IpSubnets;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(Subnet, GatewayAddress, AddressPrefix, IpSubnets);
};

struct HNSNetwork
{
    std::wstring ID;
    std::wstring Name;
    std::wstring SourceMac;
    std::wstring DNSSuffix;
    std::wstring DNSServerList;
    std::wstring DNSDomain;
    std::vector<Subnet> Subnets;
    NetworkFlags Flags{};
    InterfaceConstraint InterfaceConstraint{};

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(HNSNetwork, ID, Name, SourceMac, DNSSuffix, DNSServerList, DNSDomain, Subnets, Flags, InterfaceConstraint);
};

enum class NetworkMode
{
    NAT,
    ICS,
    ConstrainedICS
};

NLOHMANN_JSON_SERIALIZE_ENUM(
    NetworkMode,
    {
        {NetworkMode::NAT, "NAT"},
        {NetworkMode::ICS, "ICS"},
        {NetworkMode::ConstrainedICS, "ConstrainedICS"},
    })

struct Network
{
    std::wstring Name;
    NetworkMode Type{};
    bool IsolateSwitch{};
    NetworkFlags Flags{};
    std::vector<Subnet> Subnets;
    InterfaceConstraint InterfaceConstraint{};

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(Network, Name, Type, IsolateSwitch, Subnets, Flags, InterfaceConstraint);
};

struct NotificationBase
{
    GUID ID{};
    uint32_t Flags{};

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(NotificationBase, ID, Flags);
};

enum class RpcEndpointType
{
    LRpc
};

NLOHMANN_JSON_SERIALIZE_ENUM(
    RpcEndpointType,
    {
        {RpcEndpointType::LRpc, "LRpc"},
    })

struct RpcConnectionInformation
{
    RpcEndpointType EndpointType{};
    GUID ObjectUuid{};

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(RpcConnectionInformation, EndpointType, ObjectUuid);
};

enum class GuestNetworkServiceFlags
{
    IsFlowsteered = 1,
    IsFlowsteeredSelfManaged = 2,
};

DEFINE_ENUM_FLAG_OPERATORS(GuestNetworkServiceFlags);

struct GuestNetworkService
{
    GUID VirtualMachineId{};
    bool MirrorHostNetworking{};
    Version SchemaVersion{};
    RpcConnectionInformation GnsRpcServerInformation{};
    GuestNetworkServiceFlags Flags{};

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(GuestNetworkService, VirtualMachineId, MirrorHostNetworking, SchemaVersion, GnsRpcServerInformation, Flags);
};

enum class GuestNetworkServiceState
{
    None,
    Created,
    Bootstrapping,
    Synchronized,
    Paused,
    Desynchronized,
    Rehydrating,
    Degraded,
    Destroyed,
};

NLOHMANN_JSON_SERIALIZE_ENUM(
    GuestNetworkServiceState,
    {
        {GuestNetworkServiceState::None, "None"},
        {GuestNetworkServiceState::Created, "Created"},
        {GuestNetworkServiceState::Bootstrapping, "Bootstrapping"},
        {GuestNetworkServiceState::Synchronized, "Synchronized"},
        {GuestNetworkServiceState::Paused, "Paused"},
        {GuestNetworkServiceState::Desynchronized, "Desynchronized"},
        {GuestNetworkServiceState::Rehydrating, "Rehydrating"},
        {GuestNetworkServiceState::Degraded, "Degraded"},
        {GuestNetworkServiceState::Destroyed, "Destroyed"},
    })

struct GuestNetworkServiceStateRequest
{
    GuestNetworkServiceState State{};

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(GuestNetworkServiceStateRequest, State);
};

enum GuestNetworkServiceResourceType
{
    State
};

NLOHMANN_JSON_SERIALIZE_ENUM(GuestNetworkServiceResourceType, {{GuestNetworkServiceResourceType::State, "State"}})

struct ModifyGuestNetworkServiceSettingRequest
{
    GuestNetworkServiceResourceType ResourceType{};
    GuestNetworkServiceStateRequest Settings{};
    ModifyRequestType RequestType{};

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(ModifyGuestNetworkServiceSettingRequest, ResourceType, RequestType, Settings);
};

} // namespace wsl::shared::hns