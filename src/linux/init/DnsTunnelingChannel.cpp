// Copyright (C) Microsoft Corporation. All rights reserved.

#include <netinet/in.h>
#include <arpa/inet.h>
#include "DnsTunnelingChannel.h"
#include "util.h"
#include "RuntimeErrorWithSourceLocation.h"
#include "Syscall.h"
#include "message.h"

DnsTunnelingChannel::DnsTunnelingChannel(int channelFd, DnsTunnelingCallback&& reportDnsResponse) :
    m_channel(wil::unique_fd{channelFd}, "DnsTunneling"), m_reportDnsResponse(std::move(reportDnsResponse))
{
    // Create a pipe to be used for signalling the receive loop to stop
    m_shutdownReceiveWorkerPipe = wil::unique_pipe::create(0);

    // Start loop waiting for incoming messages from Windows side
    m_receiveWorkerThread = std::thread([this]() { ReceiveLoop(); });
}

DnsTunnelingChannel::~DnsTunnelingChannel()
{
    Stop();
}

void DnsTunnelingChannel::SendDnsMessage(const gsl::span<gsl::byte> dnsBuffer, const LX_GNS_DNS_CLIENT_IDENTIFIER& dnsClientIdentifier) noexcept
try
{
    wsl::shared::MessageWriter<LX_GNS_DNS_TUNNELING_MESSAGE> message(LxGnsMessageDnsTunneling);
    message->DnsClientIdentifier = dnsClientIdentifier;
    message.WriteSpan(dnsBuffer);

    m_channel.SendMessage<LX_GNS_DNS_TUNNELING_MESSAGE>(message.Span());
}
CATCH_LOG()

void DnsTunnelingChannel::ReceiveLoop() noexcept
{
    UtilSetThreadName("DnsTunneling");

    // Returns false if the write pipe was closed, signaling that loop should exit
    // Returns true if there is data to be received on the channel fd
    auto wait_for_channel_fd = [this]() -> bool {
        struct pollfd poll_fds[2];
        poll_fds[0] = {.fd = m_channel.Socket(), .events = POLLIN, .revents = 0};
        poll_fds[1] = {.fd = m_shutdownReceiveWorkerPipe.read().get(), .events = POLLIN, .revents = 0};

        unsigned int retryCount = 0;
        const unsigned int maxRetryCount = 3;

        for (;;)
        {
            int return_value = SyscallInterruptable(poll, poll_fds, ARRAY_SIZE(poll_fds), -1);
            if (return_value < 0)
            {
                GNS_LOG_ERROR("poll failed");
                retryCount++;

                if (retryCount < maxRetryCount)
                {
                    continue;
                }
                else
                {
                    return false;
                }
            }
            else if (return_value == 0)
            {
                GNS_LOG_ERROR("poll returned 0 (timeout)");
                return false;
            }
            else if (poll_fds[1].revents)
            {
                return false;
            }
            else if (poll_fds[0].revents & POLLIN)
            {
                return true;
            }
        }
    };

    std::vector<gsl::byte> receiveBuffer;

    for (;;)
    {
        try
        {
            if (!wait_for_channel_fd())
            {
                break;
            }

            GNS_LOG_INFO("processing next message from Windows");

            // Read next message. wsl::shared::socket::RecvMessage() first reads the message header, then uses it to determine the
            // total size of the message and read the rest of the message, resizing the buffer if needed.
            auto [message, span] = m_channel.ReceiveMessageOrClosed<MESSAGE_HEADER>();
            if (message == nullptr)
            {
                GNS_LOG_ERROR("failed to read message");
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
                    GNS_LOG_ERROR("failed to convert message to LX_GNS_DNS_TUNNELING_MESSAGE");
                    return;
                }

                // Extract DNS buffer from message
                auto dnsBuffer = span.subspan(offsetof(LX_GNS_DNS_TUNNELING_MESSAGE, Buffer));

                GNS_LOG_INFO(
                    "received DNS message DNS buffer size: {}, Protocol {}, DNS client id: {}",
                    dnsBuffer.size(),
                    dnsMessage->DnsClientIdentifier.Protocol == IPPROTO_UDP ? "UDP" : "TCP",
                    dnsMessage->DnsClientIdentifier.DnsClientId);

                // Invoke callback to notify about the new DNS response
                m_reportDnsResponse(dnsBuffer, dnsMessage->DnsClientIdentifier);

                break;
            }

            default:
            {
                throw RuntimeErrorWithSourceLocation(std::format("Unexpected LX_MESSAGE_TYPE : {}", static_cast<int>(message->MessageType)));
            }
            }
        }
        CATCH_LOG()
    }
}

void DnsTunnelingChannel::Stop() noexcept
try
{
    GNS_LOG_INFO("stopping DNS server");

    // Stop receive loop by closing the write fd of the pipe
    m_shutdownReceiveWorkerPipe.write().reset();

    if (m_receiveWorkerThread.joinable())
    {
        m_receiveWorkerThread.join();
    }
}
CATCH_LOG()
