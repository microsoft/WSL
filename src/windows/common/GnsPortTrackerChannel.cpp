// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "socket.hpp"
#include "GnsPortTrackerChannel.h"

using wsl::core::GnsPortTrackerChannel;

GnsPortTrackerChannel::GnsPortTrackerChannel(
    wil::unique_socket&& Socket,
    const std::function<int(const SOCKADDR_INET&, int, bool)>& Callback,
    const std::function<void(const std::string&, bool)>& InterfaceStateCallback) :
    m_callback(Callback),
    m_interfaceStateCallback(InterfaceStateCallback),
    m_channel(std::move(Socket), "GNSPortTracker", m_stopEvent.get())
{
    m_thread = std::thread{std::bind(&GnsPortTrackerChannel::Run, this)};
}

GnsPortTrackerChannel::~GnsPortTrackerChannel()
{
    LOG_IF_WIN32_BOOL_FALSE(SetEvent(m_stopEvent.get()));

    if (m_thread.joinable())
    {
        m_thread.join();
    }
}

void GnsPortTrackerChannel::Run()
{
    try
    {
        for (;;)
        {
            auto [header, range] = m_channel.ReceiveMessageOrClosed<MESSAGE_HEADER>();
            if (header == nullptr)
            {
                return;
            }

            switch (header->MessageType)
            {
            case LxGnsMessagePortMappingRequest:
            {
                const auto* message = gslhelpers::try_get_struct<LX_GNS_PORT_ALLOCATION_REQUEST>(range);
                THROW_HR_IF_MSG(E_UNEXPECTED, !message, "Unexpected message size: %i", header->MessageSize);

                m_channel.SendResultMessage<int32_t>(m_callback(ConvertPortRequestToSockAddr(message), message->Protocol, message->Allocate));
            }
            break;
            case LxGnsMessageIfStateChangeRequest:
            {
                const auto* message = gslhelpers::try_get_struct<LX_GNS_TUN_BRIDGE_REQUEST>(range);
                THROW_HR_IF_MSG(E_UNEXPECTED, !message, "Unexpected message size: %i", header->MessageSize);

                m_interfaceStateCallback(message->InterfaceName, message->InterfaceUp);
                m_channel.SendResultMessage<int32_t>(0);
            }
            break;
            default:
                THROW_HR_MSG(E_UNEXPECTED, "Unexpected message type: %i", header->MessageType);
            }
        }
    }
    CATCH_LOG()
}

SOCKADDR_INET GnsPortTrackerChannel::ConvertPortRequestToSockAddr(_In_ const LX_GNS_PORT_ALLOCATION_REQUEST* portAllocationRequest)
{
    SOCKADDR_INET address{};

    address.si_family = static_cast<uint16_t>(portAllocationRequest->Af);

    if (portAllocationRequest->Af == AF_INET)
    {
        IN_ADDR ipv4Addr{};
        ipv4Addr.S_un.S_addr = portAllocationRequest->Address32[0];
        IN4ADDR_SETSOCKADDR(&address.Ipv4, &ipv4Addr, portAllocationRequest->Port);
    }
    else
    {
        IN6_ADDR ipv6Addr{};
        // Copy 16 bytes that represent IPv6 address
        memcpy(&ipv6Addr.u, portAllocationRequest->Address32, sizeof(portAllocationRequest->Address32));
        IN6ADDR_SETSOCKADDR(&address.Ipv6, &ipv6Addr, SCOPEID_UNSPECIFIED_INIT, portAllocationRequest->Port);
    }

    return address;
}
