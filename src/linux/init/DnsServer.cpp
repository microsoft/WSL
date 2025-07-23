// Copyright (C) Microsoft Corporation. All rights reserved.

#include <arpa/inet.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include "DnsServer.h"
#include "RuntimeErrorWithSourceLocation.h"
#include "Syscall.h"
#include "util.h"

// Port used by DNS server
constexpr int c_dnsServerPort = 53;
// Max number of events to be returned by epoll_wait()
constexpr int c_epollWaitMaxEvents = 100;
// Maximum size of DNS over UDP requests is 4096 bytes (max size is reached for EDNS UDP requests)
constexpr int c_maxUdpDnsBufferSize = 4096;
// Max number of pending connections in the TCP listen queue
constexpr int c_maxListenBacklog = 1000;

DnsServer::DnsServer(DnsTunnelingCallback&& tunnelDnsRequest) : m_tunnelDnsRequest(std::move(tunnelDnsRequest))
{
}

DnsServer::~DnsServer() noexcept
{
    Stop();
}

void DnsServer::Start(const std::string& ipAddress) noexcept
try
{
    // Create epoll handler fd. 0 represents default flags
    m_epollFd = Syscall(epoll_create1, 0);

    StartUdpDnsServer(ipAddress);
    StartTcpDnsServer(ipAddress);

    // Create and register the shutdown pipe with epoll
    m_shutdownServerLoopPipe = wil::unique_pipe::create(0);

    epoll_event event{};
    event.events = EPOLLIN;
    event.data.fd = m_shutdownServerLoopPipe.read().get();
    Syscall(epoll_ctl, m_epollFd.get(), EPOLL_CTL_ADD, m_shutdownServerLoopPipe.read().get(), &event);

    // Start server loop
    m_serverThread = std::thread([this]() { ServerLoop(); });
}
CATCH_LOG()

void DnsServer::StartUdpDnsServer(const std::string& ipAddress) noexcept
try
{
    sockaddr_in serverAddr{};

    serverAddr.sin_family = AF_INET;
    Syscall(inet_pton, AF_INET, ipAddress.c_str(), &serverAddr.sin_addr);
    serverAddr.sin_port = htons(c_dnsServerPort);

    // Create IPv4 UDP socket
    m_udpSocket = Syscall(socket, AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);

    // Bind socket
    Syscall(bind, m_udpSocket.get(), reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr));

    // Configure epoll to track the UDP socket. EPOLLIN is used to get epoll notifications
    // whenever there is data available to be read from the socket
    epoll_event event{};
    event.events = EPOLLIN;
    event.data.fd = m_udpSocket.get();
    Syscall(epoll_ctl, m_epollFd.get(), EPOLL_CTL_ADD, m_udpSocket.get(), &event);

    GNS_LOG_INFO("Successfully started UDP server on IP {}", ipAddress.c_str());
}
CATCH_LOG()

void DnsServer::StartTcpDnsServer(const std::string& ipAddress) noexcept
try
{
    sockaddr_in serverAddr{};

    serverAddr.sin_family = AF_INET;
    Syscall(inet_pton, AF_INET, ipAddress.c_str(), &serverAddr.sin_addr);
    serverAddr.sin_port = htons(c_dnsServerPort);

    // Create IPv4 TCP socket
    m_tcpListenSocket = Syscall(socket, AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);

    // Bind socket
    Syscall(bind, m_tcpListenSocket.get(), reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr));

    // Listen for incoming connections
    Syscall(listen, m_tcpListenSocket.get(), c_maxListenBacklog);

    // Configure epoll to track the TCP listening socket. EPOLLIN is used to get epoll notifications
    // whenever there is a new incoming TCP connection.
    epoll_event event{};
    event.events = EPOLLIN;
    event.data.fd = m_tcpListenSocket.get();
    Syscall(epoll_ctl, m_epollFd.get(), EPOLL_CTL_ADD, m_tcpListenSocket.get(), &event);

    GNS_LOG_INFO("Successfully started TCP server on IP {}", ipAddress.c_str());
}
CATCH_LOG();

