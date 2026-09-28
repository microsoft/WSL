// Copyright (C) Microsoft Corporation. All rights reserved.
#pragma once

#include <functional>
#include <sys/types.h>
#include "Fwd.h"

class NetlinkTransaction
{
public:
    NetlinkTransaction(NetlinkChannel& channel, std::vector<char>&& request, __u32 seq);

    void Execute(const std::function<void(const NetlinkResponse&)>& routine = [](const auto&) {});

    // Useful for debugging how netlink requests are composed
    void PrintRequest() const;
    std::string GetRawRequestString() const;

private:
    NetlinkChannel& m_channel;
    std::vector<char> m_request;
    __u32 m_seq;
};
