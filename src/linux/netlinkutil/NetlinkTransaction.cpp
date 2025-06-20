// Copyright (C) Microsoft Corporation. All rights reserved.
#include "NetlinkChannel.h"
#include "NetlinkTransaction.h"
#include "NetlinkTransactionError.h"

NetlinkTransaction::NetlinkTransaction(NetlinkChannel& channel, std::vector<char>&& request, __u32 seq) :
    m_channel(channel), m_request(std::move(request)), m_seq(seq)
{
}

void NetlinkTransaction::Execute(const std::function<void(const NetlinkResponse&)>& routine)
{
    m_channel.SendMessage(m_request);

    std::vector<NetlinkResponse> responses;

    try
    {
        do
        {
            NetlinkResponse response = m_channel.ReceiveNetlinkResponse();
            if (response.Sequence() != m_seq)
            {
                response.ThrowIfErrorFound();
                continue;
            }
            responses.emplace_back(std::move(response));
            responses.back().ThrowIfErrorFound();

            routine(responses.back());
        } while (responses.empty() || (responses.back().MultiMessage() && !responses.back().Done()));
    }
    catch (const std::exception& e)
    {
        throw NetlinkTransactionError(m_request, responses, e);
    }
}

void NetlinkTransaction::PrintRequest() const
{
    throw NetlinkTransactionError(m_request, {}, RuntimeErrorWithSourceLocation("Print netlink transaction request"));
}

std::string NetlinkTransaction::GetRawRequestString() const
{
    std::stringstream str;
    utils::FormatBinary(str, m_request.data(), m_request.size());
    return str.str();
}
