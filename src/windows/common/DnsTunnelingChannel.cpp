// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "DnsTunnelingChannel.h"

using wsl::core::networking::DnsTunnelingChannel;

DnsTunnelingChannel::DnsTunnelingChannel(wil::unique_socket&& socket, DnsTunnelingCallback&& reportDnsRequest) :
    m_channel{std::move(socket), "DnsTunneling", m_stopEvent.get()}, m_reportDnsRequest(std::move(reportDnsRequest))
{
    WSL_LOG("DnsTunnelingChannel::DnsTunnelingChannel [Windows]", TraceLoggingValue(m_channel.Socket(), "socket"));

    // Start thread waiting for incoming messages from Linux side
    m_receiveWorkerThread = std::thread([this]() { ReceiveLoop(); });
}

DnsTunnelingChannel::~DnsTunnelingChannel()
{
    Stop();
}

void DnsTunnelingChannel::SendDnsMessage(const gsl::span<gsl::byte> dnsBuffer, const LX_GNS_DNS_CLIENT_IDENTIFIER& dnsClientIdentifier) noexcept
try
{
    // Exit if channel was stopped
    if (m_stopEvent.is_signaled())
    {
        return;
    }

    wsl::shared::MessageWriter<LX_GNS_DNS_TUNNELING_MESSAGE> message(LxGnsMessageDnsTunneling);
    message->DnsClientIdentifier = dnsClientIdentifier;
    message.WriteSpan(dnsBuffer);

    m_channel.SendMessage<LX_GNS_DNS_TUNNELING_MESSAGE>(message.Span());
}
CATCH_LOG()

void DnsTunnelingChannel::ReceiveLoop() noexcept
{
    std::vector<gsl::byte> receiveBuffer;

    for (;;)
    {
        try
        {
            if (m_stopEvent.is_signaled())
            {
                return;
            }

            WSL_LOG_DEBUG("DnsTunnelingChannel::ReceiveLoop [Windows] - waiting for next message from Linux");

            // Read next message. wsl::shared::socket::RecvMessage() first reads the message header, then uses it to determine the
            // total size of the message and read the rest of the message, resizing the buffer if needed.
            auto [message, span] = m_channel.ReceiveMessageOrClosed<MESSAGE_HEADER>();
            if (message == nullptr)
            {
                WSL_LOG("DnsTunnelingChannel::ReceiveLoop [Windows] - failed to read message");
                return;
            }

            // Get the message type from the message header
            switch (message->MessageType)
            {
            case LxGnsMessageDnsTunneling:
            {
                // Cast message to a LX_GNS_DNS_TUNNELING_MESSAGE struct
                auto* dnsMessage = gslhelpers::try_get_struct<LX_GNS_DNS_TUNNELING_MESSAGE>(span);
                if (!dnsMessage)
                {
                    WSL_LOG(
                        "DnsTunnelingChannel::ReceiveLoop [Windows] - failed to convert message to LX_GNS_DNS_TUNNELING_MESSAGE");
                    return;
                }

                // Extract DNS buffer from message
                auto dnsBuffer = span.subspan(offsetof(LX_GNS_DNS_TUNNELING_MESSAGE, Buffer));

                WSL_LOG_DEBUG(
                    "DnsTunnelingChannel::ReceiveLoop [Windows] - received DNS message",
                    TraceLoggingValue(dnsBuffer.size(), "DNS buffer size"),
                    TraceLoggingValue(dnsMessage->DnsClientIdentifier.Protocol == IPPROTO_UDP ? "UDP" : "TCP", "Protocol"),
                    TraceLoggingValue(dnsMessage->DnsClientIdentifier.DnsClientId, "DNS client id"));

                // Invoke callback to notify about the new DNS request
                m_reportDnsRequest(dnsBuffer, dnsMessage->DnsClientIdentifier);

                break;
            }

            default:
            {
                THROW_HR_MSG(E_UNEXPECTED, "Unexpected LX_MESSAGE_TYPE : %i", message->MessageType);
            }
            }
        }
        CATCH_LOG()
    }
}

void DnsTunnelingChannel::Stop() noexcept
try
{
    WSL_LOG("DnsTunnelingChannel::Stop [Windows]");

    m_stopEvent.SetEvent();

    // Stop receive loop
    if (m_receiveWorkerThread.joinable())
    {
        m_receiveWorkerThread.join();
    }
}
CATCH_LOG()
