// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include "DnsTunnelingChannel.h"
#include "DnsServer.h"

class DnsTunnelingManager
{
public:
    DnsTunnelingManager(int hvsocketFd, const std::string& dnsTunnelingIpAddress);
    ~DnsTunnelingManager();

    DnsTunnelingManager(const DnsTunnelingManager&) = delete;
    DnsTunnelingManager(DnsTunnelingManager&&) = delete;
    DnsTunnelingManager& operator=(const DnsTunnelingManager&) = delete;
    DnsTunnelingManager& operator=(DnsTunnelingManager&&) = delete;

private:
    // Hvsocket channel used to communicate with the host.
    DnsTunnelingChannel m_dnsChannel;

    // DNS server used for tunneling, supporting both UDP and TCP.
    DnsServer m_dnsServer;

    std::atomic<bool> m_stopped = false;
};
