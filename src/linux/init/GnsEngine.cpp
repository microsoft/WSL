// Copyright (C) Microsoft Corporation. All rights reserved.

#include <iostream>
#include <locale>
#include <regex>
#include <filesystem>
#include <format>
#include <fstream>
#include "address.h"
#include "common.h"
#include "GnsEngine.h"
#include "util.h"
#include "Utils.h"
#include "lxinitshared.h"
#include "stringshared.h"

using wsl::shared::hns::GuestEndpointResourceType;
using wsl::shared::hns::ModifyGuestEndpointSettingRequest;
using wsl::shared::hns::ModifyRequestType;

constexpr auto c_interfaceLookupTimeout = std::chrono::seconds(30);
constexpr auto c_interfaceLookupRetryPeriod = std::chrono::milliseconds(100);
constexpr auto c_ipStrings = {"ip", "ip6"};

const char* c_loopbackInterfaceName = "lo";

GnsEngine::GnsEngine(
    const NotificationRoutine& notificationRoutine,
    const StatusRoutine& statusRoutine,
    NetworkManager& manager,
    std::optional<int> dnsTunnelingFd,
    const std::string& dnsTunnelingIpAddress) :
    notificationRoutine(notificationRoutine), statusRoutine(statusRoutine), manager(manager)
{
    if (dnsTunnelingFd.has_value())
    {
        // Add the IP address to the loopback interface, to be used by the DNS tunneling listener.
        // Note: Linux allows IPv4 addresses that are not in the range 127.0.0.0/8 to be added to the loopback interface.
        auto loInterface = Interface::Open(c_loopbackInterfaceName);
        Address address{AF_INET, 32, dnsTunnelingIpAddress};
        manager.ModifyAddress(loInterface, address, Operation::Create);

        dnsTunnelingManager.emplace(dnsTunnelingFd.value(), dnsTunnelingIpAddress);
    }
}

Interface GnsEngine::OpenAdapterImpl(const GUID& id)
{
    std::string interfaceName;
    for (const auto& e : std::filesystem::directory_iterator("/sys/class/net/"))
    {
        auto adapterId = GetAdapterId(e.path());
        if (adapterId.has_value() && adapterId.value() == id)
        {
            interfaceName = e.path().filename().string();
            // Special case _wlanxx interfaces: look for the wlanxx version instead.
            if (interfaceName.compare(0, 5, "_wlan") == 0)
            {
                continue;
            }

            break;
        }
    }

    if (!interfaceName.empty())
    {
        GNS_LOG_INFO(
            "Found an interface matching the GUID {}, with name {}",
            wsl::shared::string::GuidToString<char>(id).c_str(),
            interfaceName.c_str());
        return Interface::Open(interfaceName);
    }

    throw RuntimeErrorWithSourceLocation(std::format("Couldn't find an adapter for id: {}", wsl::shared::string::GuidToString<char>(id)));
}

Interface GnsEngine::OpenAdapter(const GUID& id)
{
    return wsl::shared::retry::RetryWithTimeout<Interface>([&]() { return OpenAdapterImpl(id); }, c_interfaceLookupRetryPeriod, c_interfaceLookupTimeout);
}

Interface GnsEngine::OpenInterfaceImpl(const std::string& deviceName)
{
    try
    {
        return Interface::Open(deviceName);
    }
    catch (const std::exception& e)
    {
        throw RuntimeErrorWithSourceLocation(std::format("Failed to open interface with device name: {}", deviceName), e);
    }
}

Interface GnsEngine::OpenInterface(const std::string& deviceName)
{
    return wsl::shared::retry::RetryWithTimeout<Interface>(
        [&]() { return OpenInterfaceImpl(deviceName); }, c_interfaceLookupRetryPeriod, c_interfaceLookupTimeout);
}

