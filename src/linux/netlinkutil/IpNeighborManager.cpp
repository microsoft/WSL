// Copyright (C) Microsoft Corporation. All rights reserved.

#include <linux/if_packet.h>
#include <linux/if_ether.h>
#include <chrono>
#include <arpa/inet.h>
#include <poll.h>
#include "lxwil.h"
#include "RuntimeErrorWithSourceLocation.h"
#include "IpNeighborManager.h"
#include "NetlinkTransactionError.h"
#include "Utils.h"

#define ARPHRD_ETHER 1
#define ARPOP_REQUEST 1
#define ARPOP_REPLY 2

const MacAddress BroadcastMac{0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

template <size_t TProtocolAddressLength>
struct _arp_packet_header
{
    using IPAddress = std::array<uint8_t, TProtocolAddressLength>;
    MacAddress Destination;
    MacAddress Source;
    uint16_t EthernetType;
    uint16_t HardwareType;
    uint16_t ProtocolType;
    uint8_t HardwareAddressLength;
    uint8_t ProtocolAddressLength;
    uint16_t Operation;
    MacAddress SenderHardwareAddress;
    IPAddress SenderIpAddress;
    MacAddress TargetHardwareAddress;
    IPAddress TargetIpAddress;
} __attribute__((packed));

using arp_packet_ipv4_t = _arp_packet_header<4>;
using arp_packet_ipv6_t = _arp_packet_header<16>;

template <typename T>
void ComposeArpRequest(T& ArpRequest, uint16_t ProtocolType, const Neighbor& Source, const Neighbor& Target)
{
    // Ethernet header
    ArpRequest.Destination = BroadcastMac;
    ArpRequest.Source = Source.macAddress;
    ArpRequest.EthernetType = htons(ETH_P_ARP);
    ArpRequest.HardwareType = htons(ARPHRD_ETHER);
    ArpRequest.ProtocolType = htons(ProtocolType);
    ArpRequest.HardwareAddressLength = sizeof(ArpRequest.SenderHardwareAddress);
    ArpRequest.ProtocolAddressLength = sizeof(ArpRequest.SenderIpAddress);
    ArpRequest.Operation = htons(ARPOP_REQUEST);
    ArpRequest.SenderHardwareAddress = Source.macAddress;
    ArpRequest.TargetHardwareAddress.fill(0);
    Source.ipAddress.ConvertToBytes(ArpRequest.SenderIpAddress.data());
    Target.ipAddress.ConvertToBytes(ArpRequest.TargetIpAddress.data());
}

template <typename T>
bool ParseArpReply(const T& ArpReply, uint16_t ProtocolType, const Neighbor& Source, Neighbor& Target)
{
    typename T::IPAddress SourceIp;
    typename T::IPAddress TargetIp;
    Source.ipAddress.ConvertToBytes(SourceIp.data());
    Target.ipAddress.ConvertToBytes(TargetIp.data());

    if (ArpReply.Destination != Source.macAddress)
        return false;
    if (ArpReply.EthernetType != htons(ETH_P_ARP))
        return false;
    if (ArpReply.HardwareType != htons(ARPHRD_ETHER))
        return false;
    if (ArpReply.ProtocolType != htons(ProtocolType))
        return false;
    if (ArpReply.HardwareAddressLength != sizeof(ArpReply.SenderHardwareAddress))
        return false;
    if (ArpReply.ProtocolAddressLength != sizeof(ArpReply.SenderIpAddress))
        return false;
    if (ArpReply.Operation != htons(ARPOP_REPLY))
        return false;
    if (ArpReply.TargetHardwareAddress != Source.macAddress)
        return false;
    if (ArpReply.TargetIpAddress != SourceIp)
        return false;

    Target.macAddress = ArpReply.SenderHardwareAddress;
    return true;
}

bool IpNeighborManager::PerformNeighborDiscovery(Neighbor& Local, Neighbor& Neighbor)
{
    sockaddr_ll address{};
    wil::unique_fd packet_socket = Syscall(socket, AF_PACKET, SOCK_RAW | SOCK_NONBLOCK, htons(ETH_P_ALL));
    address.sll_family = AF_PACKET;
    address.sll_protocol = htons(ETH_P_ALL);
    address.sll_ifindex = Neighbor.dev;
    Syscall(bind, packet_socket.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address));

    union
    {
        arp_packet_ipv4_t IPv4;
        arp_packet_ipv6_t IPv6;
    } ArpRequest, ArpReply;
    size_t ArpPacketSize = Local.getFamily() == AF_INET ? sizeof(arp_packet_ipv4_t) : sizeof(arp_packet_ipv6_t);

    if (Local.getFamily() == AF_INET)
    {
        ComposeArpRequest(ArpRequest.IPv4, ETH_P_IP, Local, Neighbor);
    }
    else
    {
        ComposeArpRequest(ArpRequest.IPv6, ETH_P_IPV6, Local, Neighbor);
    }

    auto wait_for_read = [](int fd, std::chrono::milliseconds timeout_ms) -> bool {
        short poll_events = POLLIN | POLLPRI;
        struct pollfd pollfds[1];
        pollfds[0] = {.fd = fd, .events = poll_events, .revents = 0};

        int return_value = Syscall(poll, pollfds, 1, timeout_ms.count());
        return (return_value == 1) && (pollfds[0].revents == POLLIN);
    };

    for (size_t retry = 0; retry < 5; retry++)
    {
        auto expiry = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        Syscall(write, packet_socket.get(), &ArpRequest, ArpPacketSize);

        while (std::chrono::steady_clock::now() < expiry)
        {
            if (!wait_for_read(packet_socket.get(), std::chrono::duration_cast<std::chrono::milliseconds>(expiry - std::chrono::steady_clock::now())))
                continue;
            int bytes_read = Syscall(read, packet_socket.get(), &ArpReply, ArpPacketSize);
            if (bytes_read != ArpPacketSize)
            {
                continue;
            }
            if (Local.getFamily() == AF_INET)
            {
                if (ParseArpReply(ArpReply.IPv4, ETH_P_IP, Local, Neighbor))
                    return true;
            }
            else
            {
                if (ParseArpReply(ArpReply.IPv6, ETH_P_IPV6, Local, Neighbor))
                    return true;
            }
        }
    }
    return false;
}

