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
#include <lxwil.h>
#include <net/if.h>
#include <linux/if_tun.h>

#include <bpf/libbpf.h>

#include "util.h"
#include "SocketChannel.h"
#include "GnsPortTracker.h"
#include "SecCompDispatcher.h"
#include "seccomp_defs.h"
#include "CommandLine.h"
#include "listen_monitor.h"
#include "listen_monitor.skel.h"

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

extern "C" int OnListenMonitorEvent(void* ctx, void* data, size_t dataSz) noexcept
try
{
    auto* channel = static_cast<wsl::shared::SocketChannel*>(ctx);
    const auto* event = static_cast<const listen_event*>(data);

    sockaddr_storage sock{};
    if (event->family == AF_INET)
    {
        auto* ipv4 = reinterpret_cast<sockaddr_in*>(&sock);
        ipv4->sin_family = AF_INET;
        ipv4->sin_addr.s_addr = event->addr4;
        ipv4->sin_port = htons(event->port);
    }
    else if (event->family == AF_INET6)
    {
        auto* ipv6 = reinterpret_cast<sockaddr_in6*>(&sock);
        ipv6->sin6_family = AF_INET6;
        memcpy(ipv6->sin6_addr.s6_addr, event->addr6, sizeof(ipv6->sin6_addr.s6_addr));
        ipv6->sin6_port = htons(event->port);
    }
    else
    {
        return 0;
    }

    if (event->is_listen)
    {
        StartHostListener(*channel, sock);
    }
    else
    {
        StopHostListener(*channel, sock);
    }

    return 0;
}
catch (...)
{
    LOG_CAUGHT_EXCEPTION_MSG("Error processing listen monitor event");
    return 0;
}

int MonitorListeningSockets(wsl::shared::SocketChannel& channel)
{
    auto* skel = listen_monitor_bpf__open_and_load();
    if (!skel)
    {
        LOG_ERROR("Failed to open/load listen monitor BPF program, {}", errno);
        return -1;
    }

    auto destroySkel = wil::scope_exit([&] { listen_monitor_bpf__destroy(skel); });

    if (listen_monitor_bpf__attach(skel) != 0)
    {
        LOG_ERROR("Failed to attach listen monitor BPF program, {}", errno);
        return -1;
    }

    auto* rb = ring_buffer__new(bpf_map__fd(skel->maps.events), OnListenMonitorEvent, &channel, nullptr);
    if (!rb)
    {
        LOG_ERROR("Failed to create ring buffer, {}", errno);
        return -1;
    }

    auto destroyRb = wil::scope_exit([&] { ring_buffer__free(rb); });

    GNS_LOG_INFO("BPF listen monitor attached and running");

    for (;;)
    {
        int err = ring_buffer__poll(rb, -1);
        if (err == -EINTR)
        {
            continue;
        }

        if (err < 0)
        {
            LOG_ERROR("ring_buffer__poll failed, {}", err);
            return -1;
        }
    }
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
    int GuestRelayFd = -1;

    ArgumentParser parser(Argc, Argv);
    parser.AddArgument(Integer{BpfFd}, INIT_BPF_FD_ARG);
    parser.AddArgument(Integer{PortTrackerFd}, INIT_PORT_TRACKER_FD_ARG);
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

    const bool synchronousMode = BpfFd != -1;
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
        std::cerr << "synchronous mode requires --bpf-fd\n";
        return 1;
    }

    auto seccompDispatcher = std::make_shared<SecCompDispatcher>(BpfFd);

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

    GnsPortTracker portTracker(hvSocketChannel);
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