std::optional<GUID> GnsEngine::GetAdapterId(const std::string& path)
{
    // Sample symlink:
    // /sys/class/net/eth0/device -> ../../devices/LNXSYSTM:00/LNXSYBUS:00/ACPI0004:00/VMBUS:00/ebfda100-7464-4629-9da5-12de5470cb4f

    try
    {
        auto symlink = std::filesystem::read_symlink(path);
        const std::string adapterName = symlink.filename();
        if (adapterName.size() > 3 && adapterName.compare(0, 4, "wlan") == 0)
        {
            symlink = symlink.parent_path().parent_path();
        }
        auto device = symlink.parent_path().parent_path();
        std::string deviceGuid = device.filename();
        if (deviceGuid.size() > 6 && deviceGuid.compare(0, 6, "virtio") == 0)
        {
            deviceGuid = device.parent_path().parent_path().parent_path().filename();
        }

        return wsl::shared::string::ToGuid(deviceGuid);
    }
    catch (...)
    {
        return {};
    }
}

Interface GnsEngine::OpenInterfaceOrAdapter(const std::wstring& nameOrId)
{
    if (!nameOrId.empty() && nameOrId[0] == L'{')
    {
        auto id = wsl::shared::string::ToGuid(nameOrId);
        if (!id.has_value())
        {
            THROW_ERRNO(EINVAL);
        }

        return OpenAdapter(id.value());
    }
    else
    {
        return OpenInterface(wsl::shared::string::WideToMultiByte(nameOrId));
    }
}

void GnsEngine::ProcessNotification(const nlohmann::json& payload, Interface& interface)
{
    using namespace std::placeholders;

    if (!payload.contains("ResourceType"))
    {
        throw RuntimeErrorWithSourceLocation("Json is missing ResourceType");
    }

    switch (payload["ResourceType"].get<GuestEndpointResourceType>())
    {
    case GuestEndpointResourceType::Route:
        GNS_LOG_INFO("GuestEndpointResourceType::Route for interfaceName {}", interface.Name().c_str());
        ProcessNotificationImpl(interface, payload, &GnsEngine::ProcessRouteChange);
        break;

    case GuestEndpointResourceType::IPAddress:
        GNS_LOG_INFO("GuestEndpointResourceType::IPAddress for interfaceName {}", interface.Name().c_str());
        ProcessNotificationImpl(interface, payload, &GnsEngine::ProcessIpAddressChange);
        break;

    case GuestEndpointResourceType::MacAddress:
        GNS_LOG_INFO("GuestEndpointResourceType::MacAddress for interfaceName {}", interface.Name().c_str());
        ProcessNotificationImpl(interface, payload, &GnsEngine::ProcessMacAddressChange);
        break;

    case GuestEndpointResourceType::DNS:
        GNS_LOG_INFO("GuestEndpointResourceType::DNS for interfaceName {}", interface.Name().c_str());
        ProcessNotificationImpl(interface, payload, &GnsEngine::ProcessDNSChange);
        break;

    case GuestEndpointResourceType::Interface:
        GNS_LOG_INFO("GuestEndpointResourceType::Interface for interfaceName {}", interface.Name().c_str());
        ProcessNotificationImpl(interface, payload, &GnsEngine::ProcessLinkChange);
        break;

    default:
        throw RuntimeErrorWithSourceLocation(std::format(
            "Unexpected LxGnsMessageNotification for interfaceName {}: {}", interface.Name(), payload["ResourceType"].get<std::string>()));
        break;
    }
}

template <typename T>
void GnsEngine::ProcessNotificationImpl(
    Interface& interface, const nlohmann::json& payload, void (GnsEngine::*routine)(Interface&, const T&, wsl::shared::hns::ModifyRequestType))
{
    T settings{};
    nlohmann::from_json(payload.at("Settings"), settings);
    (this->*routine)(interface, settings, payload["RequestType"].get<wsl::shared::hns::ModifyRequestType>());
}

