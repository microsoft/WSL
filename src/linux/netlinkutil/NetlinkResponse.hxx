// Copyright (C) Microsoft Corporation. All rights reserved.
#pragma once

#include "NetlinkResponse.h"
#include "NetlinkParseException.h"

template <typename T>
std::vector<NetlinkMessage<T>> NetlinkResponse::Messages(int type) const
{
    std::vector<NetlinkMessage<T>> messages;

    size_t size = m_data.size();
    for (const auto* header = reinterpret_cast<const nlmsghdr*>(m_data.data()); NLMSG_OK(header, size); header = NLMSG_NEXT(header, size))
    {
        if (header->nlmsg_type == type)
        {
            auto begin = m_data.begin() + (reinterpret_cast<const char*>(header) - m_data.data());
            auto end = begin + header->nlmsg_len;
            messages.emplace_back(*this, m_data.begin(), begin, end);
        }
    }

    if (size != 0)
    {
        throw NetlinkParseException(*this, std::format("Netlink message is truncated. Missing bytes: {}", size));
    }

    return messages;
}
