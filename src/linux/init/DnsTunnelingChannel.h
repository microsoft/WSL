// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include "common.h"
#include "lxinitshared.h"
#include "SocketChannel.h"

using DnsTunnelingCallback = std::function<void(const gsl::span<gsl::byte>, const LX_GNS_DNS_CLIENT_IDENTIFIER&)>;

class DnsTunnelingChannel
{
public:
    DnsTunnelingChannel(int channelFd, DnsTunnelingCallback&& reportDnsResponse);
    ~DnsTunnelingChannel();

    DnsTunnelingChannel(const DnsTunnelingChannel&) = delete;
    DnsTunnelingChannel(DnsTunnelingChannel&&) = delete;
    DnsTunnelingChannel& operator=(const DnsTunnelingChannel&) = delete;
    DnsTunnelingChannel& operator=(DnsTunnelingChannel&&) = delete;

    // Construct and send a LX_GNS_DNS_TUNNELING_MESSAGE message on the channel.
    // Note: Callers are responsible for sequencing calls to this method.
    //
    // Arguments:
    // dnsBuffer - buffer containing DNS request.
    // dnsClientIdentifier - struct containing protocol (TCP/UDP) and unique id of the Linux DNS client making the request.
    void SendDnsMessage(const gsl::span<gsl::byte> dnsBuffer, const LX_GNS_DNS_CLIENT_IDENTIFIER& dnsClientIdentifier) noexcept;

    // Stop the channel.
    void Stop() noexcept;

private:
    // Wait for messages on the channel from Windows side.
    void ReceiveLoop() noexcept;

    wsl::shared::SocketChannel m_channel;

    // Thread running the receive loop.
    std::thread m_receiveWorkerThread;

    // Pipe used to stop m_receiveWorkerThread.
    wil::unique_pipe m_shutdownReceiveWorkerPipe;

    // Callback used to notify when there is a new DNS response message on the channel.
    const DnsTunnelingCallback m_reportDnsResponse;
};
