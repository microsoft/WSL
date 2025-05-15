// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include <map>
#include "common.h"
#include "lxinitshared.h"

using DnsTunnelingCallback = std::function<void(const gsl::span<gsl::byte>, const LX_GNS_DNS_CLIENT_IDENTIFIER&)>;

// Number of bytes used to store the length of DNS over TCP requests
constexpr int c_byteCountTcpRequestLength = 2;

class DnsServer
{
public:
    DnsServer(DnsTunnelingCallback&& tunnelDnsRequest);
    ~DnsServer() noexcept;

    DnsServer(const DnsServer&) = delete;
    DnsServer(DnsServer&&) = delete;
    DnsServer& operator=(const DnsServer&) = delete;
    DnsServer& operator=(DnsServer&&) = delete;

    // Start DNS server.
    //
    // Arguments:
    //    ipAddress - IP address to start server on.
    void Start(const std::string& ipAddress) noexcept;

    // Process DNS response received from Windows.
    //
    // Arguments:
    //    dnsBuffer - buffer containing DNS response.
    //    dnsClientIdentifier - struct containing protocol (TCP/UDP) and unique id of the Linux DNS client making the request.
    void HandleDnsResponse(const gsl::span<gsl::byte> dnsBuffer, const LX_GNS_DNS_CLIENT_IDENTIFIER& dnsClientIdentifier) noexcept;

    void Stop() noexcept;

private:
    struct TcpConnectionContext
    {
        // Connection fd
        wil::unique_fd m_tcpConnection;

        // Offset in m_currentDnsRequest indicating how much of the current DNS request on
        // the TCP connection has been read. Using 2 bytes to represent the offset as the request length is represented using 2 bytes.
        uint16_t m_currentRequestOffset = 0;

        // Buffer containing the current DNS request received on the TCP connection.
        std::vector<gsl::byte> m_currentDnsRequest;

        // Unique connection id. The connection fd would be a candidate for this, but the fd might be reused, so we need a different id.
        uint32_t m_connectionId{};

        TcpConnectionContext(uint32_t id, wil::unique_fd&& tcpConnection) :
            m_tcpConnection(std::move(tcpConnection)), m_connectionId(id)
        {
            // Resize to fit the bytes that represent the request length
            m_currentDnsRequest.resize(c_byteCountTcpRequestLength);
        }

        ~TcpConnectionContext() noexcept = default;

        TcpConnectionContext(const TcpConnectionContext&) = delete;
        TcpConnectionContext& operator=(const TcpConnectionContext&) = delete;
        TcpConnectionContext(TcpConnectionContext&&) = delete;
        TcpConnectionContext& operator=(TcpConnectionContext&&) = delete;
    };

    void StartUdpDnsServer(const std::string& ipAddress) noexcept;

    void StartTcpDnsServer(const std::string& ipAddress) noexcept;

    // Main server loop, processing epoll notifications.
    void ServerLoop() noexcept;

    // Accept new incoming TCP connection.
    void HandleNewTcpConnection() noexcept;

    // Handle new data received on an existing TCP connection.
    void HandleNewTcpData(TcpConnectionContext* context) noexcept;

    // Read the next DNS request from the UDP socket.
    void HandleUdpDnsRequest() noexcept;

    void HandleUdpDnsResponse(const gsl::span<gsl::byte> dnsBuffer, const LX_GNS_DNS_CLIENT_IDENTIFIER& dnsClientIdentifier) noexcept;

    void HandleTcpDnsResponse(const gsl::span<gsl::byte> dnsBuffer, const LX_GNS_DNS_CLIENT_IDENTIFIER& dnsClientIdentifier) noexcept;

    // File descriptor used to interact with epoll. Declared before the UDP and TCP sockets and the shutdown pipe so it will be closed after them.
    // Note: Closing a socket fd automatically leads to unregistering it from epoll - EPOLL_CTL_DEL is not necessary for that fd.
    wil::unique_fd m_epollFd;

    std::mutex m_udpLock;

    // _Guarded_by_(m_udpLock)
    wil::unique_fd m_udpSocket;

    // Unique id that is incremented for each DNS request over UDP. In case the value reaches MAX_UINT and is reset to 0,
    // it's assumed previous requests with id's 0, 1, ... finished in the meantime and the id can be reused.
    // _Guarded_by_(m_udpLock)
    uint32_t m_currentUdpRequestId = 0;

    // Mapping id of an UDP DNS request to the sockaddr_in struct storing the IP and port used by the Linux DNS client that made
    // the DNS request. Note: Since we only configure an IPv4 DNS server in Linux, we expect all Linux DNS clients to use IPv4
    // addresses. _Guarded_by_(m_udpLock)
    std::map<uint32_t, sockaddr_in> m_udpRequests;

    wil::unique_fd m_tcpListenSocket;

    std::mutex m_tcpLock;

    // Unique id that is incremented for each TCP connection. In case the value reaches MAX_UINT and is reset to 0,
    // it's assumed previous connections with id's 0, 1, ... were closed in the meantime and the id can be reused.
    // _Guarded_by_(m_udpLock)
    uint32_t m_currentTcpConnectionId = 0;

    // Mapping TCP connection unique id to connection context
    // _Guarded_by_(m_udpLock)
    std::map<uint32_t, std::unique_ptr<TcpConnectionContext>> m_tcpConnectionContexts;

    // Pipe used to stop m_serverThread.
    wil::unique_pipe m_shutdownServerLoopPipe;

    // Thread running the server loop.
    std::thread m_serverThread;

    // Callback used for tunneling a DNS request to Windows to be resolved.
    DnsTunnelingCallback m_tunnelDnsRequest;
};
