/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    PortRelayHandle.cpp

Abstract:

    Contains the implementation of the PortRelayAcceptHandle class.

--*/

#include "PortRelayHandle.h"
#include "IORelay.h"
#include "hvsocket.hpp"
#include "socket.hpp"
#include "wslutil.h"
#include "lxinitshared.h"
#include <gslhelpers.h>
#include <mswsock.h>
#include <thread>

using namespace wsl::windows::service::wslc;
using namespace wsl::windows::common;

PortRelayAcceptHandle::PortRelayAcceptHandle(
    wil::unique_socket&& ListenSocket, const GUID& VmId, uint32_t RelayPort, uint32_t LinuxPort, int Family, IORelay& IoRelay) :
    ListenSocket(std::move(ListenSocket)), VmId(VmId), RelayPort(RelayPort), LinuxPort(LinuxPort), Family(Family), IoRelay(IoRelay)
{
    Overlapped.hEvent = Event.get();
}

PortRelayAcceptHandle::~PortRelayAcceptHandle()
{
    if (State == relay::IOHandleStatus::Pending)
    {
        LOG_IF_WIN32_BOOL_FALSE(CancelIoEx(reinterpret_cast<HANDLE>(ListenSocket.get()), &Overlapped));

        DWORD bytesProcessed{};
        DWORD flagsReturned{};
        if (!WSAGetOverlappedResult(ListenSocket.get(), &Overlapped, &bytesProcessed, TRUE, &flagsReturned))
        {
            auto error = GetLastError();
            LOG_LAST_ERROR_IF(error != ERROR_CONNECTION_ABORTED && error != ERROR_OPERATION_ABORTED);
        }
    }
}

void PortRelayAcceptHandle::Schedule()
{
    WI_ASSERT(State == relay::IOHandleStatus::Standby);

    // Create a new socket for accepting
    AcceptedSocket.reset(WSASocket(Family, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED));
    THROW_LAST_ERROR_IF(!AcceptedSocket);

    memset(AcceptBuffer, 0, sizeof(AcceptBuffer));
    DWORD bytesReturned{};
    if (AcceptEx(ListenSocket.get(), AcceptedSocket.get(), AcceptBuffer, 0, sizeof(SOCKADDR_STORAGE), sizeof(SOCKADDR_STORAGE), &bytesReturned, &Overlapped))
    {
        // Accept completed immediately
        State = relay::IOHandleStatus::Completed;
    }
    else
    {
        auto error = WSAGetLastError();
        THROW_HR_IF_MSG(HRESULT_FROM_WIN32(error), error != ERROR_IO_PENDING, "Handle: 0x%p", reinterpret_cast<void*>(ListenSocket.get()));

        State = relay::IOHandleStatus::Pending;
    }
}

void PortRelayAcceptHandle::Collect()
{
    WI_ASSERT(State == relay::IOHandleStatus::Pending || State == relay::IOHandleStatus::Completed);

    if (State == relay::IOHandleStatus::Pending)
    {
        DWORD bytesReceived{};
        DWORD flagsReturned{};
        THROW_IF_WIN32_BOOL_FALSE(WSAGetOverlappedResult(ListenSocket.get(), &Overlapped, &bytesReceived, false, &flagsReturned));
    }

    // Launch a relay for this accepted connection
    LaunchRelay(std::move(AcceptedSocket));

    // Go back to standby to accept the next connection
    State = relay::IOHandleStatus::Standby;
}

HANDLE PortRelayAcceptHandle::GetHandle() const
{
    return Event.get();
}

void PortRelayAcceptHandle::LaunchRelay(wil::unique_socket&& AcceptedSocket)
{
    WSL_LOG(
        "StartPortRelay",
        TraceLoggingValue(LinuxPort, "LinuxPort"),
        TraceLoggingValue(Family, "Family"),
        TraceLoggingValue(AcceptedSocket.get(), "Socket"));

    // Launch relay in a dedicated thread
    std::thread relayThread{
        [Socket = std::move(AcceptedSocket), VmId = VmId, LinuxPort = LinuxPort, RelayPort = RelayPort, Family = Family]() mutable {
            try
            {
                wslutil::SetThreadDescription(L"Port relay");

                // Connect to the HvSocket
                auto hvSocket = hvsocket::Connect(VmId, RelayPort);

                // Send relay start message
                LX_INIT_START_SOCKET_RELAY message{};
                message.Header.MessageType = LxInitMessageStartSocketRelay;
                message.Header.MessageSize = sizeof(message);
                message.Family = (Family == AF_INET) ? LX_AF_INET : LX_AF_INET6;
                message.Port = LinuxPort;
                message.BufferSize = 0x20000; // LOCALHOST_RELAY_BUFFER_SIZE

                socket::Send(hvSocket.get(), gslhelpers::struct_as_bytes(message));

                // Relay data between the two sockets
                relay::SocketRelay(Socket.get(), hvSocket.get(), message.BufferSize);

                WSL_LOG("StopPortRelay", TraceLoggingValue(LinuxPort, "LinuxPort"), TraceLoggingValue(Socket.get(), "Socket"));
            }
            CATCH_LOG();
        }};

    relayThread.detach();
}
