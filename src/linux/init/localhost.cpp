// Copyright (C) Microsoft Corporation. All rights reserved.
#include "common.h"
#include <memory>
#include <string>
#include <string_view>
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

void ListenThread(sockaddr_vm hvSocketAddress, int listenSocket)
{
    pollfd pollDescriptors[] = {{listenSocket, POLLIN}};
    for (;;)
    {
        int result = poll(pollDescriptors, COUNT_OF(pollDescriptors), -1);
        if (result < 0)
        {
            LOG_ERROR("poll failed {}", errno);
            return;
        }

        if ((pollDescriptors[0].revents & POLLIN) == 0)
        {
            LOG_ERROR("unexpected revents {:x}", pollDescriptors[0].revents);
            return;
        }

        // Accept a connection and start a relay worker thread.
        wil::unique_fd relaySocket{UtilAcceptVsock(listenSocket, hvSocketAddress)};
        THROW_LAST_ERROR_IF(!relaySocket);

        std::thread([relaySocket = std::move(relaySocket)]() {
            try
            {
                // Read a message to determine which TCP port to connect to.
                std::vector<gsl::byte> buffer(sizeof(LX_INIT_START_SOCKET_RELAY));
                auto bytesRead = UtilReadBuffer(relaySocket.get(), buffer);
                if (bytesRead == 0)
                {
                    return;
                }

                auto* message = gslhelpers::try_get_struct<LX_INIT_START_SOCKET_RELAY>(gsl::make_span(buffer.data(), bytesRead));
                THROW_ERRNO_IF(EINVAL, !message || (message->Header.MessageType != LxInitMessageStartSocketRelay));

                // Connect to the actual socket address and set up a relay.
                //
                // N.B. While the relay was being set up, the server may have
                //      stopped listening.
                sockaddr* socketAddress;
                int socketAddressSize;
                sockaddr_in sockaddrIn{};
                sockaddr_in6 sockaddrIn6{};
                if (message->Family == AF_INET)
                {
                    sockaddrIn.sin_family = AF_INET;
                    sockaddrIn.sin_port = htons(message->Port);
                    sockaddrIn.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                    socketAddress = reinterpret_cast<sockaddr*>(&sockaddrIn);
                    socketAddressSize = sizeof(sockaddrIn);
                }
                else if (message->Family == AF_INET6)
                {
                    sockaddrIn6.sin6_family = AF_INET6;
                    sockaddrIn6.sin6_port = htons(message->Port);
                    sockaddrIn6.sin6_addr = IN6ADDR_LOOPBACK_INIT;
                    socketAddress = reinterpret_cast<sockaddr*>(&sockaddrIn6);
                    socketAddressSize = sizeof(sockaddrIn6);
                }
                else
                {
                    THROW_ERRNO(EINVAL);
                }

                wil::unique_fd tcpSocket{socket(socketAddress->sa_family, SOCK_STREAM, IPPROTO_TCP)};
                THROW_LAST_ERROR_IF(!tcpSocket);

                if (TEMP_FAILURE_RETRY(connect(tcpSocket.get(), socketAddress, socketAddressSize)) < 0)
                {
                    return;
                }

                // Resize the buffer to be the requested size.
                buffer.resize(message->BufferSize);

                // Begin relaying data.
                int outFd[2] = {tcpSocket.get(), relaySocket.get()};
                pollfd pollDescriptors[] = {{relaySocket.get(), POLLIN}, {tcpSocket.get(), POLLIN}};

                for (;;)
                {
                    if ((pollDescriptors[0].fd == -1) || (pollDescriptors[1].fd == -1))
                    {
                        return;
                    }

                    THROW_LAST_ERROR_IF(poll(pollDescriptors, COUNT_OF(pollDescriptors), -1) < 0);

                    bytesRead = 0;
                    for (int Index = 0; Index < COUNT_OF(pollDescriptors); Index += 1)
                    {
                        if (pollDescriptors[Index].revents & POLLIN)
                        {
                            bytesRead = UtilReadBuffer(pollDescriptors[Index].fd, buffer);
                            if (bytesRead == 0)
                            {
                                pollDescriptors[Index].fd = -1;
                                shutdown(outFd[Index], SHUT_WR);
                            }
                            else if (bytesRead < 0)
                            {
                                return;
                            }
                            else if (UtilWriteBuffer(outFd[Index], buffer.data(), bytesRead) < 0)
                            {
                                return;
                            }
                        }
                    }
                }
            }
            CATCH_LOG()
        }).detach();
    }

    return;
}

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
    channel.SendMessage(message);

    return 0;
}
CATCH_RETURN_ERRNO();

int StopHostListener(wsl::shared::SocketChannel& channel, const sockaddr_storage& sock)
try
{
    auto message = SockToRelayMessage(sock);
    message.Header.MessageType = LxGnsMessagePortListenerRelayStop;
    channel.SendMessage(message);

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
} // namespace

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
            ListenThread(hvSocketAddress, listenSocket.get());
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
                            " [" INIT_PORT_TRACKER_LOCALHOST_RELAY " fd]\n";

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

    ArgumentParser parser(Argc, Argv);
    parser.AddArgument(Integer{BpfFd}, INIT_BPF_FD_ARG);
    parser.AddArgument(Integer{PortTrackerFd}, INIT_PORT_TRACKER_FD_ARG);
    parser.AddArgument(Integer{NetlinkSocketFd}, INIT_NETLINK_FD_ARG);
    parser.AddArgument(Integer{GuestRelayFd}, INIT_PORT_TRACKER_LOCALHOST_RELAY);

    try
    {
        parser.Parse();
    }
    catch (const wil::ExceptionWithUserMessage& e)
    {
        std::cerr << e.what() << "\n" << Usage;
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

    GnsPortTracker portTracker(hvSocketChannel, std::move(channel), seccompDispatcher);

    seccompDispatcher->RegisterHandler(
        __NR_bind, [&portTracker](seccomp_notif* notification) { return portTracker.ProcessSecCompNotification(notification); });

#ifdef __x86_64__
    seccompDispatcher->RegisterHandler(I386_NR_socketcall, [&portTracker](seccomp_notif* notification) {
        return portTracker.ProcessSecCompNotification(notification);
    });
#else
    seccompDispatcher->RegisterHandler(ARMV7_NR_bind, [&portTracker](seccomp_notif* notification) {
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
