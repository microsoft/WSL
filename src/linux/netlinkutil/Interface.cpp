// Copyright (C) Microsoft Corporation. All rights reserved.
#include <sys/ioctl.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <linux/netlink.h>
#include <linux/nl80211.h>
#include <linux/rtnetlink.h>
#include <iostream>
#include <optional>
#include <gsl/gsl>
#include <gslhelpers.h>
#include <string.h>
#include <ifaddrs.h>
#include <linux/if_packet.h>
#include <linux/pkt_cls.h>
#include <lxwil.h>
#include <linux/if_tun.h>
#include "NetlinkChannel.h"
#include "Syscall.h"
#include "Utils.h"
#include "Interface.h"
#include "lxwil.h"

using utils::Attribute;

constexpr char c_value0[] = "0\n";
constexpr char c_value1[] = "1\n";

template <typename TAddr>
struct AddressMessage
{
    ifaddrmsg ifaddr;
    utils::AddressAttribute<TAddr> localAddress;
    utils::AddressAttribute<TAddr> address;
    utils::CacheInfoAttribute cacheInfo;
    utils::IntegerAttribute addressFlags;
} __attribute__((packed));

template <typename TAddr>
struct AddressMessageWithBroadcast : AddressMessage<TAddr>
{
    utils::AddressAttribute<TAddr> broadcastAddress;
} __attribute__((packed));

Interface::Interface()
{
}

Interface::Interface(const Interface& source) : m_index(source.m_index), m_name(source.m_name)
{
}

Interface::Interface(int index, const std::string& name) : m_index(index), m_name(name)
{
}

Interface::operator bool() const
{
    return m_index != -1;
}

template <typename TAddr>
void Interface::ChangeAddress(const Address& address, const std::optional<Address>& broadcastAddress, Operation operation)
{
    if (broadcastAddress.has_value())
    {
        ChangeAddressImpl<TAddr, AddressMessageWithBroadcast<TAddr>>(address, broadcastAddress, operation);
    }
    else
    {
        ChangeAddressImpl<TAddr, AddressMessage<TAddr>>(address, broadcastAddress, operation);
    }
}

// TMessage must be derived from AddressMessage or one of its children
template <typename TAddr, typename TMessage>
void Interface::ChangeAddressImpl(const Address& address, const std::optional<Address>& broadcastAddress, Operation operation)
{
    TMessage message{};

    message.ifaddr.ifa_family = address.Family();
    message.ifaddr.ifa_prefixlen = address.PrefixLength();
    message.ifaddr.ifa_index = m_index;
    message.ifaddr.ifa_scope = address.Scope();

    utils::InitializeAddressAttribute<TAddr>(message.address, address, IFA_ADDRESS);
    utils::InitializeAddressAttribute<TAddr>(message.localAddress, address, IFA_LOCAL);
    utils::InitializeCacheInfoAttribute(message.cacheInfo, address);

    // For remove, most flags are ignored, including noprefixroute and nodad.
    int addrFlags = address.IsPrefixRouteAutogenerationDisabled() ? IFA_F_NOPREFIXROUTE : 0;
    if (address.Family() == AF_INET6)
    {
        addrFlags |= IFA_F_NODAD;
    }
    utils::InitializeIntegerAttribute(message.addressFlags, addrFlags, IFA_FLAGS);

    if constexpr (std::is_same<TMessage, AddressMessageWithBroadcast<TAddr>>::value)
    {
        utils::InitializeAddressAttribute<TAddr>(message.broadcastAddress, broadcastAddress.value(), IFA_BROADCAST);
    }

    int flags = 0;
    if (operation == Update)
    {
        flags = NLM_F_CREATE | NLM_F_REPLACE;
    }
    else if (operation == Create)
    {
        flags = NLM_F_CREATE | NLM_F_EXCL;
    }

    NetlinkChannel channel;
    auto transaction = channel.CreateTransaction<TMessage>(message, operation == Remove ? RTM_DELADDR : RTM_NEWADDR, flags);

    transaction.Execute();
}

