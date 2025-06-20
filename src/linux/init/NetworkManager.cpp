// Copyright (C) Microsoft Corporation. All rights reserved.

#include <iostream>
#include <filesystem>
#include <lxwil.h>
#include "lxinitshared.h"
#include "common.h"
#include "NetworkManager.h"
#include "util.h"
#include "address.h"
#include "conncheckshared.h"
#include "RuntimeErrorWithSourceLocation.h"
#include "stringshared.h"

// Custom table used for storing routes for loopback IPs.
constexpr int c_loopbackRoutingTableId = 127;
// Custom table used for storing routes for local IPs. A separate table is used for local IPs so that
// when the IP addresses are changing, all routes from the table can be deleted before adding routes for the new set of IPs.
// The routes for the loopback IPs will always be the same, thus keeping them in a separate table
constexpr int c_localRoutingTableId = 128;

// See comments in AddMirroredLoopbackRoutingRules for explanation about those priorities.
constexpr int c_WindowsToLinuxRulePriority = 0;
constexpr int c_LinuxToWindowsRulePriority = 1;
constexpr int c_LocalRulePriority = 2;

// Creates routing tables per interface, identified by the interface index plus an offset.
constexpr int c_routeTableOffsetFromIndex = 1000;

// All loopback/local packets will be sent out of the guest via those gateways. There
// will be static ARP entries matching those gateways to the MAC address below, such that
// every packet will have that as destination MAC.
//
// Note: Eventually a mechanism will be needed to replace the gateway addresses in case they
// conflict with other addresses in the network.
const Address c_ipv4LoopbackGateway = {AF_INET, Ipv4MaxPrefixLen, LX_INIT_IPV4_LOOPBACK_GATEWAY_ADDRESS};
const Address c_ipv6LoopbackGateway = {AF_INET6, Ipv6MaxPrefixLen, LX_INIT_IPV6_LOOPBACK_GATEWAY_ADDRESS};

// v4 and v6 loopback address range used in Mirrored mode: 127.0.0.1/32 and ::1/128.
//
// Note: Although the v4 loopback address range is 127.0.0.0/8, only traffic to 127.0.0.1 can be used to communicate host<->guest
// in Mirrored mode. Traffic to other v4 loopback addresses will stay in the guest. This can be changed if other loopback
// addresses are needed by host<->guest loopback scenarios.
const Address c_loopbackV4AddressRange = {AF_INET, Ipv4MaxPrefixLen, "127.0.0.1"};
const Address c_loopbackV6AddressRange = {AF_INET6, Ipv6MaxPrefixLen, "::1"};

// 00:11:22:33:44:55 represents the MAC address that all loopback/local packets will have as destination
// MAC when they are sent out of the guest. This will help Windows to identify which
// packets are loopback/local.
const MacAddress c_gatewayMacAddress = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};

constexpr const char* c_acceptLocalSetting = "accept_local";
constexpr const char* c_routeLocalnetSetting = "route_localnet";
constexpr char c_disableSetting[] = "0\n";
constexpr char c_enableSetting[] = "1\n";

NetworkManager::NetworkManager(RoutingTable& routingTable) :
    routingTable(routingTable), loopbackRoutingTable(c_loopbackRoutingTableId), localRoutingTable(c_localRoutingTableId)
{
}

std::optional<int> NetworkManager::FindRoutingTableIdForInterface(const Interface& interface) const
{
    return c_routeTableOffsetFromIndex + interface.Index();
}

void NetworkManager::ChangePrimaryRoutingTable(int newTableId)
{
    routingTable.ChangeTableId(newTableId);
}

std::vector<Route> NetworkManager::ListRoutes(int family) const
{
    return routingTable.ListRoutes(family);
}

Interface NetworkManager::CreateVirtualWifiAdapter(Interface& baseAdapter, const std::string& wifiName)
{
    GNS_LOG_INFO("Creating virtual wifi adapter with name {}", wifiName.c_str());
    baseAdapter.CreateVirtualWifiAdapter(wifiName);
    auto virtualWifi = Interface::Open(wifiName);

    GNS_LOG_INFO("Enabling Ipv4 loopback routing on virtual wifi adapter with name {}", wifiName.c_str());
    EnableLoopbackRouting(virtualWifi);
    return virtualWifi;
}

