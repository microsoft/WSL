// Copyright (C) Microsoft Corporation. All rights reserved.
#pragma once

#include "NetlinkResponse.h"
#include "NetlinkTransaction.h"
#include <sys/socket.h>
#include "lxwil.h"

class NetlinkChannel
{
public:
    NetlinkChannel(int socketType = SOCK_RAW, int netlinkFamily = NETLINK_ROUTE, int groups = 0);
    ~NetlinkChannel();

    NetlinkChannel(const NetlinkChannel&) = delete;
    NetlinkChannel(NetlinkChannel&& other);

    const NetlinkChannel& operator=(const NetlinkChannel&) = delete;
    const NetlinkChannel& operator=(NetlinkChannel&& other);

    template <typename TRequest>
    NetlinkTransaction CreateTransaction(const TRequest& message, int type, int flags);

    NetlinkTransaction CreateTransaction(const void* message, size_t messageSize, int type, int flags);

    NetlinkTransaction CreateTransaction(int type, int flags);

    void SendMessage(const std::vector<char>& message);

    NetlinkResponse ReceiveNetlinkResponse();

    int GetInterfaceIndex(const std::string& name);

    int GetInterfaceFlags(const std::string& name);

    int SetInterfaceFlags(const std::string& name, int flags);

    static NetlinkChannel FromFd(int fd);

private:
    struct Tag
    {
    };

    NetlinkChannel(Tag);

    NetlinkTransaction CreateTransactionImpl(std::vector<char>&& message, int type, int flags);

    wil::unique_fd m_socket;

    std::atomic<int> seqNumber;
};

#include "NetlinkChannel.hxx"