void GnsEngine::ProcessIpAddressChange(Interface& interface, const wsl::shared::hns::IPAddress& payload, wsl::shared::hns::ModifyRequestType action)
{
    uint16_t addrFamily = UtilWinAfToLinuxAf(payload.Family);
    if (addrFamily != AF_INET && addrFamily != AF_INET6)
    {
        throw RuntimeErrorWithSourceLocation(std::format("Unexpected family: {}", payload.Family));
    }

    Address address{
        addrFamily,
        payload.OnLinkPrefixLength,
        wsl::shared::string::WideToMultiByte(payload.Address),
        static_cast<IpPrefixOrigin>(payload.PrefixOrigin),
        static_cast<IpSuffixOrigin>(payload.SuffixOrigin),
        payload.PreferredLifetime};

    // For addresses plumbed through this path, the corresponding prefix route will be plumbed separately,
    // so do not let Linux autogenerate the prefix route.
    address.SetIsPrefixRouteAutogenerationDisabled(true);

    const auto addressString = utils::Stringify(address);

    if (action == ModifyRequestType::Remove)
    {
        GNS_LOG_INFO("Remove address {} on interfaceName {}", addressString.c_str(), interface.Name().c_str());
        manager.ModifyAddress(interface, address, Operation::Remove);
    }
    else if (action == ModifyRequestType::Add)
    {
        GNS_LOG_INFO("Add address {} on interfaceName {}", addressString.c_str(), interface.Name().c_str());
        manager.ModifyAddress(interface, address, Operation::Create);
    }
    else if (action == ModifyRequestType::Update)
    {
        GNS_LOG_INFO("Update address {} on interfaceName {}", addressString.c_str(), interface.Name().c_str());
        manager.ModifyAddress(interface, address, Operation::Update);
    }
    else
    {
        throw RuntimeErrorWithSourceLocation(std::format("Unexpected ip address action: {}", static_cast<int>(action)));
    }
}

void GnsEngine::ProcessRouteChange(Interface& interface, const wsl::shared::hns::Route& route, wsl::shared::hns::ModifyRequestType action)
{
    int addrFamily = UtilWinAfToLinuxAf(route.Family);
    if (addrFamily != AF_INET && addrFamily != AF_INET6)
    {
        throw RuntimeErrorWithSourceLocation(std::format("Unexpected family: {}", route.Family));
    }

    if (action == ModifyRequestType::Reset)
    {
        GNS_LOG_INFO("Reset routes on interfaceName {}", interface.Name().c_str());
        manager.ResetRoutingTable(addrFamily, interface);
        return;
    }

    bool defaultRoute = (addrFamily == AF_INET && route.DestinationPrefix == LX_INIT_DEFAULT_ROUTE_PREFIX) ||
                        (addrFamily == AF_INET6 && route.DestinationPrefix == LX_INIT_DEFAULT_ROUTE_V6_PREFIX);
    std::optional<Address> to;
    if (!defaultRoute)
    {
        to = Address::FromPrefixString(addrFamily, wsl::shared::string::WideToMultiByte(route.DestinationPrefix));
    }

    // Note: for the next hop parameter to the Route constructor, the prefix length can be any valid prefix length -
    // it's just used to create an address object.  We currently use the SitePrefixLength field for convenience.
    const auto nextHopValue = wsl::shared::string::WideToMultiByte(route.NextHop);
    auto interfaceRoute =
        Route{addrFamily, {{addrFamily, route.SitePrefixLength, nextHopValue}}, interface.Index(), defaultRoute, to, route.Metric};

    auto routeString = utils::Stringify(interfaceRoute);

    if (action == ModifyRequestType::Add)
    {
        GNS_LOG_INFO("Add route {} on interfaceName {}", routeString.c_str(), interface.Name().c_str());
        manager.ModifyRoute(interfaceRoute, Operation::Create);
    }
    else if (action == ModifyRequestType::Remove)
    {
        GNS_LOG_INFO("Remove route {} on interfaceName {}", routeString.c_str(), interface.Name().c_str());
        manager.ModifyRoute(interfaceRoute, Operation::Remove);
    }
    else if (action == ModifyRequestType::Update)
    {
        GNS_LOG_INFO("Update route {} on interfaceName {}", routeString.c_str(), interface.Name().c_str());
        manager.ModifyRoute(interfaceRoute, Operation::Update);
    }
    else
    {
        throw RuntimeErrorWithSourceLocation(std::format("Unexpected route action: {}", static_cast<int>(action)));
    }
}

