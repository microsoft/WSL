// Copyright (C) Microsoft Corporation. All rights reserved.
#pragma once

#include <string>
#include <net/if.h>
#include <vector>
#include <optional>
#include <linux/netlink.h>
#include "InterfaceConfiguration.h"
#include "NetlinkResponse.h"
#include "Operation.h"

class Interface
{
public:
    Interface();
    Interface(const Interface& source);
    Interface(int index, const std::string& name);

    operator bool() const;

    InterfaceConfiguration Ipv4Configuration();
    InterfaceConfiguration Ipv6Configuration();

    void CreateVirtualWifiAdapter(const std::string& wifiName);
    void CreateProxyWifiAdapter(const std::string& wifiName);
    static void CreateBondAdapter(const std::string& bondName);
    static void CreateTunAdapter(const std::string& name);
    static void CreateTapAdapter(const std::string& name);
    void DeleteInterface();

    void SetIpv4Configuration(const InterfaceConfiguration& configuration);
    void SetIpv6Configuration(const InterfaceConfiguration& configuration);

    void ModifyIpAddress(const Address& address, Operation operation);

    void SetMacAddress(const MacAddress& address);
    MacAddress GetMacAddress();

    void SetName(const std::string& newName);

    void SetNamespace(int namespaceFd);

    void SetWiphyNamespace(int namespaceFd);

    void AddFlags(int flags);

    void RemoveFlags(int flags);

    void SetUp();

    void SetDown();

    int Index() const;

    void SetMtu(int mtu);

    void SetMetric(int metric);

    void RemoveFromBond(const Interface& child_interface);

    void AddToBond(const Interface& child_interface);

    void SetActiveChild(const Interface& child_interface);

    void DisableNetworkSetting(const char* settingName, int addressFamily);
    void EnableNetworkSetting(const char* settingName, int addressFamily);

    void ResetIpv6State();

    const std::string& Name() const;

    static Interface Open(const std::string& name);

    void ModifyTcClassifier(bool Add);
    void BpfAttachTcClassifier(int ProgramFd, bool Ingress);

private:
    template <size_t wifiTypeArraySize>
    void CreateWifiAdapter(const std::string& wifiName, const char (&wifiType)[wifiTypeArraySize]);

    template <typename TAddr>
    void ChangeAddress(const Address& address, const std::optional<Address>& broadcastAddress, Operation operation);

    template <typename TAddr, typename TMessage>
    void ChangeAddressImpl(const Address& address, const std::optional<Address>& broadcastAddress, Operation operation);

    template <typename TAddr>
    void SetConfiguration(const InterfaceConfiguration& current, const InterfaceConfiguration& configuration);

    static void CreateTunTapAdapter(const std::string& name, bool TunAdapter);

    InterfaceConfiguration ListAddressesImpl(int af);

    int m_index = -1;
    std::string m_name;
};