void Interface::ModifyIpAddress(const Address& address, Operation operation)
{
    // NOTE: in-place address update is not supported via NetLink, so remove the address first and
    // then re-add it. We assume that the caller has saved any other related state beforehand to
    // restore, such as routes.
    if (address.IsIpv4())
    {
        std::optional<Address> broadcastAddress = utils::ComputeBroadcastAddress(address);

        if (operation == Update)
        {
            std::vector<Address> v4Addresses = Ipv4Configuration().Addresses;
            if (std::find(v4Addresses.begin(), v4Addresses.end(), address) != v4Addresses.end())
            {
                ChangeAddress<in_addr>(address, broadcastAddress, Remove);
            }
        }

        ChangeAddress<in_addr>(address, broadcastAddress, operation);
    }
    else
    {
        if (operation == Update)
        {
            std::vector<Address> v6Addresses = Ipv6Configuration().Addresses;
            if (std::find(v6Addresses.begin(), v6Addresses.end(), address) != v6Addresses.end())
            {
                ChangeAddress<in6_addr>(address, {}, Remove);
            }
        }

        ChangeAddress<in6_addr>(address, {}, operation);
    }
}

template <size_t wifiTypeArraySize>
void Interface::CreateWifiAdapter(const std::string& wifiName, const char (&wifiType)[wifiTypeArraySize])
{
    constexpr int wifiTypeSize = wifiTypeArraySize - 1;
    struct Request
    {
        ifinfomsg Message __attribute__((aligned(NLMSG_ALIGNTO)));
        utils::IntegerAttribute LinkAttribute;
        Attribute<Attribute<char[wifiTypeSize]>> LinkInfoAttribute;
        Attribute<char> NameAttribute;
    } __attribute__((packed));

    auto buffer = std::vector<char>(RTA_ALIGN(offsetof(Request, NameAttribute.value) + wifiName.size()));
    auto* request = gslhelpers::get_struct<Request>(gsl::make_span(buffer));

    utils::InitializeIntegerAttribute(request->LinkAttribute, m_index, IFLA_LINK);

    request->LinkInfoAttribute.header.rta_len = RTA_SPACE(sizeof(Attribute<char[wifiTypeSize]>));
    request->LinkInfoAttribute.header.rta_type = IFLA_LINKINFO;
    request->LinkInfoAttribute.value.header.rta_len = RTA_SPACE(wifiTypeSize);
    request->LinkInfoAttribute.value.header.rta_type = IFLA_INFO_KIND;
    auto typeBuffer = gsl::make_span(buffer).subspan(offsetof(Request, LinkInfoAttribute.value.value));
    gsl::copy(gsl::make_span(wifiType, wifiTypeSize), typeBuffer);

    request->NameAttribute.header.rta_len = RTA_SPACE(wifiName.size());
    request->NameAttribute.header.rta_type = IFLA_IFNAME;
    auto nameBuffer = gsl::make_span(buffer).subspan(offsetof(Request, NameAttribute.value));
    gsl::copy(gsl::make_span(wifiName), nameBuffer);

    NetlinkChannel channel;
    auto transaction = channel.CreateTransaction(buffer.data(), buffer.size(), RTM_NEWLINK, NLM_F_CREATE | NLM_F_EXCL);
    transaction.Execute();
}

void Interface::CreateVirtualWifiAdapter(const std::string& wifiName)
{
    constexpr char virtualWifiType[] = "virt_wifi";
    CreateWifiAdapter(wifiName, virtualWifiType);
}

void Interface::CreateProxyWifiAdapter(const std::string& wifiName)
{
    constexpr char proxyWifiType[] = "proxy_wifi";
    CreateWifiAdapter(wifiName, proxyWifiType);
}