void GnsEngine::ProcessDNSChange(Interface& interface, const wsl::shared::hns::DNS& payload, wsl::shared::hns::ModifyRequestType action)
{
    if (action == ModifyRequestType::Remove)
    {
        GNS_LOG_INFO("Ignoring Remove on interfaceName {}", interface.Name().c_str());
        return; // Will be overwritten when the next 'add' / 'update' comes
    }

    if (action != ModifyRequestType::Update && action != ModifyRequestType::Add)
    {
        throw RuntimeErrorWithSourceLocation(std::format("Unexpected DNS Change action: {}", static_cast<int>(action)));
    }

    std::wstringstream content;
    if (!payload.Options.empty())
    {
        content << payload.Options; // The Options field is used to pass the file header
    }

    for (const auto& server : wsl::shared::string::Split(payload.ServerList, L','))
    {
        content << L"nameserver " << server << L"\n";
    }

    if (!payload.Domain.empty())
    {
        content << L"domain " << payload.Domain << L"\n";
    }

    if (!payload.Search.empty())
    {
        content << L"search " << wsl::shared::string::Join(wsl::shared::string::Split(payload.Search, L','), L' ') << L"\n";
    }

    GNS_LOG_INFO(
        "Setting DNS server domain to {}: {} on interfaceName {} ",
        payload.Domain.c_str(),
        content.str().c_str(),
        interface.Name().c_str());

    std::wofstream resolvConf;
    resolvConf.exceptions(std::ofstream::badbit | std::ofstream::failbit);
    resolvConf.open("/etc/resolv.conf", std::ofstream::trunc);
    resolvConf << content.str();
}

void GnsEngine::ProcessMacAddressChange(Interface& interface, const wsl::shared::hns::MacAddress& address, wsl::shared::hns::ModifyRequestType type)
{
    GNS_LOG_INFO(
        "Setting to MAC address to {} (will toggle the interface state) on interfaceName {} ",
        address.PhysicalAddress.c_str(),
        interface.Name().c_str());
    manager.SetAdapterMacAddress(interface, wsl::shared::string::ParseMacAddress(address.PhysicalAddress, '-'));
}

void GnsEngine::ProcessLinkChange(Interface& interface, const wsl::shared::hns::NetworkInterface& link, wsl::shared::hns::ModifyRequestType type)
{
    GNS_LOG_INFO(
        "Setting link state to {} on interfaceName {}",
        link.Connected ? "InterfaceState::Up" : "InterfaceState::Down",
        interface.Name().c_str());
    manager.SetInterfaceState(interface, link.Connected ? NetworkManager::InterfaceState::Up : NetworkManager::InterfaceState::Down);

    if (link.Connected && link.NlMtu != 0)
    {
        GNS_LOG_INFO("Setting MTU to {} on interfaceName {} ", link.NlMtu, interface.Name().c_str());
        interface.SetMtu(link.NlMtu);
    }

    if (link.Connected && link.Metric != 0)
    {
        GNS_LOG_INFO("Setting Metric to {} on interfaceName {} ", link.Metric, interface.Name().c_str());
        interface.SetMetric(link.Metric);
    }
}