void DnsServer::HandleUdpDnsResponse(const gsl::span<gsl::byte> dnsBuffer, const LX_GNS_DNS_CLIENT_IDENTIFIER& dnsClientIdentifier) noexcept
try
{
    GNS_LOG_INFO("New UDP DNS response DNS buffer size: {}, UDP request id: {}", dnsBuffer.size(), dnsClientIdentifier.DnsClientId);

    std::scoped_lock<std::mutex> lock{m_udpLock};

    auto it = m_udpRequests.find(dnsClientIdentifier.DnsClientId);
    if (it == m_udpRequests.end())
    {
        GNS_LOG_ERROR("Received a response for a UDP request that is not tracked, UDP request id: {}", dnsClientIdentifier.DnsClientId);
        return;
    }

    // Stop tracking the request, irrespective of the DNS response being successfully sent
    const auto removeDnsRequest = wil::scope_exit([&] { m_udpRequests.erase(dnsClientIdentifier.DnsClientId); });

    sockaddr_in& remoteAddr = it->second;

    // Send DNS response buffer back to the Linux DNS client
    int bufferSize = dnsBuffer.size();
    int totalBytesSent = 0;

    while (totalBytesSent < bufferSize)
    {
        int bytesSent = Syscall(
            sendto, m_udpSocket.get(), dnsBuffer.data() + totalBytesSent, bufferSize - totalBytesSent, 0, reinterpret_cast<sockaddr*>(&remoteAddr), sizeof(remoteAddr));
        totalBytesSent += bytesSent;
    }
}
CATCH_LOG()

void DnsServer::HandleTcpDnsResponse(const gsl::span<gsl::byte> dnsBuffer, const LX_GNS_DNS_CLIENT_IDENTIFIER& dnsClientIdentifier) noexcept
try
{
    GNS_LOG_INFO(
        "New TCP DNS response "
        "DNS buffer size: {}, TCP connection id: {}",
        dnsBuffer.size(),
        dnsClientIdentifier.DnsClientId);

    std::scoped_lock<std::mutex> lock{m_tcpLock};

    auto it = m_tcpConnectionContexts.find(dnsClientIdentifier.DnsClientId);
    if (it == m_tcpConnectionContexts.end())
    {
        GNS_LOG_ERROR("Received a response for an untracked TCP connection id: {}", dnsClientIdentifier.DnsClientId);
        return;
    }

    auto tcpConnection = it->second->m_tcpConnection.get();

    // Send DNS response buffer back to the Linux DNS client.
    //
    // Note: there might be more DNS requests sent on the same TCP connection. The DNS protocol allows sending the responses in a
    // different order than the order of the corresponding DNS requests.
    int bufferSize = dnsBuffer.size();
    int totalBytesSent = 0;

    while (totalBytesSent < bufferSize)
    {
        int bytesSent = Syscall(write, tcpConnection, dnsBuffer.data() + totalBytesSent, bufferSize - totalBytesSent);
        totalBytesSent += bytesSent;
    }
}
CATCH_LOG()

void DnsServer::HandleNewTcpConnection() noexcept
try
{
    std::scoped_lock<std::mutex> lock{m_tcpLock};

    // Accept new connection. Mark connection socket as non-blocking
    wil::unique_fd connectionFd = Syscall(accept4, m_tcpListenSocket.get(), nullptr, nullptr, SOCK_NONBLOCK);

    // Get next connection id. If value reaches UINT_MAX + 1 it will be automatically reset to 0
    const auto connectionId = m_currentTcpConnectionId++;

    // Track the new connection
    auto [it, _] = m_tcpConnectionContexts.emplace(
        connectionId, std::make_unique<DnsServer::TcpConnectionContext>(connectionId, std::move(connectionFd)));
    auto& localContext = it->second;

    auto removeContextOnError = wil::scope_exit([&] { m_tcpConnectionContexts.erase(connectionId); });

    // Register the new connection with epoll. EPOLLIN is used to get epoll notifications
    // whenever there is new data on the TCP connection.
    epoll_event event{};
    event.events = EPOLLIN;
    event.data.fd = localContext->m_tcpConnection.get();
    event.data.ptr = localContext.get();
    Syscall(epoll_ctl, m_epollFd.get(), EPOLL_CTL_ADD, localContext->m_tcpConnection.get(), &event);

    removeContextOnError.release();
}
CATCH_LOG();