void Interface::CreateBondAdapter(const std::string& bondName)
{
    constexpr char bondType[] = "bond";
    // This corresponds to the command generated by:
    // ip link add type bond mode active-backup fail_over_mac active
    // Format appears to be:
    // uint16_t 0x5
    // uint16_t option (0x1 = bond mode, 0xd = fail_over_mac)
    // uint32_t setting (0x1 = active-backup for bond_mode, 0x1 = active for fail_over_mac)
    constexpr char bondData[] = {0x05, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x05, 0x00, 0x0d, 0x00, 0x01, 0x00, 0x00, 0x00};
    constexpr int bondTypeSize = sizeof(bondType) - 1;
    constexpr int bondDataSize = sizeof(bondData);
    struct LinkInfo
    {
        Attribute<char[bondTypeSize]> KindAttribute;
        Attribute<char[bondDataSize]> DataAttribute;
    } __attribute__((packed));
    struct Request
    {
        ifinfomsg Message __attribute__((aligned(NLMSG_ALIGNTO)));
        Attribute<LinkInfo> LinkInfoAttribute;
        Attribute<char> NameAttribute;
    } __attribute__((packed));

    auto buffer = std::vector<char>(RTA_ALIGN(offsetof(Request, NameAttribute.value) + bondName.size()));
    auto* request = gslhelpers::get_struct<Request>(gsl::make_span(buffer));

    request->LinkInfoAttribute.header.rta_len = RTA_SPACE(sizeof(LinkInfo));
    request->LinkInfoAttribute.header.rta_type = IFLA_LINKINFO;

    request->LinkInfoAttribute.value.KindAttribute.header.rta_len = RTA_SPACE(bondTypeSize);
    request->LinkInfoAttribute.value.KindAttribute.header.rta_type = IFLA_INFO_KIND;
    auto typeBuffer = gsl::make_span(buffer).subspan(offsetof(Request, LinkInfoAttribute.value.KindAttribute.value));
    gsl::copy(gsl::make_span(bondType, bondTypeSize), typeBuffer);

    request->LinkInfoAttribute.value.DataAttribute.header.rta_len = RTA_SPACE(bondDataSize);
    request->LinkInfoAttribute.value.DataAttribute.header.rta_type = IFLA_INFO_DATA;
    auto dataBuffer = gsl::make_span(buffer).subspan(offsetof(Request, LinkInfoAttribute.value.DataAttribute.value));
    gsl::copy(gsl::make_span(bondData, bondDataSize), dataBuffer);

    request->NameAttribute.header.rta_len = RTA_SPACE(bondName.size());
    request->NameAttribute.header.rta_type = IFLA_IFNAME;
    auto nameBuffer = gsl::make_span(buffer).subspan(offsetof(Request, NameAttribute.value));
    gsl::copy(gsl::make_span(bondName), nameBuffer);

    NetlinkChannel channel;
    auto transaction = channel.CreateTransaction(buffer.data(), buffer.size(), RTM_NEWLINK, NLM_F_CREATE | NLM_F_EXCL);
    transaction.Execute();
}

void Interface::RemoveFromBond(const Interface& child_interface)
{
    struct Message
    {
        ifinfomsg ifinfo;
        utils::IntegerAttribute master;
    } __attribute__((packed));

    Message message{};
    message.ifinfo.ifi_index = child_interface.Index();
    message.master.header.rta_len = sizeof(message.master);
    message.master.header.rta_type = IFLA_MASTER;
    message.master.value = 0;

    NetlinkChannel channel;
    auto transaction = channel.CreateTransaction(message, RTM_NEWLINK, 0);
    transaction.Execute([](const NetlinkResponse& response) {
        fprintf(stderr, "Interface::RemoveFromBond netlink response: %s\n", utils::Stringify(response).c_str());
    });
}

void Interface::AddToBond(const Interface& child_interface)
{
    struct Message
    {
        ifinfomsg ifinfo;
        utils::IntegerAttribute master;
    } __attribute__((packed));

    Message message{};
    message.ifinfo.ifi_index = child_interface.Index();
    utils::InitializeIntegerAttribute(message.master, m_index, IFLA_MASTER);

    NetlinkChannel channel;
    auto transaction = channel.CreateTransaction(message, RTM_NEWLINK, 0);
    transaction.Execute([](const NetlinkResponse& response) {
        fprintf(stderr, "Interface::AddToBond netlink response: %s\n", utils::Stringify(response).c_str());
    });
}

void Interface::DeleteInterface()
{
    struct Message
    {
        ifinfomsg ifinfo;
    } __attribute__((packed));

    Message message{};
    message.ifinfo.ifi_index = m_index;

    NetlinkChannel channel;
    auto transaction = channel.CreateTransaction(message, RTM_DELLINK, 0);
    transaction.Execute([](const NetlinkResponse& response) {
        fprintf(stderr, "Interface::DeleteInterface netlink response: %s\n", utils::Stringify(response).c_str());
    });
}