Interface NetworkManager::CreateProxyWifiAdapter(Interface& baseAdapter, const std::string& wifiName)
{
    baseAdapter.CreateProxyWifiAdapter(wifiName);
    auto proxyWifi = Interface::Open(wifiName);

    GNS_LOG_INFO("Enabling Ipv4 loopback routing on proxy wifi adapter with name {}", wifiName.c_str());
    EnableLoopbackRouting(proxyWifi);
    return proxyWifi;
}

// SUPPORTS IPV4 ONLY, and only supports 1 IP address per adapter.
// Not used for mirroring.
void NetworkManager::SetAdapterConfiguration(Interface& interface, const wsl::shared::hns::HNSEndpoint& configuration)
{
    InterfaceConfiguration config;
    config.Addresses.emplace_back(Address{AF_INET, configuration.PrefixLength, wsl::shared::string::WideToMultiByte(configuration.IPAddress)});

    config.LocalAddresses = config.Addresses;
    config.BroadcastAddress = utils::ComputeBroadcastAddress(config.Addresses[0]);

    std::stringstream addressInfo;
    addressInfo << config.Addresses[0];
    GNS_LOG_INFO(
        "Setting the IPv4 address on endpointID ({}) to {} on interfaceName {}",
        wsl::shared::string::GuidToString<char>(configuration.ID).c_str(),
        addressInfo.str().c_str(),
        interface.Name().c_str());
    interface.SetIpv4Configuration(config);
}

void NetworkManager::SetInterfaceState(Interface& adapter, InterfaceState state)
{
    GNS_LOG_INFO("Setting interface state to {} on interfaceName {}", state == InterfaceState::Up ? "Up" : "Down", adapter.Name().c_str());
    if (state == InterfaceState::Up)
    {
        adapter.SetUp();
    }
    else
    {
        adapter.SetDown();
    }
}

void NetworkManager::SetAdapterName(Interface& adapter, const std::string& name)
{
    adapter.SetName(name);
}

void NetworkManager::SetAdapterNamespace(Interface& adapter, int namespaceFd)
{
    adapter.SetNamespace(namespaceFd);
}

void NetworkManager::SetWiphyNamespace(Interface& adapter, int namespaceFd)
{
    adapter.SetWiphyNamespace(namespaceFd);
}

void NetworkManager::ModifyRoute(const Route& route, Operation operation)
{
    routingTable.ModifyRoute(route, operation);
}

void NetworkManager::ResetRoutingTable(int addressFamily, const Interface& interface)
{
    auto routes = routingTable.ListRoutes(addressFamily);
    auto pred = [&](const Route& route) {
        // Routes without gateways are link level routes (scope link)
        return !route.via.has_value() || route.dev != interface.Index();
    };

    std::erase_if(routes, pred);

    for (const auto& e : routes)
    {
        const auto routeString = utils::Stringify(e);
        try
        {
            GNS_LOG_INFO("Removing route {} from interfaceName {}", routeString, interface.Name());
            routingTable.ModifyRoute(e, Operation::Remove);
        }
        catch (const std::exception& ex)
        {
            throw RuntimeErrorWithSourceLocation(std::format("Failed to remove route '{}', {}", routeString, ex.what()));
        }
    }
}

void NetworkManager::ModifyAddress(Interface& adapter, const Address& address, Operation operation)
{
    std::vector<Route> routes;

    // If the ip address is changing, the routing table needs to be saved & restored
    // because netlink doesn't allow ip addresses to be changed, but only deleted and added,
    // which causes the routing rules attached to the interface to be dropped
    if (operation == Operation::Update)
    {
        routes = routingTable.ListRoutes(address.Family());
    }

    adapter.ModifyIpAddress(address, operation);

    // Restore the routes for this interface.
    // Note: If a route fails to be restored, it's probably because the new address's subnet is different,
    // and so the route would have been unusable with the new address anyway
    for (const auto& savedRoute : routes)
    {
        if (savedRoute.dev == adapter.Index() && savedRoute.via.has_value())
        {
            const auto savedRouteString = utils::Stringify(savedRoute);

            try
            {
                GNS_LOG_INFO(
                    "Restoring route {} after address change, on interfaceName {}", savedRouteString.c_str(), adapter.Name().c_str());
                routingTable.ModifyRoute(savedRoute, Operation::Create);
            }
            catch (const std::exception& ex)
            {
                GNS_LOG_ERROR(
                    "Failed to restore route {} after address change, on interfaceName {}, caught exception "
                    "{}",
                    savedRouteString.c_str(),
                    adapter.Name().c_str(),
                    ex.what());
            }
        }
    }
}

