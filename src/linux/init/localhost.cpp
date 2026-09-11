// Copyright (C) Microsoft Corporation. All rights reserved.
#include "common.h"
#include <memory>
#include <array>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <iostream>

#include <libgen.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/ip.h>
#include <sys/syscall.h>
#include <linux/unistd.h>
#include <linux/sock_diag.h>
#include <linux/inet_diag.h>
#include <lxwil.h>
#include <linux/if_tun.h>

#include "util.h"
#include "SocketChannel.h"
#include "GnsPortTracker.h"
#include "SecCompDispatcher.h"
#include "seccomp_defs.h"
#include "CommandLine.h"
#include "NetlinkChannel.h"
#include "NetlinkTransactionError.h"

#define TCP_LISTEN 10

namespace {

std::vector<sockaddr_storage> QueryListeningSockets(NetlinkChannel& channel)
{
    std::vector<sockaddr_storage> sockets{};
    try
    {
        inet_diag_req_v2 message{};
        message.sdiag_protocol = IPPROTO_TCP;
        message.idiag_states = (1 << TCP_LISTEN);

        auto onMessage = [&](const NetlinkResponse& response) {
            for (const auto& e : response.Messages<inet_diag_msg>(SOCK_DIAG_BY_FAMILY))
            {
                const auto* payload = e.Payload();
                sockaddr_storage sock{};

                if (payload->idiag_family == AF_INET)
                {
                    auto* ipv4 = reinterpret_cast<sockaddr_in*>(&sock);
                    ipv4->sin_family = AF_INET;
                    ipv4->sin_addr.s_addr = payload->id.idiag_src[0];
                    ipv4->sin_port = payload->id.idiag_sport;
                }
                else if (payload->idiag_family == AF_INET6)
                {
                    auto* ipv6 = reinterpret_cast<sockaddr_in6*>(&sock);
                    ipv6->sin6_family = AF_INET6;
                    static_assert(sizeof(ipv6->sin6_addr.s6_addr32) == sizeof(payload->id.idiag_src));
                    memcpy(ipv6->sin6_addr.s6_addr32, payload->id.idiag_src, sizeof(ipv6->sin6_addr.s6_addr32));
                    ipv6->sin6_port = payload->id.idiag_sport;
                }

                sockets.emplace_back(sock);
            }
        };

        // Query IPv4 listening sockets.
        {
            message.sdiag_family = AF_INET;
            auto transaction = channel.CreateTransaction(message, SOCK_DIAG_BY_FAMILY, NLM_F_DUMP);
            transaction.Execute(onMessage);
        }

        // Query IPv6 listening sockets.
        {
            message.sdiag_family = AF_INET6;
            auto transaction = channel.CreateTransaction(message, SOCK_DIAG_BY_FAMILY, NLM_F_DUMP);
            transaction.Execute(onMessage);
        }
    }
    catch (const NetlinkTransactionError& e)
    {
        // Log but don't fail - network state might be temporarily unavailable
        LOG_ERROR("Failed to query listening sockets via sock_diag: {}", e.what());
    }

    return sockets;
}

int SendRelayListenerSocket(wsl::shared::SocketChannel& channel, int hvSocketPort)
try
{
    LX_GNS_SET_PORT_LISTENER message{};
    message.Header.MessageType = LxGnsMessageSetPortListener;
    message.Header.MessageSize = sizeof(message);
    message.HvSocketPort = hvSocketPort;

    channel.SendMessage(message);

    return 0;
}
CATCH_RETURN_ERRNO();

LX_GNS_PORT_LISTENER_RELAY SockToRelayMessage(const sockaddr_storage& sock)
{
    LX_GNS_PORT_LISTENER_RELAY message{};
    message.Header.MessageSize = sizeof(message);
    message.Family = sock.ss_family;
    if (sock.ss_family == AF_INET)
    {
        auto ipv4 = reinterpret_cast<const sockaddr_in*>(&sock);
        message.Address[0] = ipv4->sin_addr.s_addr;
        message.Port = ntohs(ipv4->sin_port);
    }
    else if (sock.ss_family == AF_INET6)
    {
        auto ipv6 = reinterpret_cast<const sockaddr_in6*>(&sock);
        message.Port = ntohs(ipv6->sin6_port);
        memcpy(message.Address, ipv6->sin6_addr.__in6_union.__s6_addr, sizeof(message.Address));
    }
    return message;
}

int StartHostListener(wsl::shared::SocketChannel& channel, const sockaddr_storage& sock)
try
{
    auto message = SockToRelayMessage(sock);
    message.Header.MessageType = LxGnsMessagePortListenerRelayStart;
    auto transaction = channel.StartTransaction();
    transaction.Send(message);

    return 0;
}
CATCH_RETURN_ERRNO();

int StopHostListener(wsl::shared::SocketChannel& channel, const sockaddr_storage& sock)
try
{
    auto message = SockToRelayMessage(sock);
    message.Header.MessageType = LxGnsMessagePortListenerRelayStop;
    auto transaction = channel.StartTransaction();
    transaction.Send(message);

    return 0;
}
CATCH_RETURN_ERRNO();

bool IsSameSockAddr(const sockaddr_storage& left, const sockaddr_storage& right)
{
    if (left.ss_family != right.ss_family)
    {
        return false;
    }

    if (left.ss_family == AF_INET)
    {
        auto leftIpv4 = reinterpret_cast<const sockaddr_in*>(&left);
        auto rightIpv4 = reinterpret_cast<const sockaddr_in*>(&right);
        return (leftIpv4->sin_addr.s_addr == rightIpv4->sin_addr.s_addr && leftIpv4->sin_port == rightIpv4->sin_port);
    }
    else if (left.ss_family == AF_INET6)
    {
        auto leftIpv6 = reinterpret_cast<const sockaddr_in6*>(&left);
        auto rightIpv6 = reinterpret_cast<const sockaddr_in6*>(&right);
        return (leftIpv6->sin6_port == rightIpv6->sin6_port && memcmp(&leftIpv6->sin6_addr, &rightIpv6->sin6_addr, sizeof(in6_addr)) == 0);
    }

    FATAL_ERROR("Unrecognized socket family {}", left.ss_family);
    return false;
}

// Monitor listening TCP sockets using sock_diag netlink interface.
int MonitorListeningSockets(wsl::shared::SocketChannel& channel)
{
    NetlinkChannel netlinkChannel(SOCK_RAW, NETLINK_SOCK_DIAG);
    std::vector<sockaddr_storage> relays{};
    int result = 0;

    for (;;)
    {
        auto sockets = QueryListeningSockets(netlinkChannel);

        // Stop any relays that no longer match listening ports.
        std::erase_if(relays, [&](const auto& entry) {
            auto found =
                std::find_if(sockets.begin(), sockets.end(), [&](const auto& socket) { return IsSameSockAddr(entry, socket); });

            bool remove = (found == sockets.end());
            if (remove)
            {
                if (StopHostListener(channel, entry) < 0)
                {
                    result = -1;
                }
            }

            return remove;
        });

        // Create relays for any new ports.
        std::for_each(sockets.begin(), sockets.end(), [&](const auto& socket) {
            auto found =
                std::find_if(relays.begin(), relays.end(), [&](const auto& entry) { return IsSameSockAddr(entry, socket); });

            if (found == relays.end())
            {
                if (StartHostListener(channel, socket) < 0)
                {
                    result = -1;
                }
                else
                {
                    relays.push_back(socket);
                }
            }
        });

        // Ensure all start / stop operations were successful.
        if (result < 0)
        {
            break;
        }

        // Sleep before scanning again.
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    return result;
}

enum class RelayEndpoint : uint64_t
{
    HvSocket = 0,
    TcpSocket = 1
};

constexpr uint64_t c_listenerEventKey = 0;
constexpr uint64_t c_relayEndpointBit = uint64_t{1} << 63;
constexpr uint64_t c_connectionIdMask = ~c_relayEndpointBit;

uint64_t RelayEventKey(uint64_t connectionId, RelayEndpoint endpoint)
{
    WI_ASSERT(connectionId <= c_connectionIdMask);
    return connectionId | (endpoint == RelayEndpoint::TcpSocket ? c_relayEndpointBit : 0);
}

void SetNonBlocking(int fd)
{
    const int flags = fcntl(fd, F_GETFL, 0);
    THROW_LAST_ERROR_IF(flags < 0);
    THROW_LAST_ERROR_IF(fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0);
}

class RelayConnection
{
public:
    RelayConnection(int epollFd, uint64_t connectionId, wil::unique_fd&& relaySocket) :
        m_epollFd(epollFd), m_connectionId(connectionId), m_relaySocket(std::move(relaySocket))
    {
        SetNonBlocking(m_relaySocket.get());
        AddToEpoll(RelayEndpoint::HvSocket, EPOLLIN | EPOLLRDHUP);
    }

    bool Done() const noexcept
    {
        return m_done;
    }

    void Stop() noexcept
    {
        m_done = true;
    }

    void HandleEvent(RelayEndpoint endpoint, uint32_t events)
    {
        if (m_done)
        {
            return;
        }

        if (m_state == State::ReadingMessage)
        {
            WI_ASSERT(endpoint == RelayEndpoint::HvSocket);
            ReadMessage();
            if (m_state == State::ReadingMessage || m_done)
            {
                return;
            }
        }

        if (m_state == State::Connecting && endpoint == RelayEndpoint::TcpSocket && (events & (EPOLLOUT | EPOLLERR | EPOLLHUP)))
        {
            CompleteConnect();
        }

        if (m_state == State::Connecting || m_state == State::Relaying)
        {
            auto& writeDirection = DirectionForDestination(endpoint);
            if (!writeDirection.Empty() && (events & (EPOLLOUT | EPOLLERR | EPOLLHUP)))
            {
                Write(writeDirection);
            }

            auto& readDirection = DirectionForSource(endpoint);
            if (!readDirection.SourceEof && readDirection.Empty() && (events & (EPOLLIN | EPOLLRDHUP | EPOLLHUP)))
            {
                Read(readDirection);
            }

            if (events & EPOLLERR)
            {
                int socketError{};
                socklen_t socketErrorSize = sizeof(socketError);
                THROW_LAST_ERROR_IF(getsockopt(Fd(endpoint), SOL_SOCKET, SO_ERROR, &socketError, &socketErrorSize) < 0);
                if (socketError != 0)
                {
                    THROW_ERRNO(socketError);
                }
            }

            HalfCloseIfDrained(m_relayToTcp);
            HalfCloseIfDrained(m_tcpToRelay);
            m_done = m_relayToTcp.DestinationShutdown && m_tcpToRelay.DestinationShutdown;

            if (!m_done)
            {
                UpdateInterests();
            }
        }
    }

private:
    enum class State
    {
        ReadingMessage,
        Connecting,
        Relaying
    };

    struct Direction
    {
        Direction(RelayEndpoint source, RelayEndpoint destination, size_t bufferSize = 0) :
            Source(source), Destination(destination), Buffer(bufferSize)
        {
        }

        bool Empty() const noexcept
        {
            return Size == 0;
        }

        RelayEndpoint Source;
        RelayEndpoint Destination;
        std::vector<gsl::byte> Buffer;
        size_t Offset{};
        size_t Size{};
        bool SourceEof{};
        bool DestinationShutdown{};
    };

    int Fd(RelayEndpoint endpoint) const noexcept
    {
        return endpoint == RelayEndpoint::HvSocket ? m_relaySocket.get() : m_tcpSocket.get();
    }

    Direction& DirectionForSource(RelayEndpoint endpoint)
    {
        return endpoint == RelayEndpoint::HvSocket ? m_relayToTcp : m_tcpToRelay;
    }

    Direction& DirectionForDestination(RelayEndpoint endpoint)
    {
        return endpoint == RelayEndpoint::HvSocket ? m_tcpToRelay : m_relayToTcp;
    }

    uint32_t EventsFor(RelayEndpoint endpoint) const
    {
        if (m_state == State::ReadingMessage)
        {
            return EPOLLIN | EPOLLRDHUP;
        }

        if (m_state == State::Connecting && endpoint == RelayEndpoint::TcpSocket)
        {
            return EPOLLOUT | EPOLLRDHUP;
        }

        uint32_t events{};
        const auto& readDirection = endpoint == RelayEndpoint::HvSocket ? m_relayToTcp : m_tcpToRelay;
        if (!readDirection.SourceEof && readDirection.Empty())
        {
            events |= EPOLLIN | EPOLLRDHUP;
        }

        const auto& writeDirection = endpoint == RelayEndpoint::HvSocket ? m_tcpToRelay : m_relayToTcp;
        if (!writeDirection.Empty())
        {
            events |= EPOLLOUT;
        }

        return events;
    }

    void AddToEpoll(RelayEndpoint endpoint, uint32_t events)
    {
        epoll_event event{};
        event.events = events;
        event.data.u64 = RelayEventKey(m_connectionId, endpoint);
        THROW_LAST_ERROR_IF(epoll_ctl(m_epollFd, EPOLL_CTL_ADD, Fd(endpoint), &event) < 0);

        if (endpoint == RelayEndpoint::HvSocket)
        {
            m_relayEvents = events;
        }
        else
        {
            m_tcpEvents = events;
        }
    }

    void UpdateInterest(RelayEndpoint endpoint, uint32_t events)
    {
        auto& currentEvents = endpoint == RelayEndpoint::HvSocket ? m_relayEvents : m_tcpEvents;
        if (currentEvents == events)
        {
            return;
        }

        if (events == 0)
        {
            THROW_LAST_ERROR_IF(epoll_ctl(m_epollFd, EPOLL_CTL_DEL, Fd(endpoint), nullptr) < 0);
            currentEvents = 0;
            return;
        }

        epoll_event event{};
        event.events = events;
        event.data.u64 = RelayEventKey(m_connectionId, endpoint);
        const int operation = currentEvents == 0 ? EPOLL_CTL_ADD : EPOLL_CTL_MOD;
        THROW_LAST_ERROR_IF(epoll_ctl(m_epollFd, operation, Fd(endpoint), &event) < 0);
        currentEvents = events;
    }

    void UpdateInterests()
    {
        UpdateInterest(RelayEndpoint::HvSocket, EventsFor(RelayEndpoint::HvSocket));
        if (m_tcpSocket)
        {
            UpdateInterest(RelayEndpoint::TcpSocket, EventsFor(RelayEndpoint::TcpSocket));
        }
    }

    void ReadMessage()
    {
        while (m_messageBytes < sizeof(m_message))
        {
            auto* destination = reinterpret_cast<char*>(&m_message) + m_messageBytes;
            const auto bytesRead = TEMP_FAILURE_RETRY(read(m_relaySocket.get(), destination, sizeof(m_message) - m_messageBytes));
            if (bytesRead > 0)
            {
                m_messageBytes += static_cast<size_t>(bytesRead);
                continue;
            }

            if (bytesRead == 0)
            {
                m_done = true;
                return;
            }

            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                return;
            }

            THROW_LAST_ERROR();
        }

        THROW_ERRNO_IF(
            EINVAL,
            m_message.Header.MessageType != LxInitMessageStartSocketRelay || m_message.Header.MessageSize != sizeof(m_message) ||
                m_message.BufferSize == 0);

        const auto bufferSize = static_cast<size_t>(m_message.BufferSize);
        m_relayToTcp = Direction(RelayEndpoint::HvSocket, RelayEndpoint::TcpSocket, bufferSize);
        m_tcpToRelay = Direction(RelayEndpoint::TcpSocket, RelayEndpoint::HvSocket, bufferSize);

        sockaddr* socketAddress{};
        socklen_t socketAddressSize{};
        sockaddr_in ipv4Address{};
        sockaddr_in6 ipv6Address{};

        if (m_message.Family == AF_INET)
        {
            ipv4Address.sin_family = AF_INET;
            ipv4Address.sin_port = htons(m_message.Port);
            ipv4Address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            socketAddress = reinterpret_cast<sockaddr*>(&ipv4Address);
            socketAddressSize = sizeof(ipv4Address);
        }
        else if (m_message.Family == AF_INET6)
        {
            ipv6Address.sin6_family = AF_INET6;
            ipv6Address.sin6_port = htons(m_message.Port);
            ipv6Address.sin6_addr = IN6ADDR_LOOPBACK_INIT;
            socketAddress = reinterpret_cast<sockaddr*>(&ipv6Address);
            socketAddressSize = sizeof(ipv6Address);
        }
        else
        {
            THROW_ERRNO(EINVAL);
        }

        m_tcpSocket.reset(socket(socketAddress->sa_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_TCP));
        THROW_LAST_ERROR_IF(!m_tcpSocket);

        const int result = connect(m_tcpSocket.get(), socketAddress, socketAddressSize);
        if (result < 0 && errno != EINPROGRESS)
        {
            const int error = errno;
            LOG_ERROR("Failed to connect to port: {}, family: {}, errno: {}", m_message.Port, m_message.Family, error);
            THROW_ERRNO(error);
        }

        m_state = result == 0 ? State::Relaying : State::Connecting;
        AddToEpoll(RelayEndpoint::TcpSocket, EventsFor(RelayEndpoint::TcpSocket));
        UpdateInterest(RelayEndpoint::HvSocket, EventsFor(RelayEndpoint::HvSocket));
    }

    void CompleteConnect()
    {
        int socketError{};
        socklen_t socketErrorSize = sizeof(socketError);
        THROW_LAST_ERROR_IF(getsockopt(m_tcpSocket.get(), SOL_SOCKET, SO_ERROR, &socketError, &socketErrorSize) < 0);
        if (socketError != 0)
        {
            LOG_ERROR("Failed to connect to port: {}, family: {}, errno: {}", m_message.Port, m_message.Family, socketError);
            THROW_ERRNO(socketError);
        }

        m_state = State::Relaying;
        UpdateInterests();
    }

    void Read(Direction& direction)
    {
        WI_ASSERT(direction.Empty());
        const auto bytesRead = TEMP_FAILURE_RETRY(read(Fd(direction.Source), direction.Buffer.data(), direction.Buffer.size()));
        if (bytesRead > 0)
        {
            direction.Offset = 0;
            direction.Size = static_cast<size_t>(bytesRead);
        }
        else if (bytesRead == 0)
        {
            direction.SourceEof = true;
        }
        else if (errno != EAGAIN && errno != EWOULDBLOCK)
        {
            THROW_LAST_ERROR();
        }
    }

    void Write(Direction& direction)
    {
        WI_ASSERT(!direction.Empty());
        const auto bytesWritten =
            TEMP_FAILURE_RETRY(send(Fd(direction.Destination), direction.Buffer.data() + direction.Offset, direction.Size, MSG_NOSIGNAL));
        if (bytesWritten > 0)
        {
            direction.Offset += static_cast<size_t>(bytesWritten);
            direction.Size -= static_cast<size_t>(bytesWritten);
            if (direction.Empty())
            {
                direction.Offset = 0;
            }
        }
        else if (bytesWritten == 0)
        {
            THROW_ERRNO(EPIPE);
        }
        else if (bytesWritten < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
        {
            THROW_LAST_ERROR();
        }
    }

    void HalfCloseIfDrained(Direction& direction)
    {
        if (m_state != State::Relaying || !direction.SourceEof || !direction.Empty() || direction.DestinationShutdown)
        {
            return;
        }

        if (shutdown(Fd(direction.Destination), SHUT_WR) < 0)
        {
            LOG_ERROR("shutdown({}) failed, {}", Fd(direction.Destination), errno);
        }

        direction.DestinationShutdown = true;
    }

    int m_epollFd;
    uint64_t m_connectionId;
    wil::unique_fd m_relaySocket;
    wil::unique_fd m_tcpSocket;
    State m_state = State::ReadingMessage;
    LX_INIT_START_SOCKET_RELAY m_message{};
    size_t m_messageBytes{};
    Direction m_relayToTcp{RelayEndpoint::HvSocket, RelayEndpoint::TcpSocket};
    Direction m_tcpToRelay{RelayEndpoint::TcpSocket, RelayEndpoint::HvSocket};
    uint32_t m_relayEvents{};
    uint32_t m_tcpEvents{};
    bool m_done{};
};
} // namespace

void RunLocalHostRelay(sockaddr_vm hvSocketAddress, int listenSocket)
{
    SetNonBlocking(listenSocket);

    wil::unique_fd epollFd{epoll_create1(EPOLL_CLOEXEC)};
    THROW_LAST_ERROR_IF(!epollFd);

    epoll_event listenerEvent{};
    listenerEvent.events = EPOLLIN;
    listenerEvent.data.u64 = c_listenerEventKey;
    THROW_LAST_ERROR_IF(epoll_ctl(epollFd.get(), EPOLL_CTL_ADD, listenSocket, &listenerEvent) < 0);

    std::unordered_map<uint64_t, std::unique_ptr<RelayConnection>> connections;
    uint64_t nextConnectionId = 1;
    std::array<epoll_event, 64> events{};

    for (;;)
    {
        const int eventCount = TEMP_FAILURE_RETRY(epoll_wait(epollFd.get(), events.data(), static_cast<int>(events.size()), -1));
        THROW_LAST_ERROR_IF(eventCount < 0);

        for (int index = 0; index < eventCount; ++index)
        {
            const auto eventKey = events[index].data.u64;
            if (eventKey == c_listenerEventKey)
            {
                for (;;)
                {
                    sockaddr_vm peerAddress = hvSocketAddress;
                    socklen_t peerAddressSize = sizeof(peerAddress);
                    wil::unique_fd relaySocket{accept4(
                        listenSocket, reinterpret_cast<sockaddr*>(&peerAddress), &peerAddressSize, SOCK_NONBLOCK | SOCK_CLOEXEC)};
                    if (!relaySocket)
                    {
                        const int error = errno;
                        if (error == EAGAIN || error == EWOULDBLOCK)
                        {
                            break;
                        }

                        if (error == EINTR || error == ECONNABORTED || error == EPROTO)
                        {
                            continue;
                        }

                        LOG_ERROR("accept4 failed, {}", error);
                        THROW_ERRNO(error);
                    }

                    THROW_ERRNO_IF(EOVERFLOW, nextConnectionId > c_connectionIdMask);
                    const auto connectionId = nextConnectionId++;
                    try
                    {
                        connections.emplace(connectionId, std::make_unique<RelayConnection>(epollFd.get(), connectionId, std::move(relaySocket)));
                    }
                    CATCH_LOG();
                }

                continue;
            }

            const auto connectionId = eventKey & c_connectionIdMask;
            const auto endpoint = (eventKey & c_relayEndpointBit) ? RelayEndpoint::TcpSocket : RelayEndpoint::HvSocket;
            const auto connection = connections.find(connectionId);
            if (connection == connections.end())
            {
                continue;
            }

            try
            {
                connection->second->HandleEvent(endpoint, events[index].events);
            }
            catch (...)
            {
                LOG_CAUGHT_EXCEPTION();
                connection->second->Stop();
            }
        }

        // Closing the uniquely owned sockets removes their epoll registrations after the final fd reference is closed.
        // Defer erasure until the full batch is processed because events already returned by epoll_wait() remain cached here.
        std::erase_if(connections, [](const auto& entry) { return entry.second->Done(); });
    }
}

// Create a thread to monitor for connections to relay.
int StartLocalhostRelay(wsl::shared::SocketChannel& channel, int GuestRelayFd, bool ScanForPorts)
try
{
    // If the other end of a socket is reset, write will result in EPIPE. Ignore
    // this signal and just use the write return value.
    THROW_LAST_ERROR_IF(signal(SIGPIPE, SIG_IGN) == SIG_ERR);

    sockaddr_vm hvSocketAddress = {};
    socklen_t hvSocketAddressLen = sizeof(hvSocketAddress);
    if (getsockname(GuestRelayFd, reinterpret_cast<sockaddr*>(&hvSocketAddress), &hvSocketAddressLen) < 0 ||
        hvSocketAddressLen != sizeof(hvSocketAddress))
    {
        LOG_ERROR("Failed to get hvsocket port: {}, {}", errno, hvSocketAddressLen);
        return -1;
    }

    wil::unique_fd listenSocket{GuestRelayFd};
    THROW_LAST_ERROR_IF(!listenSocket);

    // Create a thread to accept incoming connections from the host listener
    std::thread([hvSocketAddress, listenSocket = std::move(listenSocket)]() {
        try
        {
            RunLocalHostRelay(hvSocketAddress, listenSocket.get());
        }
        CATCH_LOG()
    }).detach();

    if (SendRelayListenerSocket(channel, hvSocketAddress.svm_port) < 0)
    {
        LOG_ERROR("Unable to send relay listener socket");
        return -1;
    }

    if (ScanForPorts)
    {
        return MonitorListeningSockets(channel);
    }

    return 0;
}
catch (...)
{
    LOG_CAUGHT_EXCEPTION_MSG("Could not start localhost relay.")
    return -1;
}

int RunPortTracker(int Argc, char** Argv)
{
    using namespace wsl::shared;

    constexpr auto* Usage = "Usage: localhost " INIT_PORT_TRACKER_FD_ARG
                            " fd"
                            " [" INIT_BPF_FD_ARG
                            " fd]"
                            " [" INIT_NETLINK_FD_ARG
                            " fd]"
                            " [" INIT_PORT_TRACKER_LOCALHOST_RELAY
                            " fd]"
                            " [" INIT_PORT_TRACKER_NETWORKING_MODE_ARG " mode]\n";

    // This is only supported on VM mode.
    if (!UtilIsUtilityVm())
    {
        return -1;
    }

    // Initialize error and telemetry logging.
    InitializeLogging(true);

    int BpfFd = -1;
    int PortTrackerFd = -1;
    int NetlinkSocketFd = -1;
    int GuestRelayFd = -1;
    int NetworkingMode = static_cast<int>(LxMiniInitNetworkingModeNone);

    ArgumentParser parser(Argc, Argv);
    parser.AddArgument(Integer{BpfFd}, INIT_BPF_FD_ARG);
    parser.AddArgument(Integer{PortTrackerFd}, INIT_PORT_TRACKER_FD_ARG);
    parser.AddArgument(Integer{NetlinkSocketFd}, INIT_NETLINK_FD_ARG);
    parser.AddArgument(Integer{GuestRelayFd}, INIT_PORT_TRACKER_LOCALHOST_RELAY);
    parser.AddArgument(Integer{NetworkingMode}, INIT_PORT_TRACKER_NETWORKING_MODE_ARG);

    try
    {
        parser.Parse();
    }
    catch (const wil::ExceptionWithUserMessage& e)
    {
        std::cerr << e.what() << "\n" << Usage;
        return 1;
    }

    if (NetworkingMode < LxMiniInitNetworkingModeNone || NetworkingMode > LxMiniInitNetworkingModeConsomme)
    {
        std::cerr << "Invalid networking mode (" << NetworkingMode << ")\n";
        return 1;
    }

    const bool synchronousMode = BpfFd != -1 && NetlinkSocketFd != -1;
    const bool localhostRelay = GuestRelayFd != -1;
    auto hvSocketChannel = std::make_shared<wsl::shared::SocketChannel>(wil::unique_fd{PortTrackerFd}, "localhost");

    if (localhostRelay)
    {
        // This needs to be the first message sent over the PortTrackerFd channel,
        // before running the seccomp dispatcher loop.
        const int ret = StartLocalhostRelay(*hvSocketChannel, GuestRelayFd, !synchronousMode);
        if (ret < 0)
        {
            LOG_ERROR("Failed to start the guest side of the localhost relay");
        }
        if (!synchronousMode)
        {
            return ret;
        }
    }

    if (!synchronousMode)
    {
        std::cerr << "either both or none of --bpf-fd and --netlink-socket can be passed\n";
        return 1;
    }

    auto channel = NetlinkChannel::FromFd(NetlinkSocketFd);

    auto seccompDispatcher = std::make_shared<SecCompDispatcher>(BpfFd);

    GnsPortTracker portTracker(hvSocketChannel, std::move(channel), seccompDispatcher, static_cast<LX_MINI_INIT_NETWORKING_MODE>(NetworkingMode));

    seccompDispatcher->RegisterHandler(
        __NR_bind, [&portTracker](seccomp_notif* notification) { return portTracker.ProcessSecCompNotification(notification); });

    // listen() can perform an implicit autobind (assigning an ephemeral port) when called on a
    // socket that was never explicitly bind()'d. That autobind is otherwise invisible to the
    // port tracker, so listen() needs to be intercepted the same way bind() is.
    seccompDispatcher->RegisterHandler(
        __NR_listen, [&portTracker](seccomp_notif* notification) { return portTracker.ProcessSecCompNotification(notification); });

#ifdef __x86_64__
    seccompDispatcher->RegisterHandler(I386_NR_socketcall, [&portTracker](seccomp_notif* notification) {
        return portTracker.ProcessSecCompNotification(notification);
    });
#else
    seccompDispatcher->RegisterHandler(ARMV7_NR_bind, [&portTracker](seccomp_notif* notification) {
        return portTracker.ProcessSecCompNotification(notification);
    });
    seccompDispatcher->RegisterHandler(ARMV7_NR_listen, [&portTracker](seccomp_notif* notification) {
        return portTracker.ProcessSecCompNotification(notification);
    });
#endif

    seccompDispatcher->RegisterHandler(__NR_ioctl, [hvSocketChannel, seccompDispatcher](auto notification) -> int {
        LX_GNS_TUN_BRIDGE_REQUEST request{};
        request.Header.MessageType = LxGnsMessageIfStateChangeRequest;
        request.Header.MessageSize = sizeof(request);
        auto ifreqMemory =
            seccompDispatcher->ReadProcessMemory(notification->id, notification->pid, notification->data.args[2], sizeof(ifreq));
        if (!ifreqMemory.has_value())
        {
            return -1;
        }

        auto& ifRequest = *reinterpret_cast<ifreq*>(ifreqMemory->data());
        memcpy(request.InterfaceName, ifRequest.ifr_ifrn.ifrn_name, sizeof(request.InterfaceName));
        request.InterfaceUp = ifRequest.ifr_ifru.ifru_flags & IFF_UP;
        const auto& reply = hvSocketChannel->Transaction(request);

        return reply.Result;
    });

    try
    {
        portTracker.Run();
    }
    catch (const std::exception& e)
    {
        std::cerr << "Port tracker exiting with fatal error, " << e.what() << std::endl;
    }

    return 1;
}