void Interface::SetActiveChild(const Interface& child_interface)
{
    constexpr char bondType[] = "bond";
    constexpr int bondTypeSize = sizeof(bondType) - 1;

    struct ActiveChildInfo
    {
        utils::IntegerAttribute ActiveChildAttribute;
    } __attribute__((packed));

    struct LinkInfo
    {
        Attribute<gsl::byte[bondTypeSize]> KindAttribute;
        Attribute<ActiveChildInfo> DataAttribute;
    } __attribute__((packed));

    struct Request
    {
        ifinfomsg Message __attribute__((aligned(NLMSG_ALIGNTO)));
        Attribute<LinkInfo> LinkInfoAttribute;
    } __attribute__((packed));

    auto buffer = std::vector<gsl::byte>(RTA_ALIGN(sizeof(Request)));
    auto* request = gslhelpers::get_struct<Request>(gsl::make_span(buffer));

    request->Message.ifi_index = m_index;

    request->LinkInfoAttribute.header.rta_len = sizeof(request->LinkInfoAttribute);
    request->LinkInfoAttribute.header.rta_type = IFLA_LINKINFO;

    request->LinkInfoAttribute.value.KindAttribute.header.rta_len = sizeof(request->LinkInfoAttribute.value.KindAttribute);
    request->LinkInfoAttribute.value.KindAttribute.header.rta_type = IFLA_INFO_KIND;
    auto typeBuffer = gsl::make_span(buffer).subspan(offsetof(Request, LinkInfoAttribute.value.KindAttribute.value));
    gsl::copy(gsl::as_bytes(gsl::make_span(bondType, bondTypeSize)), typeBuffer);

    request->LinkInfoAttribute.value.DataAttribute.header.rta_len = sizeof(request->LinkInfoAttribute.value.DataAttribute);
    request->LinkInfoAttribute.value.DataAttribute.header.rta_type = IFLA_INFO_DATA;

    utils::InitializeIntegerAttribute(
        request->LinkInfoAttribute.value.DataAttribute.value.ActiveChildAttribute, child_interface.Index(), IFLA_BOND_ACTIVE_SLAVE);

    NetlinkChannel channel;
    auto transaction = channel.CreateTransaction(buffer.data(), buffer.size(), RTM_NEWLINK, 0);
    transaction.Execute([](const NetlinkResponse& response) {
        fprintf(stderr, "Interface::SetActiveChild(bond) netlink response: %s\n", utils::Stringify(response).c_str());
    });
}

void Interface::CreateTunTapAdapter(const std::string& name, bool TunAdapter)
{
    wil::unique_fd fd;
    if (name.size() > IFNAMSIZ)
    {
        throw RuntimeErrorWithSourceLocation("Tun adapter name exceeds IFNAMSIZ");
    }

    ifreq ifr{};
    ifr.ifr_flags = TunAdapter ? IFF_TUN : IFF_TAP;
    fd = Syscall(open, "/dev/net/tun", O_RDWR);
    std::copy(name.begin(), name.end(), ifr.ifr_name);
    Syscall(ioctl, fd.get(), TUNSETIFF, &ifr);
    Syscall(ioctl, fd.get(), TUNSETPERSIST, 1);
}

void Interface::CreateTunAdapter(const std::string& name)
{
    CreateTunTapAdapter(name, true);
}

void Interface::CreateTapAdapter(const std::string& name)
{
    CreateTunTapAdapter(name, false);
}

void Interface::SetMtu(int mtu)
{
    struct Message
    {
        ifinfomsg ifinfo;
        utils::IntegerAttribute mtu;
    } __attribute__((packed));

    Message message{};
    message.ifinfo.ifi_index = m_index;
    utils::InitializeIntegerAttribute(message.mtu, mtu, IFLA_MTU);

    NetlinkChannel channel;
    auto transaction = channel.CreateTransaction(message, RTM_NEWLINK, 0);
    transaction.Execute();
}

void Interface::SetMetric(int metric)
{
    struct Message
    {
        ifinfomsg ifinfo;
        utils::IntegerAttribute metric;
    } __attribute__((packed));

    Message message{};
    message.ifinfo.ifi_index = m_index;
    utils::InitializeIntegerAttribute(message.metric, metric, IFLA_PRIORITY);

    NetlinkChannel channel;
    auto transaction = channel.CreateTransaction(message, RTM_NEWLINK, 0);
    transaction.Execute();
}

void Interface::SetIpv4Configuration(const InterfaceConfiguration& configuration)
{
    SetConfiguration<in_addr>(Ipv4Configuration(), configuration);
}

