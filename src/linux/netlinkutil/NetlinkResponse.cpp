// Copyright (C) Microsoft Corporation. All rights reserved.
#include "NetlinkResponse.h"
#include "NetlinkError.h"
#include "Utils.h"
#include <errno.h>

NetlinkResponse::NetlinkResponse(std::vector<char>&& content) : m_data(std::move(content))
{
}

void NetlinkResponse::ThrowIfErrorFound(bool dump) const
{
    auto results = Messages<nlmsgerr>(NLMSG_ERROR);

    for (const auto& e : results)
    {
        int result = e.Payload()->error;

        if (result != 0)
        {
            throw NetlinkError(result);
        }
    }

    if (dump)
    {
        size_t size = m_data.size();
        for (const auto* header = reinterpret_cast<const nlmsghdr*>(m_data.data()); NLMSG_OK(header, size); header = NLMSG_NEXT(header, size))
        {
            if (header->nlmsg_flags & NLM_F_DUMP_INTR)
            {
                throw NetlinkError(-EINTR);
            }
        }

        for (const auto& message : Messages<int>(NLMSG_DONE))
        {
            const int result = *message.Payload();
            if (result != 0)
            {
                throw NetlinkError(result);
            }
        }
    }
}

__u32 NetlinkResponse::Sequence() const
{
    return reinterpret_cast<const nlmsghdr*>(m_data.data())->nlmsg_seq;
}

bool NetlinkResponse::MultiMessage() const
{
    size_t size = m_data.size();
    for (const auto* header = reinterpret_cast<const nlmsghdr*>(m_data.data()); NLMSG_OK(header, size); header = NLMSG_NEXT(header, size))
    {
        if (header->nlmsg_flags & NLM_F_MULTI)
        {
            return true;
        }
    }

    return false;
}

NetlinkResponse::Titerator NetlinkResponse::Begin() const
{
    return m_data.begin();
}

NetlinkResponse::Titerator NetlinkResponse::End() const
{
    return m_data.end();
}

bool NetlinkResponse::Done() const
{
    auto doneMessages = Messages<nlmsghdr>(NLMSG_DONE);

    return !doneMessages.empty();
}

std::ostream& operator<<(std::ostream& out, const NetlinkResponse& response)
{
    const auto begin = &*response.Begin();
    const auto size = response.End() - response.Begin();

    return utils::FormatBinary(out, begin, size);
}
