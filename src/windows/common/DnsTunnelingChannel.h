// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include <wil/resource.h>
#include "lxinitshared.h"
#include "SocketChannel.h"

namespace wsl::core::networking {

using DnsTunnelingCallback = std::function<void(const gsl::span<gsl::byte>, const LX_GNS_DNS_CLIENT_IDENTIFIER&)>;

class DnsTunnelingChannel
{
public:
    DnsTunnelingChannel(wil::unique_socket&& socket, DnsTunnelingCallback&& reportDnsRequest);
    ~DnsTunnelingChannel();

    DnsTunnelingChannel(const DnsTunnelingChannel&) = delete;
    DnsTunnelingChannel& operator=(const DnsTunnelingChannel&) = delete;

    DnsTunnelingChannel(DnsTunnelingChannel&&) = delete;
    DnsTunnelingChannel& operator=(DnsTunnelingChannel&&) = delete;

    // Construct and send a LX_GNS_DNS_TUNNELING_MESSAGE message on the channel.
    // Note: Callers are responsible for sequencing calls to this method.
    //
    // Arguments:
    // dnsBuffer - buffer containing DNS response.
    // dnsClientIdentifier - struct containing protocol (TCP/UDP) and unique id of the Linux DNS client making the request.
    void SendDnsMessage(const gsl::span<gsl::byte> dnsBuffer, const LX_GNS_DNS_CLIENT_IDENTIFIER& dnsClientIdentifier) noexcept;

    // Stop the channel.
    void Stop() noexcept;

private:
    // Wait for messages on the channel from Linux side.
    void ReceiveLoop() noexcept;

    wil::unique_event m_stopEvent{wil::EventOptions::ManualReset};

    wsl::shared::SocketChannel m_channel;

    std::thread m_receiveWorkerThread;

    // Callback used to notify when there is a new DNS request message on the channel.
    DnsTunnelingCallback m_reportDnsRequest;
};

} // namespace wsl::core::networking