void Interface::SetIpv6Configuration(const InterfaceConfiguration& configuration)
{
    SetConfiguration<in6_addr>(Ipv6Configuration(), configuration);
}

void Interface::SetName(const std::string& newName)
{
    struct Request
    {
        struct ifinfomsg Message __attribute__((aligned(NLMSG_ALIGNTO)));
        Attribute<char> Attribute;
    };

    auto buffer = std::vector<char>(RTA_ALIGN(offsetof(Request, Attribute.value) + newName.size()));
    auto* request = gslhelpers::get_struct<Request>(gsl::make_span(buffer));
    request->Message.ifi_index = m_index;
    request->Attribute.header.rta_len = RTA_LENGTH(newName.size());
    request->Attribute.header.rta_type = IFLA_IFNAME;
    auto nameBuffer = gsl::make_span(buffer).subspan(offsetof(Request, Attribute.value));
    gsl::copy(gsl::make_span(newName), nameBuffer);

    NetlinkChannel channel;
    auto transaction = channel.CreateTransaction(buffer.data(), buffer.size(), RTM_NEWLINK, 0);
    transaction.Execute();
}

void Interface::SetWiphyNamespace(int namespaceFd)
{
    struct Request
    {
        int Command;
        utils::IntegerAttribute Wiphy;
        utils::IntegerAttribute NamespaceFd;
    };

    Request request{};
    request.Command = NL80211_CMD_SET_WIPHY_NETNS;

    utils::InitializeIntegerAttribute(request.Wiphy, 0, NL80211_ATTR_WIPHY);
    utils::InitializeIntegerAttribute(request.NamespaceFd, namespaceFd, NL80211_ATTR_NETNS_FD);

    NetlinkChannel channel(SOCK_RAW, NETLINK_GENERIC);
    auto transaction = channel.CreateTransaction(request, 0x15, 0);
    transaction.Execute();
}

void Interface::SetNamespace(int namespaceFd)
{
    struct Request
    {
        struct ifinfomsg Message __attribute__((aligned(NLMSG_ALIGNTO)));
        utils::IntegerAttribute Attribute;
    };

    Request request{};
    request.Message.ifi_index = m_index;

    utils::InitializeIntegerAttribute(request.Attribute, namespaceFd, IFLA_NET_NS_FD);

    NetlinkChannel channel;
    auto transaction = channel.CreateTransaction<Request>(request, RTM_NEWLINK, 0);
    transaction.Execute();
}

void Interface::AddFlags(int flags)
{
    NetlinkChannel channel;
    const auto currentFlags = channel.GetInterfaceFlags(m_name);

    channel.SetInterfaceFlags(m_name, currentFlags | flags);
}

void Interface::RemoveFlags(int flags)
{
    NetlinkChannel channel;
    const auto currentFlags = channel.GetInterfaceFlags(m_name);

    channel.SetInterfaceFlags(m_name, currentFlags & ~flags);
}

void Interface::SetUp()
{
    AddFlags(IFF_UP | IFF_RUNNING);
}

void Interface::SetDown()
{
    RemoveFlags(IFF_UP | IFF_RUNNING);
}

void Interface::SetMacAddress(const MacAddress& address)
{
    static_assert(sizeof(address) == 6);

    struct Message
    {
        ifinfomsg ifinfo;
        utils::MacAddressAttribute address;
    } __attribute__((packed));

    Message message{};
    message.ifinfo.ifi_index = m_index;
    message.address.header.nla_len = sizeof(message.address);
    message.address.header.nla_type = IFLA_ADDRESS;
    message.address.address = address;

    NetlinkChannel channel;
    auto transaction = channel.CreateTransaction(message, RTM_NEWLINK, 0);
    transaction.Execute();
}