void NetworkManager::SetAdapterMacAddress(Interface& interface, const MacAddress& address)
{
    SetInterfaceState(interface, InterfaceState::Down);
    interface.SetMacAddress(address);
    SetInterfaceState(interface, InterfaceState::Up);
}

void NetworkManager::DisassociateAdapterFromBond(const std::string& bondInterfaceName, Interface& interface)
{
    auto bondInterface = Interface::Open(bondInterfaceName);
    GNS_LOG_INFO(
        "Trying to disassociate from bond - bondDeviceName {}, interfaceName {}", bondInterfaceName.c_str(), interface.Name().c_str());
    bondInterface.RemoveFromBond(interface);
    GNS_LOG_INFO(
        "Successfully disassociated from bond - bondDeviceName {}, interfaceName {}", bondInterfaceName.c_str(), interface.Name().c_str());
}

void NetworkManager::AssociateAdapterWithBond(const std::string& bondInterfaceName, Interface& interface)
{
    auto bondInterface = Interface::Open(bondInterfaceName);
    // must set the interface down before associating it to bond
    SetInterfaceState(interface, InterfaceState::Down);
    bondInterface.AddToBond(interface);
    GNS_LOG_INFO(
        "Successfully associated to bond - bondDeviceName {}, interfaceName {}", bondInterfaceName.c_str(), interface.Name().c_str());
    SetInterfaceState(interface, InterfaceState::Up);
}

void NetworkManager::ActivateAdapterWithBond(const std::string& bondInterfaceName, const Interface& interface)
{
    auto bondInterface = Interface::Open(bondInterfaceName);
    bondInterface.SetActiveChild(interface);
}

Interface NetworkManager::CreateBondAdapter(const std::string& name)
{
    Interface::CreateBondAdapter(name);
    auto bondInterface = Interface::Open(name);

    // Enable routing of IPv4 loopback on the bond interface.
    GNS_LOG_INFO("Enabling IPv4 loopback routing on bond adapter with name {}", name.c_str());
    EnableLoopbackRouting(bondInterface);
    return bondInterface;
}

/*
    Enable the accept_local and route_localnet settings required to send/receive loopback and local
    packets on an interface.

    Note: The function supports only IPv4 settings at the moment. There are no equivalent IPv6 settings
    for accept_local and route_localnet.
*/
void NetworkManager::EnableLoopbackRouting(Interface& interface)
{
    GNS_LOG_INFO("Enabling sysctl accept_local setting on adapter with name {}", interface.Name().c_str());
    interface.EnableNetworkSetting(c_acceptLocalSetting, AF_INET);

    GNS_LOG_INFO("Enabling sysctl route_localnet setting on adapter with name {}", interface.Name().c_str());
    interface.EnableNetworkSetting(c_routeLocalnetSetting, AF_INET);
}

/*
    Note: GELNIC stands for Guest-Exclusive Loopback NIC. It represents the mirrored interface of the host
    loopback interface. Every packet that arrives in the guest having a loopback destination address will
    arrive on the GELNIC.
*/
void NetworkManager::InitializeLoopbackConfiguration(Interface& gelnic)
{
    // Enable routing of IPv4 loopback on the GELNIC.
    GNS_LOG_INFO("Enabling IPv4 loopback routing on GELNIC adapter {}", gelnic.Name().c_str());
    EnableLoopbackRouting(gelnic);

    // Disable IPv4 reverse path filtering on the GELNIC.
    // The effective rp_filter setting for interface "name" is the more restrictive between "name" and "all", so both must be set.
    GNS_LOG_INFO("Disabling sysctl rp_filter setting on loopback adapter");
    gelnic.DisableNetworkSetting("rp_filter", AF_INET);
    ModifyNetSetting(AF_INET, "rp_filter", "all", c_disableSetting, strlen(c_disableSetting));

    InitializeLoopbackConfigurationImpl(gelnic, AF_INET);
    // InitializeLoopbackConfigurationImpl(gelnic, AF_INET6);
}

