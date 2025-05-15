// Copyright (C) Microsoft Corporation. All rights reserved.
#pragma once

#include <vector>
#include <optional>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include "Fwd.h"

template <typename TMessage>
class NetlinkMessage
{
public:
    using Titerator = std::vector<char>::const_iterator;

    NetlinkMessage(const NetlinkResponse& response, Titerator responseBegin, Titerator begin, Titerator end);

    const TMessage* Payload() const;

    const nlmsghdr* Header() const;

    template <typename TAttribute>
    std::vector<const TAttribute*> Attributes(int type) const;

    template <typename TAttribute>
    std::optional<const TAttribute*> UniqueAttribute(int type) const;

private:
    const rtattr* FirstAttribute() const;

    const NetlinkResponse& m_response;
    Titerator m_responseBegin;
    Titerator m_begin;
    Titerator m_end;
};

#include "NetlinkMessage.hxx"
