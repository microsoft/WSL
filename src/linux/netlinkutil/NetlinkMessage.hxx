// Copyright (C) Microsoft Corporation. All rights reserved.
#pragma once

#include <format>
#include <string>
#include <linux/if_link.h>

#include "NetlinkMessage.h"
#include "NetlinkParseException.h"

template <typename TMessage>
NetlinkMessage<TMessage>::NetlinkMessage(const NetlinkResponse& response, Titerator responseBegin, Titerator begin, Titerator end) :
    m_response(response), m_responseBegin(responseBegin), m_begin(begin), m_end(end)
{
}

template <typename TMessage>
const TMessage* NetlinkMessage<TMessage>::Payload() const
{
    const auto* data = reinterpret_cast<const char*>(NLMSG_DATA(&*m_begin));
    if (data + sizeof(TMessage) > &*m_end)
    {
        throw NetlinkParseException(
            m_response,
            std::format(
                "Message at offset {}: attempted to access beyond message offset ({} > {})",
                (m_begin - m_responseBegin),
                sizeof(TMessage),
                (m_end - m_begin)));
    }

    return reinterpret_cast<const TMessage*>(NLMSG_DATA(&*m_begin));
}

template <>
inline const rtattr* NetlinkMessage<rtmsg>::FirstAttribute() const
{
    return RTM_RTA(NLMSG_DATA(&*m_begin));
}

template <>
inline const rtattr* NetlinkMessage<ifaddrmsg>::FirstAttribute() const
{
    return IFA_RTA(NLMSG_DATA(&*m_begin));
}

template <typename TAttribute>
const rtattr* NetlinkMessage<TAttribute>::FirstAttribute() const
{
    throw RuntimeErrorWithSourceLocation("Tried listing attributes for a message without attributes");
}

template <typename TMessage>
template <typename TAttribute>
std::vector<const TAttribute*> NetlinkMessage<TMessage>::Attributes(int type) const
{
    std::vector<const TAttribute*> attributes;

    auto len = NLMSG_PAYLOAD(Header(), sizeof(TMessage));
    for (const rtattr* e = FirstAttribute(); RTA_OK(e, len); e = RTA_NEXT(e, len))
    {
        if (e->rta_type == type)
        {
            const auto* ptr = reinterpret_cast<const TAttribute*>(RTA_DATA(e));

            if (sizeof(TAttribute) > e->rta_len)
            {
                throw NetlinkParseException(
                    m_response,
                    std::format(
                        "Attribute at offset {}: attempted to access beyond attribute offset ({} > {})",
                        (reinterpret_cast<const char*>(e) - &*m_responseBegin),
                        sizeof(TMessage),
                        e->rta_len));
            }

            attributes.push_back(ptr);
        }
    }

    return attributes;
}

template <typename TMessage>
template <typename TAttribute>
std::optional<const TAttribute*> NetlinkMessage<TMessage>::UniqueAttribute(int type) const
{
    auto attributes = Attributes<TAttribute>(type);

    if (attributes.empty())
    {
        return {};
    }
    else if (attributes.size() == 1)
    {
        return attributes[0];
    }

    throw RuntimeErrorWithSourceLocation(std::format("Unexpected attribute count: {} for attribute type: {}", attributes.size(), type));
}

template <typename TMessage>
const nlmsghdr* NetlinkMessage<TMessage>::Header() const
{
    return reinterpret_cast<const nlmsghdr*>(&*m_begin);
}