/*
    In mirrored networking mode, Linux ip rules (policy based routing) are configured such that
    loopback traffic or local traffic (local = traffic with destination an IP assigned to Linux/Windows)
    can flow between Windows and Linux. Note: This applies only to TCP and UDP traffic.

    Below are outputs of "ip rule show" in both NAT and mirrored mode, along with explanations about how
    the rules work.

    The leftmost part of each rule represents the priority of the rule (priority 0 is the highest priority)

    The "lookup" keyword is followed by the name or id of a routing table
    The local table is used by Linux to know when to deliver a packet locally (to a Linux process). This table is used
    for both traffic with destination 127.0.0.1 and traffic with destination an IP assigned to Linux.
    Table 127 contains routes used to send traffic with destination 127.0.0.1 out of Linux, to be processed by Windows.
    Traffic will be sent out of Linux via the "GELNIC" interface (which will be called loopback0).
    Table 128 contains routes used to send traffic with local destination out of Linux, to be processed by Windows.
    Traffic will be sent out of Linux via the mirrored interface that has the destination IP assigned to it.
    Tables main and default contain routes that are not related to loopback traffic

    Priority 0 rules will deliver to Linux loopback/local traffic coming from Windows (this can be traffic
    with origin in Windows or traffic with origin in Linux that was sent to Windows and Windows sent it back to Linux).
    "iif" represents input interface (the interface on which Linux received the traffic).
    "loopback0" is the interface used to send 127.0.0.1 traffic between Linux and Windows. "eth0" refers to an
    interface that was mirrored in Linux. Each time an interface is mirrored, a rule like this needs to be added
    for that interface. And each time the interface is deleted, the rule needs to be deleted.

    Priority 1 rules are used to send traffic that originates in Linux out of Linux to Windows, so that Windows can decide
    if that traffic must be sent to Windows or back to Linux. Those rules apply to traffic originating in the Linux
    root network namespace, but also to traffic that root network namespace receives from other network namespaces, such
    as Docker containers.

    The priority 2 rule is needed for loopback/local traffic that is not TCP or UDP, such as ICMP. This traffic will
    stay inside Linux, it cannot be sent between Linux and Windows. The rule is also needed for receiving inbound traffic
    from an external machine.

    NAT mode ip rule show:
    0:      from all lookup local
    32766:  from all lookup main
    32767:  from all lookup default

    mirrored mode ip rule show:
    0:      from all iif loopback0 ipproto tcp lookup local
    0:      from all iif loopback0 ipproto udp lookup local
    0:      from all iif eth0 ipproto tcp lookup local
    0:      from all iif eth0 ipproto udp lookup local
    1:      from all ipproto tcp lookup 127
    1:      from all ipproto udp lookup 127
    1:      from all ipproto tcp lookup 128
    1:      from all ipproto udp lookup 128
    2:      from all lookup local
    32766:  from all lookup main
    32767:  from all lookup default
*/
void NetworkManager::AddMirroredLoopbackRoutingRules(Interface& gelnic, int addressFamily)
{
    GNS_LOG_INFO("gelnic name {}, addressFamily {}", gelnic.Name().c_str(), addressFamily);

    // Delete rule with priority 0 for local table (from all prio 0 lookup local).
    Rule rule = Rule(addressFamily, RT_TABLE_LOCAL, 0);
    ruleManager.ModifyRoutingTablePriority(rule, Operation::Remove);

    // Adding priority 0 rules for the GELNIC interface
    // Similar priority 0 rules will also be added or deleted when an interface is mirrored in Linux, or deleted
    UpdateMirroredLoopbackRulesForInterface(gelnic.Name(), Operation::Create);

    auto AddPriority1Rule = [&](const Protocol protocol, const int routingTableId) {
        Rule rule = Rule(addressFamily, routingTableId, c_LinuxToWindowsRulePriority, protocol);
        ruleManager.ModifyRoutingTablePriorityWithProtocol(rule, Operation::Create);
    };

    // Adding priority 1 rules
    AddPriority1Rule(Protocol::Tcp, c_loopbackRoutingTableId);
    AddPriority1Rule(Protocol::Udp, c_loopbackRoutingTableId);
    AddPriority1Rule(Protocol::Tcp, c_localRoutingTableId);
    AddPriority1Rule(Protocol::Udp, c_localRoutingTableId);

    // Add a rule referencing the local table, with priority 2
    rule = Rule(addressFamily, RT_TABLE_LOCAL, c_LocalRulePriority);
    ruleManager.ModifyRoutingTablePriority(rule, Operation::Create);
}

