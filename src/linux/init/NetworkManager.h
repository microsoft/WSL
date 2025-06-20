// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once
#include <Interface.h>
#include <RoutingTable.h>
#include <IpRuleManager.h>
#include <IpNeighborManager.h>
#include <conncheckshared.h>
#include "hns_schema.h"

class NetworkManager
{
public:
    NetworkManager(RoutingTable& routingTable);

    Interface CreateVirtualWifiAdapter(Interface& baseAdapter, const std::string& wifiName);

    Interface CreateProxyWifiAdapter(Interface& baseAdapter, const std::string& wifiName);

    void SetAdapterConfiguration(Interface& adapter, const wsl::shared::hns::HNSEndpoint& configuration);

    enum class InterfaceState
    {
        Up,
        Down
    };

    void SetInterfaceState(Interface& adapter, InterfaceState state);

    void SetAdapterName(Interface& adapter, const std::string& name);

    void SetAdapterNamespace(Interface& adapter, int namespaceFd);

    void SetWiphyNamespace(Interface& adapter, int namespaceFd);

    std::optional<int> FindRoutingTableIdForInterface(const Interface& interface) const;

    void ChangePrimaryRoutingTable(int newTableId);

    std::vector<Route> ListRoutes(int family) const;

    void ModifyRoute(const Route& route, Operation operation);

    void ModifyAddress(Interface& adapter, const Address& address, Operation operation);

    void ResetRoutingTable(int addressFamily, const Interface& interface);

    void SetAdapterMacAddress(Interface& interface, const MacAddress& address);

    void DisassociateAdapterFromBond(const std::string& bondInterfaceName, Interface& interface);

    void AssociateAdapterWithBond(const std::string& bondInterfaceName, Interface& interface);

    void ActivateAdapterWithBond(const std::string& bondInterfaceName, const Interface& interface);

    Interface CreateBondAdapter(const std::string& name);

    void EnableLoopbackRouting(Interface& interface);

    void InitializeLoopbackConfiguration(Interface& gelnic);

    void AddMirroredLoopbackRoutingRules(Interface& gelnic, int addressFamily);

    void UpdateMirroredLoopbackRulesForInterface(const std::string& interfaceName, Operation operation);

    void UpdateLoopbackRoute(Interface& interface, const Address& address, Operation operation);

    void ResetLoopbackRoutes();

    void CreateTunAdapter(const std::string& name);

    void DisableRouterDiscovery();

    void DisableDAD();

    void DisableIpv6AddressGeneration();

    void EnableIpv4ArpFilter();

    wsl::shared::conncheck::ConnCheckResult SendConnectRequest(const char* remoteAddress);

private:
    void InitializeLoopbackConfigurationImpl(Interface& gelnic, int addressFamily);

    RoutingTable& routingTable;
    // Custom routing tables used for loopback mirroring. Not to be confused with the Linux "local" table
    RoutingTable loopbackRoutingTable;
    RoutingTable localRoutingTable;

    IpRuleManager ruleManager;
    IpNeighborManager neighborManager;

    void ModifyNetSetting(int addressFamily, const char* settingName, const char* scope, const char* settingValue, size_t settingValueLen);
};
