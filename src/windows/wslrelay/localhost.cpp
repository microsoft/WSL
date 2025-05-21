/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    helpers.cpp

Abstract:

    This file contains helper function definitions.

--*/

#include "precomp.h"
#include "helpers.hpp"
#include "svccomm.hpp"
#include "socket.hpp"
#include "hvsocket.hpp"
#include "relay.hpp"
#include "localhost.h"
#include <gsl/algorithm>
#include <gslhelpers.h>

#define LOCALHOST_RELAY_BUFFER_SIZE (0x20000)

struct in6_addr_linux
{
    union
    {
        uint8_t addr[16];
        uint32_t addr32[4];
    } u;
};

const uint16_t ADDR6_MASK3 = ~in6_addr_linux(IN6ADDR_LOOPBACK_INIT).u.addr32[3];
const uint32_t N_ADDR_LOOPBACK = ntohl(INADDR_LOOPBACK);
const uint32_t N_ADDR_ANY = ntohl(INADDR_ANY);

static VOID PortListenerAsync(_Inout_ std::shared_ptr<LX_PORT_LISTENER_THREAD_CONTEXT> Arguments);
static wil::unique_socket BindRelayListener(ADDRESS_FAMILY const Family, USHORT const Port);

static int GetPortListener(_In_ wsl::shared::SocketChannel& Channel)
{
    const auto& GuestAgentInfo = Channel.ReceiveMessage<LX_GNS_SET_PORT_LISTENER>();
    return GuestAgentInfo.HvSocketPort;
}

static int WindowsAddressFamily(int LinuxAddressFamily)
{
    switch (LinuxAddressFamily)
    {
    case LX_AF_INET:
        return AF_INET;

    case LX_AF_INET6:
        return AF_INET6;
    }
    WSL_LOG("PortRelayBindFamily", TraceLoggingValue(LinuxAddressFamily, "LinuxAddressFamily"), TraceLoggingLevel(WINEVENT_LEVEL_VERBOSE));

    THROW_HR(E_INVALIDARG);
}

bool BindsLocalhost(int Af, uint32_t const* Address)
{
    switch (Af)
    {
    case LX_AF_INET:
        return Address[0] == N_ADDR_ANY || Address[0] == N_ADDR_LOOPBACK;
    case LX_AF_INET6:
        return (Address[0] | Address[1] | Address[2] | (Address[3] & ADDR6_MASK3)) == 0;
    }
    return false;
}

void wsl::windows::wslrelay::localhost::RelayWorker(_In_ wsl::shared::SocketChannel& Channel, _In_ const GUID& VmId)
{
    std::vector<gsl::byte> Buffer;
    Relay Relay;
    const int HvSocketPort = GetPortListener(Channel);

    for (;;)
    {
        auto [Message, Span] = Channel.ReceiveMessageOrClosed<MESSAGE_HEADER>();
        if (Message == nullptr)
        {
            break;
        }

        switch (Message->MessageType)
        {
        case LxGnsMessagePortListenerRelayStart:
        case LxGnsMessagePortListenerRelayStop:
        {
            const auto* RelayOperation = gslhelpers::try_get_struct<LX_GNS_PORT_LISTENER_RELAY>(Span);
            THROW_HR_IF(E_INVALIDARG, !RelayOperation);
            // Ignore non-localhost addresses.
            if (WindowsAddressFamily(RelayOperation->Family) == AF_INET)
            {
                if (RelayOperation->Address[0] != INADDR_ANY && RelayOperation->Address[0] != htonl(INADDR_LOOPBACK))
                {
                    continue;
                }
            }
            else
            {
                WI_ASSERT(WindowsAddressFamily(RelayOperation->Family) == AF_INET6);
                bool IgnorePort = false;
                for (int Part = 0; Part < 3; ++Part)
                {
                    if (RelayOperation->Address[Part] != 0)
                    {
                        IgnorePort = true;
                        break;
                    }
                }
                // Create relays for unspecified (any) or loopback Ipv6 addresses.
                if (IgnorePort || ntohl(RelayOperation->Address[3]) > 1)
                {
                    continue;
                }
            }
            if (Message->MessageType == LxGnsMessagePortListenerRelayStart)
            {
                Relay.StartPortListener(VmId, RelayOperation->Family, RelayOperation->Port, HvSocketPort);
            }
            else
            {
                Relay.StopPortListener(RelayOperation->Family, RelayOperation->Port);
            }
            break;
        }
        case LxGnsMessagePortMappingRequest:
        {
            // TODO: handle UDP binds
            const auto* RelayOperation = gslhelpers::try_get_struct<LX_GNS_PORT_ALLOCATION_REQUEST>(Span);
            THROW_HR_IF(E_INVALIDARG, !RelayOperation);
            RESULT_MESSAGE<int32_t> Response = {};
            Response.Header.MessageType = decltype(Response)::Type;
            Response.Header.MessageSize = sizeof(Response);

            if (RelayOperation->Protocol == IPPROTO_TCP && BindsLocalhost(RelayOperation->Af, RelayOperation->Address32))
            {
                if (RelayOperation->Allocate)
                {
                    Response.Result = Relay.StartPortListener(VmId, RelayOperation->Af, RelayOperation->Port, HvSocketPort);
                }
                else
                {
                    Relay.StopPortListener(RelayOperation->Af, RelayOperation->Port);
                }
            }

            Channel.SendMessage(Response);
            break;
        }

        default:
            THROW_HR_MSG(E_UNEXPECTED, "Unexpected message %d", Message->MessageType);
        }
    }
}