std::tuple<bool, int> GnsEngine::ProcessNextMessage()
{
    int return_value = 0;

    auto payload = notificationRoutine();
    if (!payload.has_value())
    {
        GNS_LOG_ERROR("Received empty message, exiting");
        return std::make_tuple(false, -1);
    }

    switch (payload->MessageType)
    {
    case LxGnsMessageNoOp:
    {
        break;
    }
    case LxGnsMessageNotification:
    {
        auto interface = OpenAdapter(payload->AdapterId.value());

        ProcessNotification(nlohmann::json::parse(payload->Json), interface);
        break;
    }
    case LxGnsMessageInterfaceConfiguration:
    {
        const auto endpoint = wsl::shared::FromJson<wsl::shared::hns::HNSEndpoint>(payload->Json.c_str());
        const auto endpointString = wsl::shared::string::GuidToString<char>(endpoint.ID);
        auto interface = OpenAdapter(endpoint.ID);

        // Give the interface a new name if requested.
        if (endpoint.PortFriendlyName.size() > 0)
        {
            auto assignedName = wsl::shared::string::WideToMultiByte(endpoint.PortFriendlyName);
            if (assignedName.compare(interface.Name()) != 0)
            {
                // Special case for wlanxx adapters: create a virtual wifi interface.
                if (assignedName.size() > 3 && assignedName.compare(0, 4, "wlan") == 0)
                {
                    auto backingName = std::string("_") + assignedName;
                    GNS_LOG_INFO(
                        "LxGnsMessageInterfaceConfiguration: endpointID ({}) setting interfaceName to {}",
                        endpointString.c_str(),
                        backingName.c_str());
                    manager.SetAdapterName(interface, backingName);

                    GNS_LOG_INFO(
                        "LxGnsMessageInterfaceConfiguration: endpointID ({}) creating virtual Wi-Fi named {}",
                        endpointString.c_str(),
                        assignedName.c_str());
                    interface = manager.CreateVirtualWifiAdapter(interface, assignedName);

                    auto backingInterface = Interface::Open(backingName);
                    GNS_LOG_INFO(
                        "LxGnsMessageInterfaceConfiguration: endpointID ({}) setting interface ({}) state up on the newly "
                        "created interfaceName {}",
                        endpointString.c_str(),
                        backingName.c_str(),
                        backingInterface.Name().c_str());
                    manager.SetInterfaceState(backingInterface, NetworkManager::InterfaceState::Up);
                }
                else
                {
                    GNS_LOG_INFO(
                        "LxGnsMessageInterfaceConfiguration: endpointID ({}) setting interfaceName from {} to {}",
                        endpointString.c_str(),
                        interface.Name().c_str(),
                        assignedName.c_str());
                    manager.SetAdapterName(interface, assignedName);
                    interface = Interface::Open(assignedName);
                }
            }
            else
            {
                GNS_LOG_INFO(
                    "LxGnsMessageInterfaceConfiguration: no-op - the endpoint ID {} PortFriendlyName ({}) is already matching "
                    "the interfaceName {}",
                    endpointString.c_str(),
                    assignedName.c_str(),
                    interface.Name().c_str());
            }
        }
        else
        {
            GNS_LOG_INFO(
                "LxGnsMessageInterfaceConfiguration: no-op - the endpoint ID {} PortFriendlyName is blank", endpointString.c_str());
        }

        // The IP address can be empty if flow steering is enabled (we'll get it from a notification)
        if (!endpoint.IPAddress.empty())
        {
            manager.SetAdapterConfiguration(interface, endpoint);
        }

        manager.SetInterfaceState(interface, NetworkManager::InterfaceState::Up);
        break;
    }
    case LxGnsMessageVmNicCreatedNotification:
    {
        auto vmNic = wsl::shared::FromJson<wsl::shared::hns::VmNicCreatedNotification>(payload->Json.c_str());
        auto interface = OpenAdapter(vmNic.adapterId);

        GNS_LOG_INFO(
            "LxGnsMessageVmNicCreatedNotification: EnableLoopbackRouting on adapterId {}, interfaceName {}",
            wsl::shared::string::GuidToString<char>(vmNic.adapterId).c_str(),
            interface.Name().c_str());
        manager.EnableLoopbackRouting(interface);
        break;
    }
    case LxGnsMessageCreateDeviceRequest:
    {
        auto createDeviceRequest = wsl::shared::FromJson<wsl::shared::hns::CreateDeviceRequest>(payload->Json.c_str());
        switch (createDeviceRequest.type)
        {
        case wsl::shared::hns::DeviceType::Loopback:
        {
            const GUID emptyGuid{};
            assert(createDeviceRequest.lowerEdgeAdapterId.has_value());
            auto gelnic = OpenAdapter(createDeviceRequest.lowerEdgeAdapterId.value());
            GNS_LOG_INFO(
                "LxGnsMessageCreateDeviceRequest [Loopback]: InitializeLoopbackConfiguration deviceName {}, interfaceName {}",
                wsl::shared::string::GuidToString<char>(createDeviceRequest.lowerEdgeAdapterId.value_or(emptyGuid)).c_str(),
                gelnic.Name().c_str());
            manager.InitializeLoopbackConfiguration(gelnic);
            break;
        }
        default:
            throw RuntimeErrorWithSourceLocation(
                std::format("Unexpected Wslcore::Networking::DeviceType : {}", static_cast<int>(createDeviceRequest.type)));
            break;
        }
        break;
    }
    case LxGnsMessageModifyGuestDeviceSettingRequest:
    {
        auto modifyRequest = wsl::shared::FromJson<wsl::shared::hns::ModifyGuestEndpointSettingRequest<wsl::shared::hns::NetworkInterface>>(
            payload->Json.c_str());
        if (modifyRequest.ResourceType != GuestEndpointResourceType::Interface)
        {
            GNS_LOG_INFO(
                "ModifyGuestEndpointSettingRequest - ignoring request that's not for type Interface (type {}) device "
                "{}",
                static_cast<uint32_t>(modifyRequest.ResourceType),
                modifyRequest.targetDeviceName.value_or(L"<empty>").c_str());
            break;
        }

        if (!modifyRequest.targetDeviceName.has_value())
        {
            GNS_LOG_INFO("ModifyGuestEndpointSettingRequest targetDeviceName is empty");
            break;
        }

        auto interface = OpenInterfaceOrAdapter(modifyRequest.targetDeviceName.value());
        GNS_LOG_INFO(
            "ModifyGuestEndpointSettingRequest [Interface]: setting link state for deviceName {} interfaceName {}",
            modifyRequest.targetDeviceName->c_str(),
            interface.Name().c_str());

        ProcessLinkChange(interface, modifyRequest.Settings, modifyRequest.RequestType);
        break;
    }
    case LxGnsMessageLoopbackRoutesRequest:
    {
        auto request = wsl::shared::FromJson<wsl::shared::hns::LoopbackRoutesRequest>(payload->Json.c_str());
        if (request.operation != wsl::shared::hns::OperationType::Create && request.operation != wsl::shared::hns::OperationType::Remove)
        {
            GNS_LOG_INFO(
                "LxGnsMessageLoopbackRoutesRequest - ignoring request that has the wrong operation type {} for interface "
                "{}",
                static_cast<int>(request.operation),
                request.targetDeviceName.c_str());
            break;
        }

        int addrFamily = UtilWinAfToLinuxAf(request.family);
        if (addrFamily != AF_INET && addrFamily != AF_INET6)
        {
            throw RuntimeErrorWithSourceLocation(std::format("LxGnsMessageLoopbackRoutesRequest: unexpected family: {}", request.family));
        }

        assert(request.operation == wsl::shared::hns::OperationType::Create || request.operation == wsl::shared::hns::OperationType::Remove);
        auto operation = (request.operation == wsl::shared::hns::OperationType::Create) ? Operation::Create : Operation::Remove;
        auto interface = OpenInterfaceOrAdapter(request.targetDeviceName);
        auto ipAddress = wsl::shared::string::WideToMultiByte(request.ipAddress);
        int prefixLen = MAX_PREFIX_LEN(addrFamily);
        Address address(addrFamily, prefixLen, ipAddress);
        manager.UpdateLoopbackRoute(interface, address, operation);
        break;
    }
    case LxGnsMessageDeviceSettingRequest:
    {
        auto json = nlohmann::json::parse(payload->Json);
        auto interface = OpenInterfaceOrAdapter(json.at("targetDeviceName").get<std::wstring>());
        ProcessNotification(json, interface);
        break;
    }
    case LxGnsMessageInitialIpConfigurationNotification:
    {
        auto notification = wsl::shared::FromJson<wsl::shared::hns::InitialIpConfigurationNotification>(payload->Json.c_str());
        auto interface = OpenInterfaceOrAdapter(notification.targetDeviceName);

        if (WI_IsFlagClear(notification.flags, wsl::shared::hns::InitialIpConfigurationNotificationFlags::SkipPrimaryRoutingTableUpdate))
        {
            auto table = manager.FindRoutingTableIdForInterface(interface);
            if (!table.has_value())
            {
                throw RuntimeErrorWithSourceLocation(std::format(
                    "LxGnsMessageInitialIpConfigurationNotification: failed to find routing table with name {}", interface.Name()));
            }

            GNS_LOG_INFO(
                "LxGnsMessageInitialIpConfigurationNotification: Changing primary routing table to {} with id {}",
                interface.Name().c_str(),
                table.value());
            manager.ChangePrimaryRoutingTable(table.value());
        }

        GNS_LOG_INFO("LxGnsMessageInitialIpConfigurationNotification: Resetting IPv6 state for interface {}", interface.Name().c_str());
        interface.ResetIpv6State();

        if (WI_IsFlagClear(notification.flags, wsl::shared::hns::InitialIpConfigurationNotificationFlags::SkipLoopbackRouteReset))
        {
            GNS_LOG_INFO("LxGnsMessageInitialIpConfigurationNotification: Wiping loopback routes");
            manager.ResetLoopbackRoutes();
        }

        // EnableIpv4ArpFilter does not need to be called per interface as each interface gets mirrored
        // It could be global if we had a single global Init message
        // If there are more global init requirements in the future, we should consider a new global message
        GNS_LOG_INFO("LxGnsMessageInitialIpConfigurationNotification: Enabling IPv4 arp_filter");
        manager.EnableIpv4ArpFilter();
        break;
    }
    case LxGnsMessageSetupIpv6:
    {
        manager.DisableDAD();
        manager.DisableRouterDiscovery();
        manager.DisableIpv6AddressGeneration();
        break;
    }
    case LxGnsMessageConnectTestRequest:
    {
        // the payload is where to send the request, not in a JSON format
        wsl::shared::conncheck::ConnCheckResult result = manager.SendConnectRequest(payload->Json.c_str());
        // convert the 2 enums into a single integer value
        // Ipv4 status will be the lower 16 bits
        return_value = static_cast<uint32_t>(result.Ipv4Status);
        // Ipv6 status will be the upper 16 bits
        return_value |= (static_cast<uint32_t>(result.Ipv6Status) << 16);
        GNS_LOG_INFO("LxGnsMessageConnectTestRequest (destination: {}) returning: {:#x}", payload->Json.c_str(), return_value);
        break;
    }
    case LxGnsMessageGlobalNetFilter:
    {
        // the global network filters exist to 'mark' traffic that is originating from root
        // vs. traffic that is originating from another Linux container (namespace)
        // it also adds a NAT to the chain for the traffic that is not marked
        // see the 'nft add rule' command in the below LxGnsMessageInterfaceNetFilter section
        auto runCommand = [](const std::string& command) { THROW_LAST_ERROR_IF(UtilExecCommandLine(command.c_str()) < 0); };
        for (const auto& ip : c_ipStrings)
        {
            runCommand(std::format("nft add table {} filter", ip));
            runCommand(std::format("nft \"add chain {} filter WSLOUTPUT {{ type filter hook output priority filter; }}\"", ip));
            runCommand(std::format("nft add rule {} filter WSLOUTPUT counter mark set 0x1", ip));
            runCommand(std::format("nft add table {} nat", ip));
            runCommand(std::format("nft \"add chain {} nat WSLPOSTROUTING {{ type nat hook postrouting priority srcnat - 1; }}\"", ip));
        }

        break;
    }
    case LxGnsMessageInterfaceNetFilter:
    {
        auto interfaceNetFilterRequest = wsl::shared::FromJson<wsl::shared::hns::InterfaceNetFilterRequest>(payload->Json.c_str());
        auto interface = OpenInterfaceOrAdapter(interfaceNetFilterRequest.targetDeviceName);

        GNS_LOG_INFO(
            "LxGnsMessageInterfaceNetFilter for interface {} {{operation={}, startPort={}, endPort={}}}",
            interface.Name().c_str(),
            static_cast<int>(interfaceNetFilterRequest.operation),
            interfaceNetFilterRequest.ephemeralPortRangeStart,
            interfaceNetFilterRequest.ephemeralPortRangeEnd);

        switch (interfaceNetFilterRequest.operation)
        {
        case wsl::shared::hns::OperationType::Create:
        {
            // Create SNAT rules on the interface.
            for (const auto& ip : c_ipStrings)
            {
                for (const auto& protocol : {"udp", "tcp"})
                {
                    const auto commandLine = std::format(
                        "nft add rule {} nat WSLPOSTROUTING oif {} {} sport 1-65535 mark != 0x1 counter masquerade to :{}-{}",
                        ip,
                        interface.Name().c_str(),
                        protocol,
                        interfaceNetFilterRequest.ephemeralPortRangeStart,
                        interfaceNetFilterRequest.ephemeralPortRangeEnd);

                    GNS_LOG_INFO("LxGnsMessageInterfaceNetFilter (Create): {}", commandLine.c_str());
                    THROW_LAST_ERROR_IF(UtilExecCommandLine(commandLine.c_str()) < 0);
                }
            }

            manager.UpdateMirroredLoopbackRulesForInterface(interface.Name(), Operation::Create);
            break;
        }
        case wsl::shared::hns::OperationType::Remove:
        {
            // Remove SNAT rules on the interface (one in ipv4 and one in ipv6).
            // Rules can only be removed via handle number, so find the handle numbers first.
            for (const auto& ip : c_ipStrings)
            {
                const auto listChainCommand = std::format("nft -a list chain {} nat WSLPOSTROUTING", ip);
                std::string listOutputString;
                THROW_LAST_ERROR_IF(UtilExecCommandLine(listChainCommand.c_str(), &listOutputString) < 0);

                std::regex pattern("oif\\s+\"" + interface.Name() + "\"\\s+.*handle\\s+(\\d+)");
                std::smatch matches;
                std::vector<int> handleNumbers;
                auto iter = listOutputString.cbegin();
                while (std::regex_search(iter, listOutputString.cend(), matches, pattern))
                {
                    handleNumbers.push_back(std::stoi(matches.str(1)));
                    iter = matches.suffix().first;
                }

                for (const auto& handle : handleNumbers)
                {
                    auto commandLine = std::format("nft delete rule {} nat WSLPOSTROUTING handle {}", ip, handle);
                    GNS_LOG_INFO("LxGnsMessageInterfaceNetFilter (Remove): {}", commandLine.c_str());
                    THROW_LAST_ERROR_IF(UtilExecCommandLine(commandLine.c_str()) < 0);
                }
            }

            manager.UpdateMirroredLoopbackRulesForInterface(interface.Name(), Operation::Remove);
            break;
        }
        default:
            throw RuntimeErrorWithSourceLocation(std::format(
                "Unexpected Wslcore::Networking::OperationType : {}", static_cast<int>(interfaceNetFilterRequest.operation)));
            break;
        }
        break;
    }

    default:
        throw RuntimeErrorWithSourceLocation(std::format("Unexpected LX_MESSAGE_TYPE : {}", static_cast<int>(payload->MessageType)));
    }

    return std::make_tuple(true, return_value);
}

void GnsEngine::run()
{
    UtilSetThreadName("GnsEngine");

    while (true)
    {
        try
        {
            GNS_LOG_INFO("Processing Next Message");
            auto [should_continue, return_value] = ProcessNextMessage();
            if (!should_continue)
            {
                break;
            }

            GNS_LOG_INFO("Processing Next Message Successful ({:#x})", return_value);
            statusRoutine(return_value, "");
        }
        catch (const std::exception& e)
        {
            GNS_LOG_ERROR("Error while processing message: {}", e.what());
            statusRoutine(-1, e.what());
        }
    }

    // ensure our exit path is in the error stream
    GNS_LOG_ERROR("exiting");
}