MacAddress Interface::GetMacAddress()
{
    MacAddress address;
    ifreq interface_request{};
    wil::unique_fd fd;
    fd = Syscall(socket, AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    std::copy(m_name.begin(), m_name.end(), interface_request.ifr_ifrn.ifrn_name);
    Syscall(ioctl, fd.get(), SIOCGIFHWADDR, &interface_request);
    std::copy(
        interface_request.ifr_ifru.ifru_hwaddr.sa_data, interface_request.ifr_ifru.ifru_hwaddr.sa_data + address.size(), address.begin());
    return address;
}

template <typename TAddr>
void Interface::SetConfiguration(const InterfaceConfiguration& currentConfiguration, const InterfaceConfiguration& configuration)
{
    std::vector<const Address*> add;
    std::vector<const Address*> remove;

    auto compare = [](const std::vector<Address>& left, const std::vector<Address>& right, std::vector<const Address*>& dest) {
        for (const auto& e : left)
        {
            if (std::find(right.begin(), right.end(), e) == right.end())
            {
                dest.emplace_back(&e);
            }
        }
    };

    compare(currentConfiguration.Addresses, configuration.Addresses, remove);
    compare(configuration.Addresses, currentConfiguration.Addresses, add);

    for (const auto* address : remove)
    {
        ChangeAddress<TAddr>(*address, configuration.BroadcastAddress, Remove);
    }

    for (const auto* address : add)
    {
        ChangeAddress<TAddr>(*address, configuration.BroadcastAddress, Create);
    }
}

InterfaceConfiguration Interface::Ipv6Configuration()
{
    return ListAddressesImpl(AF_INET6);
}

InterfaceConfiguration Interface::Ipv4Configuration()
{
    return ListAddressesImpl(AF_INET);
}

InterfaceConfiguration Interface::ListAddressesImpl(int af)
{
    InterfaceConfiguration configuration;
    auto routine = [&](const NetlinkResponse& response) {
        auto messages = response.Messages<ifaddrmsg>(RTM_NEWADDR);

        for (const auto& message : messages)
        {
            const auto* ifaddr = message.Payload();
            if (ifaddr->ifa_index != static_cast<size_t>(m_index))
            {
                continue;
            }

            auto getAddresses = [&](int type, std::vector<Address>& dest) {
                for (const auto attribute : message.Attributes<const void*>(type))
                {
                    auto messageAf = message.Payload()->ifa_family;
                    int prefixLength = message.Payload()->ifa_prefixlen;
                    dest.emplace_back(Address::FromBinary(messageAf, prefixLength, attribute));
                }
            };

            getAddresses(IFA_ADDRESS, configuration.Addresses);
            getAddresses(IFA_LOCAL, configuration.LocalAddresses);

            std::vector<Address> broadcastAddresses;
            getAddresses(IFA_BROADCAST, broadcastAddresses);
            if (broadcastAddresses.size() == 1)
            {
                configuration.BroadcastAddress = std::move(broadcastAddresses[0]);
            }
            else if (broadcastAddresses.size() > 1)
            {
                throw RuntimeErrorWithSourceLocation("More than one broadcast address found");
            }
        }
    };

    NetlinkChannel channel;

    ifaddrmsg payload = {};
    payload.ifa_family = af;
    payload.ifa_index = m_index;
    auto transaction = channel.CreateTransaction(payload, RTM_GETADDR, NLM_F_DUMP);

    transaction.Execute(routine);

    return configuration;
}

/*
    Implements functionality equivalent to sysctl -w net.ipv(4/6).conf.<interface>.<setting>, by writing to
    /proc/sys/net/ipv(4/6)/conf/m_name
*/
void Interface::EnableNetworkSetting(const char* settingName, int addressFamily)
{
    const std::string settingFilePath =
        std::format("/proc/sys/net/{}/conf/{}/{}", (addressFamily == AF_INET ? "ipv4" : "ipv6"), m_name, settingName);

    wil::unique_fd fd(Syscall(open, settingFilePath.c_str(), (O_WRONLY | O_CLOEXEC)));

    Syscall(write, fd.get(), c_value1, sizeof(c_value1));
}

void Interface::DisableNetworkSetting(const char* settingName, int addressFamily)
{
    const std::string settingFilePath =
        std::format("/proc/sys/net/{}/conf/{}/{}", (addressFamily == AF_INET ? "ipv4" : "ipv6"), m_name, settingName);

    wil::unique_fd fd(Syscall(open, settingFilePath.c_str(), (O_WRONLY | O_CLOEXEC)));

    Syscall(write, fd.get(), c_value0, sizeof(c_value0));
}

void Interface::ResetIpv6State()
{
    // Disable IPv6, turn off address autogeneration and router discovery, and then re-enable IPv6.
    // Some process (presumably netd) re-enables some of these settings after receiving new
    // adapter configuration (eg, after connecting to a new Wi-Fi network), so this function should
    // be called  after that to restore the settings to the desired values.
    EnableNetworkSetting("disable_ipv6", AF_INET6);

    DisableNetworkSetting("accept_ra", AF_INET6);
    DisableNetworkSetting("autoconf", AF_INET6);
    DisableNetworkSetting("use_tempaddr", AF_INET6);
    EnableNetworkSetting("addr_gen_mode", AF_INET6); // A value of 1 means address generation is disabled.

    DisableNetworkSetting("disable_ipv6", AF_INET6);
}

int Interface::Index() const
{
    return m_index;
}

const std::string& Interface::Name() const
{
    return m_name;
}

Interface Interface::Open(const std::string& name)
{
    NetlinkChannel channel;
    return {channel.GetInterfaceIndex(name), name};
}

void Interface::ModifyTcClassifier(bool Add)
{
    char kind[] = "clsact";
    struct Message
    {
        tcmsg TcMsg;
        Attribute<char[sizeof(kind)]> Kind;
    } __attribute__((packed));
    Message message{};

    message.TcMsg.tcm_family = AF_UNSPEC;
    message.TcMsg.tcm_ifindex = m_index;
    message.TcMsg.tcm_handle = TC_H_MAKE(TC_H_CLSACT, 0);
    message.TcMsg.tcm_parent = TC_H_CLSACT;
    message.TcMsg.tcm_info = 0;
    message.Kind.header.rta_len = sizeof(kind) + sizeof(rtattr);
    message.Kind.header.rta_type = TCA_KIND;
    std::copy(kind, kind + sizeof(kind), message.Kind.value);

    NetlinkChannel channel;
    auto transaction =
        channel.CreateTransaction(message, Add ? RTM_NEWQDISC : RTM_DELQDISC, NLM_F_REQUEST | (Add ? (NLM_F_CREATE | NLM_F_EXCL) : 0));
    transaction.Execute();
}

void Interface::BpfAttachTcClassifier(int ProgramFd, bool Ingress)
{
    char name[] = "gns";
    struct TcOptions
    {
        Attribute<int> Fd;
        Attribute<char[sizeof(name)]> Name;
        Attribute<int> Flags;
    } __attribute__((packed));
    char kind[] = "bpf";
    struct Message
    {
        tcmsg TcMsg;
        Attribute<char[sizeof(kind)]> Kind;
        Attribute<TcOptions> TcOptions;
    } __attribute__((packed));
    Message message{};

    message.TcMsg.tcm_family = AF_UNSPEC;
    message.TcMsg.tcm_ifindex = m_index;
    message.TcMsg.tcm_handle = 0;
    message.TcMsg.tcm_parent = TC_H_MAKE(TC_H_CLSACT, Ingress ? TC_H_MIN_INGRESS : TC_H_MIN_EGRESS);
    message.TcMsg.tcm_info = htons(ETH_P_ALL);
    message.Kind.header.rta_len = sizeof(kind) + sizeof(rtattr);
    message.Kind.header.rta_type = TCA_KIND;
    std::copy(kind, kind + sizeof(kind), message.Kind.value);
    message.TcOptions.header.rta_len = sizeof(TcOptions) + sizeof(rtattr);
    message.TcOptions.header.rta_type = TCA_OPTIONS;
    message.TcOptions.value.Fd.header.rta_len = sizeof(int) + sizeof(rtattr);
    message.TcOptions.value.Fd.header.rta_type = TCA_BPF_FD;
    message.TcOptions.value.Fd.value = ProgramFd;
    message.TcOptions.value.Name.header.rta_len = sizeof(name) + sizeof(rtattr);
    message.TcOptions.value.Name.header.rta_type = TCA_BPF_NAME;
    message.TcOptions.value.Flags.header.rta_len = sizeof(int) + sizeof(rtattr);
    message.TcOptions.value.Flags.header.rta_type = TCA_BPF_FLAGS;
    message.TcOptions.value.Flags.value = TCA_BPF_FLAG_ACT_DIRECT;
    std::copy(name, name + sizeof(name), message.TcOptions.value.Name.value);

    NetlinkChannel channel;
    auto transaction = channel.CreateTransaction(message, RTM_NEWTFILTER, NLM_F_REQUEST | NLM_F_EXCL | NLM_F_CREATE);
    transaction.Execute();
}