void IpNeighborManager::ModifyNeighborEntry(const Neighbor& Neighbor, Operation operation)
{
    assert(operation == Operation::Create || operation == Operation::Update || operation == Operation::Remove);

    // In case of Remove, there are no additional flags needed besides NLM_F_REQUEST | NLM_F_ACK.
    int flags = 0;
    if (operation == Update)
    {
        flags = NLM_F_CREATE | NLM_F_REPLACE;
    }
    else if (operation == Create)
    {
        flags = NLM_F_CREATE;
    }

    int netlinkOperation = operation == Remove ? RTM_DELNEIGH : RTM_NEWNEIGH;
    if (Neighbor.getFamily() == AF_INET)
    {
        ModifyNeighborEntryImpl<in_addr>(Neighbor, netlinkOperation, flags);
    }
    else
    {
        ModifyNeighborEntryImpl<in6_addr>(Neighbor, netlinkOperation, flags);
    }
}

template <typename T>
void IpNeighborManager::SendMessage(const Neighbor& Neighbor, int operation, int flags, const std::function<void(T&)>& routine)
{
    T message{};
    message.header.ndm_family = Neighbor.getFamily();
    message.header.ndm_ifindex = Neighbor.dev;
    message.header.ndm_state = NUD_PERMANENT;
    message.header.ndm_type = RTN_UNICAST;

    routine(message);

    auto transaction = m_channel.CreateTransaction(message, operation, flags);
    try
    {
        transaction.Execute();
    }
    catch (const NetlinkTransactionError& transactionErr)
    {
        auto errorCode = transactionErr.Error();
        if (errorCode.has_value())
        {
            // Errors "file exists" and "file not found" are ignored in order to avoid keeping
            // track in GnsDaemon of what neighbour entries were added/deleted and allow the same entry
            // to be added/deleted multiple times.
            if (errorCode.value() == -EEXIST || errorCode.value() == -ENOENT)
            {
                return;
            }
        }

        throw;
    }
}

template <typename TAddr>
void IpNeighborManager::ModifyNeighborEntryImpl(const Neighbor& Neighbor, int operation, int flags)
{
    struct Message
    {
        ndmsg header;
        utils::AddressAttribute<TAddr> ip;
        utils::MacAddressAttribute mac;
    } __attribute__((packed));

    SendMessage<Message>(Neighbor, operation, flags, [&](Message& message) {
        utils::InitializeAddressAttribute<TAddr>(message.ip, Neighbor.ipAddress, NDA_DST);

        message.mac.header.nla_len = sizeof(message.mac);
        message.mac.header.nla_type = NDA_LLADDR;
        message.mac.address = Neighbor.macAddress;
    });
}
