// Copyright (C) Microsoft Corporation. All rights reserved.

#include <iostream>
#include <filesystem>
#include <fstream>
#include <netinet/in.h>
#include "common.h"
#include "DnsTunnelingManager.h"

DnsTunnelingManager::DnsTunnelingManager(int hvsocketFd, const std::string& dnsTunnelingIpAddress) :
    m_dnsChannel(
        hvsocketFd,
        [this](const gsl::span<gsl::byte> dnsBuffer, const LX_GNS_DNS_CLIENT_IDENTIFIER& dnsClientIdentifier) {
            m_dnsServer.HandleDnsResponse(dnsBuffer, dnsClientIdentifier);
        }),
    m_dnsServer([this](const gsl::span<gsl::byte> dnsBuffer, const LX_GNS_DNS_CLIENT_IDENTIFIER& dnsClientIdentifier) {
        if (m_stopped)
        {
            return;
        }

        m_dnsChannel.SendDnsMessage(dnsBuffer, dnsClientIdentifier);
    })
{
    GNS_LOG_INFO("Using DNS server IP {}", dnsTunnelingIpAddress.c_str());

    // Start DNS server used for tunneling. Server has both TCP and UDP support.
    //
    // Note: because DnsTunnelingManager runs as part of GNS daemon, which is started before GnsPortTracker, binding the DNS
    // server will not be intercepted by the bind seccomp hook. This is ok because in FSE mode there is no need for host<->guest
    // loopback communication to/from the DNS server (all traffic to/from DNS server will stay in the container).
    m_dnsServer.Start(dnsTunnelingIpAddress);
}

DnsTunnelingManager::~DnsTunnelingManager()
{
    // Scoped m_dnsLock
    {
        // Set flag to signal object is stopping
        m_stopped = true;
    }

    // Stop channel first as it can call into the DNS server object
    m_dnsChannel.Stop();

    // Stop DNS server
    m_dnsServer.Stop();
}