void NetworkManager::UpdateMirroredLoopbackRulesForInterface(const std::string& interfaceName, Operation operation)
{
    assert(operation == Operation::Create || operation == Operation::Remove);

    // Add or remove priority 0 rules needed by mirrored loopback traffic. See the comments in AddMirroredLoopbackRoutingRules for
    // more details. Currently only IPv4 guest<->host loopback is supported in mirrored mode - adding only IPv4 rules.
    GNS_LOG_INFO(
        "{} priority 0 rule for interfaceName {} for TCP", operation == Operation::Create ? "Add" : "Remove", interfaceName.c_str());
    Rule rule = Rule(AF_INET, RT_TABLE_LOCAL, c_WindowsToLinuxRulePriority, Protocol::Tcp);
    rule.iif = interfaceName;
    ruleManager.ModifyLoopbackRule(rule, operation);

    GNS_LOG_INFO(
        "{} priority 0 rule for interfaceName {} for UDP", operation == Operation::Create ? "Add" : "Remove", interfaceName.c_str());
    rule = Rule(AF_INET, RT_TABLE_LOCAL, c_WindowsToLinuxRulePriority, Protocol::Udp);
    rule.iif = interfaceName;
    ruleManager.ModifyLoopbackRule(rule, operation);
}

/*
    Adds the policy rules required for loopback. Also adds routes for the loopback address range
    127.0.0.1/32 or ::1/128.
*/
void NetworkManager::InitializeLoopbackConfigurationImpl(Interface& gelnic, int addressFamily)
{
    // Set to GELNIC to status up before adding the configurations
    gelnic.SetUp();

    AddMirroredLoopbackRoutingRules(gelnic, addressFamily);

    auto gateway = addressFamily == AF_INET ? c_ipv4LoopbackGateway : c_ipv6LoopbackGateway;
    auto addressRange = addressFamily == AF_INET ? c_loopbackV4AddressRange : c_loopbackV6AddressRange;

    // Add a static ARP entry for the loopback gateway. The purpose of the static entries is
    // to guarantee that each loopback packet that leaves the guest has the same destination MAC.
    GNS_LOG_INFO("Adding static ARP entry for the loopback gateway  {}", gateway.Addr().c_str());
    Neighbor neighbor = Neighbor(gateway, c_gatewayMacAddress, gelnic.Index());
    neighborManager.ModifyNeighborEntry(neighbor, Operation::Create);

    // Add routes for 127.0.0.1/32 or ::1/128
    Route route = Route(addressFamily, gateway, gelnic.Index(), false, addressRange, 0);
    route.isLoopbackRoute = true;

    const auto routeString = utils::Stringify(route);
    GNS_LOG_INFO("Add route {} on GELNIC adapter {}", routeString.c_str(), gelnic.Name().c_str());
    loopbackRoutingTable.ModifyRoute(route, Operation::Create);
}

/*
    Add or remove loopback routes for the set of IP addresses that were added/deleted on an interface. All routes are via the
    same gateway address. The function can be used for both IPv4 and IPv6 addresses.
*/
void NetworkManager::UpdateLoopbackRoute(Interface& interface, const Address& address, Operation operation)
{
    assert(operation == Operation::Create || operation == Operation::Remove);

    // For the moment don't process IPv6 addresses, since inbound IPv6 loopback is not supported yet (dropped by
    // default by the Linux stack). Once that is addressed, this check will be removed.
    if (address.Family() == AF_INET6)
    {
        GNS_LOG_INFO("Ignoring IPv6 address {}", utils::Stringify(address).c_str());
        return;
    }

    auto gateway = address.Family() == AF_INET ? c_ipv4LoopbackGateway : c_ipv6LoopbackGateway;

    if (operation == Operation::Create)
    {
        // When adding routes, always add the static neighbor entry for the loopback gateway. The purpose of the static entries is
        // to guarantee that each loopback packet that leaves the guest has the same destination MAC.
        //
        // Note: The entries are added each time we add routes in order to avoid keeping track of whether they are added or not
        // (as the entries will be lost when an interface changes state to down).
        GNS_LOG_INFO("Adding static neighbor entry for the loopback gateway {}", gateway.Addr().c_str());
        Neighbor neighbor = Neighbor(gateway, c_gatewayMacAddress, interface.Index());
        neighborManager.ModifyNeighborEntry(neighbor, Operation::Create);
    }

    Route route = Route(address.Family(), gateway, interface.Index(), false, address, 0);
    route.isLoopbackRoute = true;

    const auto routeString = utils::Stringify(route);
    GNS_LOG_INFO(
        "{} loopback route {} on interfaceName {}",
        operation == Operation::Create ? "Add" : "Remove",
        routeString.c_str(),
        interface.Name().c_str());
    localRoutingTable.ModifyRoute(route, operation);
}