wsl::windows::wslrelay::localhost::Relay::~Relay()
{
    // Iterate through each relay and set the exit event and wait for all worker threads to finish.
    auto lock = m_lock.lock_exclusive();
    for (auto const& entry : m_RelayThreads)
    {
        entry.second->ThreadContext->ExitEvent.SetEvent();
    }

    m_RelayThreads.clear();
}

int wsl::windows::wslrelay::localhost::Relay::StartPortListener(_In_ const GUID& VmId, _In_ unsigned short Family, _In_ unsigned short Port, _In_ int HvSocketPort)
try
{
    auto lock = m_lock.lock_exclusive();
    if (const auto iter = m_RelayThreads.find({Family, Port}); iter != m_RelayThreads.end())
    {
        iter->second->ThreadContext->Count++;
        return 0;
    }

    // Create a worker thread to service the port relay.
    //
    // N.B. The worker thread takes ownership of the arguments pointer.
    const auto Arguments = std::make_shared<LX_PORT_LISTENER_CONTEXT>();
    Arguments->ThreadContext = std::make_shared<LX_PORT_LISTENER_THREAD_CONTEXT>();
    Arguments->ThreadContext->VmId = VmId;
    Arguments->ThreadContext->Family = Family;
    Arguments->ThreadContext->Port = Port;
    Arguments->ThreadContext->HvSocketPort = HvSocketPort;
    Arguments->ThreadContext->ExitEvent.create(wil::EventOptions::ManualReset);
    Arguments->ThreadContext->ListenSocket = BindRelayListener(Family, Port);
    Arguments->Worker = std::thread(PortListenerAsync, Arguments->ThreadContext);

    m_RelayThreads[{Family, Port}] = Arguments;

    return 0;
}
catch (...)
{
    LOG_CAUGHT_EXCEPTION();
    switch (wil::ResultFromCaughtException())
    {
    case HRESULT_FROM_WIN32(WSAEADDRINUSE):
    case HRESULT_FROM_WIN32(WSAEACCES): // TODO: Remap and handle this next to the bind call.
        return -LX_EADDRINUSE;
    default:
        return -LX_EINVAL;
    }
}

void wsl::windows::wslrelay::localhost::Relay::StopPortListener(_In_ unsigned short Family, _In_ unsigned short Port)
{
    try
    {
        // Search through the worker threads and terminate any that match the address family and port.
        auto lock = m_lock.lock_exclusive();
        const auto iter = m_RelayThreads.find({Family, Port});
        if (iter != m_RelayThreads.end())
        {
            iter->second->ThreadContext->Count--;
            if (iter->second->ThreadContext->Count <= 0)
            {
                iter->second->ThreadContext->ExitEvent.SetEvent();
                iter->second->Worker.join();
                m_RelayThreads.erase(iter);
                WSL_LOG("PortRelayUnBind", TraceLoggingValue(Family, "family"), TraceLoggingValue(Port, "port"), TraceLoggingLevel(WINEVENT_LEVEL_VERBOSE));
            }
        }
    }
    CATCH_LOG()
}

