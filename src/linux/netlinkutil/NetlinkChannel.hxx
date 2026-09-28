// Copyright (C) Microsoft Corporation. All rights reserved.
#pragma once
#include <sys/ioctl.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <unistd.h>
#include <string.h>
#include <atomic>

#include "NetlinkChannel.h"
#include "Syscall.h"

inline NetlinkChannel::NetlinkChannel(int socketType, int netlinkFamily, int groups)
{
    m_socket.reset(Syscall(::socket, AF_NETLINK, socketType, netlinkFamily));

    sockaddr_nl address = {};
    address.nl_family = AF_NETLINK;
    address.nl_groups = groups;

    Syscall(bind, m_socket.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address));
}

inline NetlinkChannel::NetlinkChannel(NetlinkChannel&& other)
{
    *this = std::move(other);
}

inline const NetlinkChannel& NetlinkChannel::operator=(NetlinkChannel&& other)
{
    m_socket = std::move(other.m_socket);

    return *this;
}

inline NetlinkChannel::NetlinkChannel(Tag)
{
}

inline NetlinkChannel::~NetlinkChannel()
{
}

inline NetlinkChannel NetlinkChannel::FromFd(int fd)
{
    assert(fd != -1);

    NetlinkChannel channel(Tag{});
    channel.m_socket.reset(fd);

    return channel;
}

inline NetlinkTransaction NetlinkChannel::CreateTransactionImpl(std::vector<char>&& message, int type, int flags)
{
    auto header = reinterpret_cast<nlmsghdr*>(message.data());
    header->nlmsg_len = message.size();
    header->nlmsg_type = type;
    header->nlmsg_seq = ++seqNumber;
    header->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK | flags;

    return {*this, std::move(message), header->nlmsg_seq};
}

inline void NetlinkChannel::SendMessage(const std::vector<char>& message)
{
    Syscall(sendto, m_socket.get(), message.data(), message.size(), 0, nullptr, 0);
}

inline NetlinkTransaction NetlinkChannel::CreateTransaction(int type, int flags)
{
    auto header = std::vector<char>(sizeof(nlmsghdr));
    return CreateTransactionImpl(std::move(header), type, flags);
}

template <typename T>
NetlinkTransaction NetlinkChannel::CreateTransaction(const T& message, int type, int flags)
{
    return CreateTransaction(&message, sizeof(T), type, flags);
}

inline NetlinkTransaction NetlinkChannel::CreateTransaction(const void* message, size_t messageSize, int type, int flags)
{
    struct Request
    {
        nlmsghdr header;
        char message;
    } __attribute__((packed));

    if (messageSize == 0)
    {
        return CreateTransaction(type, flags);
    }

    std::vector<char> buffer(offsetof(Request, message) + messageSize);
    const auto request = reinterpret_cast<Request*>(buffer.data());
    memcpy(&request->message, message, messageSize);
    return CreateTransactionImpl(std::move(buffer), type, flags);
}

inline NetlinkResponse NetlinkChannel::ReceiveNetlinkResponse()
{
    std::vector<char> buffer;

    sockaddr_storage src = {};
    iovec iov = {};

    msghdr message = {};
    message.msg_name = &src;
    message.msg_namelen = sizeof(src);
    message.msg_iov = &iov;
    message.msg_iovlen = 1;

    int size = Syscall(recvmsg, m_socket.get(), &message, MSG_PEEK | MSG_TRUNC);
    buffer.resize(size);

    size = Syscall(recvfrom, m_socket.get(), buffer.data(), buffer.size(), 0, nullptr, nullptr);
    if (size != static_cast<int>(buffer.size()))
    {
        throw RuntimeErrorWithSourceLocation(std::format("Unexpected response size: {} != {}", size, buffer.size()));
    }

    return {std::move(buffer)};
}

inline int NetlinkChannel::GetInterfaceIndex(const std::string& name)
{
    ifreq ifr = {};
    strncpy(ifr.ifr_name, name.c_str(), sizeof(ifr.ifr_name) - 1);
    Syscall(ioctl, m_socket.get(), SIOCGIFINDEX, &ifr);

    return ifr.ifr_ifindex;
}

inline int NetlinkChannel::SetInterfaceFlags(const std::string& name, int flags)
{
    ifreq ifr = {};
    strncpy(ifr.ifr_name, name.c_str(), sizeof(ifr.ifr_name) - 1);
    ifr.ifr_flags = flags;

    Syscall(ioctl, m_socket.get(), SIOCSIFFLAGS, &ifr);

    return ifr.ifr_flags;
}

inline int NetlinkChannel::GetInterfaceFlags(const std::string& name)
{
    ifreq ifr = {};
    strncpy(ifr.ifr_name, name.c_str(), sizeof(ifr.ifr_name) - 1);

    Syscall(ioctl, m_socket.get(), SIOCGIFFLAGS, &ifr);

    return ifr.ifr_flags;
}