void NetworkManager::ResetLoopbackRoutes()
{
    localRoutingTable.RemoveAll(AF_UNSPEC);
}

void NetworkManager::CreateTunAdapter(const std::string& name)
{
    Interface::CreateTunAdapter(name);

    // Enable routing of IPv4 loopback on the tunnel interface.
    GNS_LOG_INFO("Enabling IPv4 loopback routing on tunnel adapter with name {}", name.c_str());
    Interface tunInterface = {-1, name};
    EnableLoopbackRouting(tunInterface);
}

void NetworkManager::ModifyNetSetting(int addressFamily, const char* settingName, const char* scope, const char* settingValue, size_t settingValueLen)
{
    const std::filesystem::path settingFilePath =
        std::format("/proc/sys/net/{}/conf/{}/{}", ((addressFamily == AF_INET) ? "ipv4" : "ipv6"), scope, settingName);
    wil::unique_fd fd(Syscall(open, settingFilePath.c_str(), (O_WRONLY | O_CLOEXEC)));
    Syscall(write, fd.get(), settingValue, settingValueLen);
}

void NetworkManager::DisableRouterDiscovery()
{
    ModifyNetSetting(AF_INET6, "accept_ra", "all", c_disableSetting, strlen(c_disableSetting));
    ModifyNetSetting(AF_INET6, "accept_ra", "default", c_disableSetting, strlen(c_disableSetting));
}

void NetworkManager::DisableDAD()
{
    // DAD is not enabled for IPv4 by default on Linux-based systems, so only disable for IPv6.
    ModifyNetSetting(AF_INET6, "dad_transmits", "all", c_disableSetting, strlen(c_disableSetting));
    ModifyNetSetting(AF_INET6, "dad_transmits", "default", c_disableSetting, strlen(c_disableSetting));
}

void NetworkManager::DisableIpv6AddressGeneration()
{
    // Disable autoconfiguration.
    ModifyNetSetting(AF_INET6, "autoconf", "all", c_disableSetting, strlen(c_disableSetting));
    ModifyNetSetting(AF_INET6, "autoconf", "default", c_disableSetting, strlen(c_disableSetting));

    // Disable link local address generation.
    constexpr char c_genModeNone[] = "1\n";
    ModifyNetSetting(AF_INET6, "addr_gen_mode", "all", c_genModeNone, strlen(c_genModeNone));
    ModifyNetSetting(AF_INET6, "addr_gen_mode", "default", c_genModeNone, strlen(c_genModeNone));

    // Disable privacy extensions, i.e. temporary address generation.
    ModifyNetSetting(AF_INET6, "use_tempaddr", "all", c_disableSetting, strlen(c_disableSetting));
    ModifyNetSetting(AF_INET6, "use_tempaddr", "default", c_disableSetting, strlen(c_disableSetting));
}

void NetworkManager::EnableIpv4ArpFilter()
{
    // sets /proc/sys/net/ipv4/conf/all/arp_filter to a value of 1
    // this is to stop Linux from attempting to ARP a configured IP address across all connected interfaces
    // setting this to 1 instructs Linux to only ARP for that address over the interface that the address was assigned
    // This setting is required to avoid breaking mirroring where multiple interfaces are mirrored on the same network (they have
    // addresses on the same prefix) which can cause the Host to interpret an ARP from an interface without the address to be a
    // duplicate which causes the host fail DAD, and DHCP immediately requests a new address (this will just continue in a loop)
    ModifyNetSetting(AF_INET, "arp_filter", "all", c_enableSetting, strlen(c_enableSetting));
    ModifyNetSetting(AF_INET, "arp_filter", "default", c_enableSetting, strlen(c_enableSetting));
}

wsl::shared::conncheck::ConnCheckResult NetworkManager::SendConnectRequest(const char* remoteAddress)
{
    return wsl::shared::conncheck::CheckConnection(remoteAddress, nullptr, "80");
}