void DnsServer::HandleNewTcpData(TcpConnectionContext* context) noexcept
try
{
    std::vector<gsl::byte> dnsRequest;
    uint32_t tcpConnectionId{};

    // Scoped m_tcpLock
    {
        std::scoped_lock<std::mutex> lock{m_tcpLock};

        // In case of any failure reading data, close the connection and stop tracking it.
        // Note: Closing the connection automatically unregisters it from epoll.
        auto removeConnectionOnError = wil::scope_exit([&] { m_tcpConnectionContexts.erase(context->m_connectionId); });

        // Read the remaining bytes of the current DNS request
        int bytesReceived = Syscall(
            recv,
            context->m_tcpConnection.get(),
            context->m_currentDnsRequest.data() + context->m_currentRequestOffset,
            context->m_currentDnsRequest.size() - context->m_currentRequestOffset,
            0);

        // 0 bytes received indicates connection was closed by the TCP client
        if (bytesReceived == 0)
        {
            return;
        }

        context->m_currentRequestOffset += bytesReceived;

        if (context->m_currentRequestOffset == context->m_currentDnsRequest.size())
        {
            // We read the 2 bytes that represent the DNS request length
            // Resize buffer to fit the entire DNS request (2 bytes storing the request length + the actual DNS request)
            if (context->m_currentDnsRequest.size() == c_byteCountTcpRequestLength)
            {
                uint16_t dnsRequestLength = 0;
                memcpy(&dnsRequestLength, context->m_currentDnsRequest.data(), c_byteCountTcpRequestLength);
                // The request length is stored in network byte order
                dnsRequestLength = ntohs(dnsRequestLength);

                context->m_currentDnsRequest.resize(c_byteCountTcpRequestLength + dnsRequestLength);
            }
            // We read a full DNS request
            else
            {
                // Move request to a local variable
                dnsRequest = std::move(context->m_currentDnsRequest);
                tcpConnectionId = context->m_connectionId;

                // Reset state to prepare for the next DNS request on the connection (if any)
                context->m_currentRequestOffset = 0;
                context->m_currentDnsRequest.resize(c_byteCountTcpRequestLength);
            }
        }

        removeConnectionOnError.release();
    }

    if (!dnsRequest.empty())
    {
        // Tunnel request to Windows
        LX_GNS_DNS_CLIENT_IDENTIFIER dnsClientIdentifier{};
        dnsClientIdentifier.DnsClientId = tcpConnectionId;
        dnsClientIdentifier.Protocol = IPPROTO_TCP;

        GNS_LOG_INFO("New TCP DNS request DNS buffer size: {}, TCP connection id: {}", dnsRequest.size(), dnsClientIdentifier.DnsClientId);

        m_tunnelDnsRequest(gsl::make_span(dnsRequest), dnsClientIdentifier);
    }
}
CATCH_LOG();

void DnsServer::HandleDnsResponse(const gsl::span<gsl::byte> dnsBuffer, const LX_GNS_DNS_CLIENT_IDENTIFIER& dnsClientIdentifier) noexcept
try
{
    switch (dnsClientIdentifier.Protocol)
    {
    case IPPROTO_UDP:
    {
        HandleUdpDnsResponse(dnsBuffer, dnsClientIdentifier);
        break;
    }
    case IPPROTO_TCP:
    {
        HandleTcpDnsResponse(dnsBuffer, dnsClientIdentifier);
        break;
    }

    default:
    {
        GNS_LOG_ERROR("Unexpected DNS protocol {}", dnsClientIdentifier.Protocol);
        break;
    }
    }
}
CATCH_LOG()