static wil::unique_socket BindRelayListener(ADDRESS_FAMILY const Family, USHORT const Port)
{

    // Perform a mapping from Linux address family to Windows.
    const int AddressFamily = WindowsAddressFamily(Family);

    // Create a listening tcp socket on the specified port.

    wil::unique_socket ListenSocket(WSASocket(AddressFamily, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED));

    THROW_LAST_ERROR_IF(!ListenSocket);

    // Set the SO_REUSEADDR socket option.

    constexpr BOOLEAN On = true;
    THROW_LAST_ERROR_IF(setsockopt(ListenSocket.get(), SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&On), sizeof(On)) == SOCKET_ERROR);

    sockaddr* Address;
    sockaddr_in InetAddress{};
    sockaddr_in6 Inet6Address{};
    DWORD AddressSize;
    if (AddressFamily == AF_INET)
    {
        InetAddress.sin_family = AF_INET;
        InetAddress.sin_port = htons(Port);
        InetAddress.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        Address = reinterpret_cast<sockaddr*>(&InetAddress);
        AddressSize = sizeof(InetAddress);
    }
    else
    {
        Inet6Address.sin6_family = AF_INET6;
        Inet6Address.sin6_port = htons(Port);
        Inet6Address.sin6_addr = IN6ADDR_LOOPBACK_INIT;
        Address = reinterpret_cast<sockaddr*>(&Inet6Address);
        AddressSize = sizeof(Inet6Address);
    }

    // Start listening on the specified port.
    // TODO: Catch relevant bind errors WSAEACCES, WSAEADDRINUSE and throw as a new hr that will
    // end up being emitted by seccomp as EADDRINUSE.
    THROW_LAST_ERROR_IF(bind(ListenSocket.get(), Address, AddressSize) == SOCKET_ERROR);

    THROW_LAST_ERROR_IF(listen(ListenSocket.get(), -1) == SOCKET_ERROR);

    WSL_LOG("PortRelayBind", TraceLoggingValue(Family, "family"), TraceLoggingValue(Port, "port"), TraceLoggingLevel(WINEVENT_LEVEL_VERBOSE));

    return ListenSocket;
}

static VOID PortListenerAsync(_Inout_ std::shared_ptr<LX_PORT_LISTENER_THREAD_CONTEXT> Arguments)
try
{
    // Begin accepting connections until the relay is stopped.

    const int AddressFamily = WindowsAddressFamily(Arguments->Family);

    for (;;)
    {
        wil::unique_socket InetSocket(WSASocket(AddressFamily, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED));
        THROW_LAST_ERROR_IF(!InetSocket);

        wsl::windows::common::socket::Accept(Arguments->ListenSocket.get(), InetSocket.get(), INFINITE, Arguments->ExitEvent.get());

        // Establish a relay thread.

        WSL_LOG("PortRelayUsage", TraceLoggingValue(Arguments->Family, "family"), TraceLoggingValue(Arguments->Port, "port"), TraceLoggingLevel(WINEVENT_LEVEL_INFO));

        auto RelayThread = std::thread([Arguments, InetSocket = std::move(InetSocket)]() {
            try
            {
                wsl::windows::common::wslutil::SetThreadDescription(L"Port relay");
                const auto HvSocket = wsl::windows::common::hvsocket::Connect(Arguments->VmId, Arguments->HvSocketPort);
                LX_INIT_START_SOCKET_RELAY Message{};
                Message.Header.MessageType = LxInitMessageStartSocketRelay;
                Message.Header.MessageSize = sizeof(Message);
                Message.Family = Arguments->Family;
                Message.Port = Arguments->Port;
                Message.BufferSize = LOCALHOST_RELAY_BUFFER_SIZE;
                wsl::windows::common::socket::Send(HvSocket.get(), gslhelpers::struct_as_bytes(Message));
                wsl::windows::common::relay::SocketRelay(InetSocket.get(), HvSocket.get(), Message.BufferSize);
            }
            CATCH_LOG()
        });

        RelayThread.detach();
    }
}
CATCH_LOG()