void DnsServer::ServerLoop() noexcept
{
    UtilSetThreadName("DnsServer");

    epoll_event events[c_epollWaitMaxEvents];
    memset(events, 0, sizeof(events));

    for (;;)
    {
        try
        {
            // A fixed number of events is requested from epoll_wait (c_epollWaitMaxEvents). In case the number of ready events is
            // greater than c_epollWaitMaxEvents, epoll will round-robin through the ready events until we get a notification for all of them.
            size_t numReadyEvents = Syscall(epoll_wait, m_epollFd.get(), events, c_epollWaitMaxEvents, -1);

            // No event
            if (numReadyEvents == 0)
            {
                continue;
            }

            for (size_t index = 0; index < numReadyEvents; index++)
            {
                // Notification for the shutdown pipe == the server needs to exit
                if (events[index].data.fd == m_shutdownServerLoopPipe.read().get())
                {
                    return;
                }
                // Notification for the listen socket == a new incoming TCP connection
                else if (events[index].data.fd == m_tcpListenSocket.get())
                {
                    HandleNewTcpConnection();
                }
                // Notification for the UDP socket == There is data to be read from the UDP socket, indicating a new DNS request was received
                else if (events[index].data.fd == m_udpSocket.get())
                {
                    HandleUdpDnsRequest();
                }
                // Other notifications == new data was received on one of the active TCP connections
                else
                {
                    HandleNewTcpData(static_cast<TcpConnectionContext*>(events[index].data.ptr));
                }
            }
        }
        CATCH_LOG()
    }
}

void DnsServer::HandleUdpDnsRequest() noexcept
try
{
    static std::array<gsl::byte, c_maxUdpDnsBufferSize> s_dnsBuffer;

    gsl::span<gsl::byte> dnsRequest;
    uint32_t udpRequestId{};

    // Scoped m_udpLock
    {
        std::scoped_lock<std::mutex> lock{m_udpLock};

        // Since we only configure an IPv4 DNS server in Linux, we expect all Linux DNS clients to use IPv4 addresses
        sockaddr_in remoteAddr{};
        socklen_t remoteAddrLen = sizeof(remoteAddr);

        // Read the DNS request
        int bytesReceived = Syscall(
            recvfrom, m_udpSocket.get(), s_dnsBuffer.data(), c_maxUdpDnsBufferSize, 0, reinterpret_cast<sockaddr*>(&remoteAddr), &remoteAddrLen);

        if (bytesReceived == 0)
        {
            GNS_LOG_ERROR("recvfrom returned 0 bytes");
            return;
        }

        // Get next request id. If value reaches UINT_MAX + 1 it will be automatically reset to 0
        const auto requestId = m_currentUdpRequestId++;

        GNS_LOG_INFO(
            "New UDP DNS request DNS client IP: {}, DNS client port {}, DNS buffer size: {}, UDP request id: {}",
            Address::FromBinary(AF_INET, 0, &remoteAddr.sin_addr).Addr().c_str(),
            ntohs(remoteAddr.sin_port),
            bytesReceived,
            requestId);

        // Move request to a local variable
        dnsRequest = std::move(gsl::make_span(s_dnsBuffer).subspan(0, bytesReceived));
        udpRequestId = requestId;

        // Track the request
        m_udpRequests.emplace(requestId, remoteAddr);
    }

    if (!dnsRequest.empty())
    {
        auto removeRequestOnError = wil::scope_exit([&] {
            std::scoped_lock<std::mutex> lock{m_udpLock};
            m_udpRequests.erase(udpRequestId);
        });

        // Tunnel request to Windows
        LX_GNS_DNS_CLIENT_IDENTIFIER dnsClientIdentifier{};
        dnsClientIdentifier.Protocol = IPPROTO_UDP;
        dnsClientIdentifier.DnsClientId = udpRequestId;

        m_tunnelDnsRequest(dnsRequest, dnsClientIdentifier);

        removeRequestOnError.release();
    }
}
CATCH_LOG()

void DnsServer::Stop() noexcept
try
{
    GNS_LOG_INFO("stopping DNS server");

    // Signal the server loop to stop by closing the write fd of the pipe
    m_shutdownServerLoopPipe.write().reset();

    if (m_serverThread.joinable())
    {
        m_serverThread.join();
    }
}
CATCH_LOG()