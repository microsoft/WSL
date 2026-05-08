// Copyright (C) Microsoft Corporation. All rights reserved.

/*++

Module Name:

    SocketTransport.cpp

Abstract:

    Implementation of SocketTransport classes for overlapped and
    non-overlapped socket I/O.

--*/

#include "precomp.h"
#include "SocketTransport.h"
#include "socket.hpp"
#pragma hdrstop

using namespace wsl::windows::common;

// ============================================================================
// OverlappedSocketTransport
// ============================================================================

OverlappedSocketTransport::OverlappedSocketTransport(_In_ wil::unique_socket&& socket, _In_opt_ HANDLE exitEvent) :
    m_socket(std::move(socket)), m_exitEvent(exitEvent)
{
}

int OverlappedSocketTransport::Receive(
    _In_ gsl::span<gsl::byte> Buffer, _In_ DWORD Flags, _In_ DWORD Timeout, _In_ const std::source_location& Location)
{
    return socket::ReceiveNoThrow(m_socket.get(), Buffer, m_exitEvent, Flags, Timeout, Location);
}

int OverlappedSocketTransport::Send(_In_ gsl::span<const gsl::byte> Buffer, _In_ const std::source_location& Location)
{
    return socket::Send(m_socket.get(), Buffer, m_exitEvent, Location);
}

SOCKET OverlappedSocketTransport::Socket() const
{
    return m_socket.get();
}

wil::unique_socket OverlappedSocketTransport::Release()
{
    return std::move(m_socket);
}

void OverlappedSocketTransport::Close()
{
    m_socket.reset();
}

// ============================================================================
// SyncSocketTransport
// ============================================================================

SyncSocketTransport::SyncSocketTransport(_In_ wil::unique_socket&& socket, _In_opt_ HANDLE exitEvent) :
    m_socket(std::move(socket)), m_exitEvent(exitEvent)
{
    // Associate a kernel event with the socket for FD_READ, FD_WRITE, and FD_CLOSE.
    // This puts the socket into non-blocking mode as a side effect.
    THROW_LAST_ERROR_IF(
        WSAEventSelect(m_socket.get(), m_networkEvent.get(), FD_READ | FD_WRITE | FD_CLOSE) == SOCKET_ERROR);
}

bool SyncSocketTransport::WaitForSocketEvent(_In_ long eventMask, _In_ DWORD timeout, _In_ const std::source_location& location)
{
    // Drain any already-signaled network events so the event object resets.
    WSANETWORKEVENTS events{};
    WSAEnumNetworkEvents(m_socket.get(), m_networkEvent.get(), &events);

    // If the event we want is already signaled, return immediately.
    if (events.lNetworkEvents & eventMask)
    {
        // Check for errors on the events we care about.
        if ((eventMask & FD_READ) && (events.lNetworkEvents & FD_READ) && events.iErrorCode[FD_READ_BIT] != 0)
        {
            WSASetLastError(events.iErrorCode[FD_READ_BIT]);
            return false;
        }

        if ((eventMask & FD_WRITE) && (events.lNetworkEvents & FD_WRITE) && events.iErrorCode[FD_WRITE_BIT] != 0)
        {
            WSASetLastError(events.iErrorCode[FD_WRITE_BIT]);
            return false;
        }

        if (events.lNetworkEvents & FD_CLOSE)
        {
            // Connection closed by peer.
            return false;
        }

        return true;
    }

    // Wait for the socket event or exit event.
    HANDLE waitHandles[2];
    DWORD handleCount = 1;
    waitHandles[0] = m_networkEvent.get();
    if (ARGUMENT_PRESENT(m_exitEvent))
    {
        waitHandles[1] = m_exitEvent;
        handleCount = 2;
    }

    const DWORD waitResult = WaitForMultipleObjects(handleCount, waitHandles, FALSE, timeout);

    if (waitResult == WAIT_OBJECT_0)
    {
        // Socket event signaled. Drain events and check for errors.
        WSAEnumNetworkEvents(m_socket.get(), m_networkEvent.get(), &events);

        if (events.lNetworkEvents & FD_CLOSE)
        {
            return false;
        }

        if ((eventMask & FD_READ) && (events.lNetworkEvents & FD_READ) && events.iErrorCode[FD_READ_BIT] != 0)
        {
            WSASetLastError(events.iErrorCode[FD_READ_BIT]);
            return false;
        }

        if ((eventMask & FD_WRITE) && (events.lNetworkEvents & FD_WRITE) && events.iErrorCode[FD_WRITE_BIT] != 0)
        {
            WSASetLastError(events.iErrorCode[FD_WRITE_BIT]);
            return false;
        }

        return true;
    }

    if (handleCount > 1 && waitResult == WAIT_OBJECT_0 + 1)
    {
        // Exit event signaled — caller should treat as connection close.
        return false;
    }

    if (waitResult == WAIT_TIMEOUT)
    {
        WSASetLastError(WSAETIMEDOUT);
        SetLastError(static_cast<DWORD>(HCS_E_CONNECTION_TIMEOUT));
        return false;
    }

    THROW_LAST_ERROR_MSG("WaitForMultipleObjects failed. From: %hs", std::format("{}", location).c_str());
}

int SyncSocketTransport::Receive(
    _In_ gsl::span<gsl::byte> Buffer, _In_ DWORD Flags, _In_ DWORD Timeout, _In_ const std::source_location& Location)
{
    const bool waitAll = (Flags & MSG_WAITALL) != 0;
    const bool peek = (Flags & MSG_PEEK) != 0;

    // For peek operations, just do a single recv attempt with a wait.
    if (peek)
    {
        if (!WaitForSocketEvent(FD_READ, Timeout, Location))
        {
            return 0;
        }

        int result = recv(m_socket.get(), reinterpret_cast<char*>(Buffer.data()), gsl::narrow_cast<int>(Buffer.size()), MSG_PEEK);
        if (result == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK)
        {
            // Data not yet available despite event — treat as 0 bytes for peek.
            return 0;
        }

        return result;
    }

    // For non-peek receives: accumulate bytes in a loop since WSAEventSelect
    // puts the socket in non-blocking mode and MSG_WAITALL won't work.
    DWORD totalRead = 0;
    const DWORD target = gsl::narrow_cast<DWORD>(Buffer.size());

    while (totalRead < target)
    {
        int result = recv(
            m_socket.get(),
            reinterpret_cast<char*>(Buffer.data() + totalRead),
            gsl::narrow_cast<int>(target - totalRead),
            0);

        if (result > 0)
        {
            totalRead += result;

            // If not MSG_WAITALL, return after first successful recv.
            if (!waitAll)
            {
                return totalRead;
            }

            continue;
        }

        if (result == 0)
        {
            // Connection closed by peer.
            return totalRead;
        }

        // result == SOCKET_ERROR
        const int error = WSAGetLastError();
        if (error != WSAEWOULDBLOCK)
        {
            // Real error.
            if (totalRead > 0)
            {
                // Return partial data already read.
                return totalRead;
            }

            SetLastError(static_cast<DWORD>(error));
            return SOCKET_ERROR;
        }

        // WSAEWOULDBLOCK — wait for data.
        if (!WaitForSocketEvent(FD_READ, Timeout, Location))
        {
            if (totalRead > 0)
            {
                return totalRead;
            }

            return 0;
        }
    }

    return totalRead;
}

int SyncSocketTransport::Send(_In_ gsl::span<const gsl::byte> Buffer, _In_ const std::source_location& Location)
{
    DWORD offset = 0;
    const DWORD total = gsl::narrow_cast<DWORD>(Buffer.size());

    while (offset < total)
    {
        int result = send(
            m_socket.get(),
            reinterpret_cast<const char*>(Buffer.data() + offset),
            gsl::narrow_cast<int>(total - offset),
            0);

        if (result > 0)
        {
            offset += result;
            if (offset < total)
            {
                WSL_LOG("PartialSocketWrite", TraceLoggingValue(total, "MessageSize"), TraceLoggingValue(offset, "Offset"));
            }

            continue;
        }

        if (result == 0)
        {
            THROW_WIN32_MSG(ERROR_CONNECTION_ABORTED, "Socket closed during send(). From: %hs", std::format("{}", Location).c_str());
        }

        // result == SOCKET_ERROR
        const int error = WSAGetLastError();
        if (error != WSAEWOULDBLOCK)
        {
            THROW_WIN32_MSG(static_cast<DWORD>(error), "send() failed. From: %hs", std::format("{}", Location).c_str());
        }

        // WSAEWOULDBLOCK — wait for socket to become writable.
        if (!WaitForSocketEvent(FD_WRITE, INFINITE, Location))
        {
            THROW_WIN32_MSG(ERROR_CONNECTION_ABORTED, "Socket closed during send(). From: %hs", std::format("{}", Location).c_str());
        }
    }

    WI_ASSERT(offset == total);
    return offset;
}

SOCKET SyncSocketTransport::Socket() const
{
    return m_socket.get();
}

wil::unique_socket SyncSocketTransport::Release()
{
    // Undo WSAEventSelect's side effect of putting the socket in non-blocking mode.
    // Pass NULL event and 0 events to clear the association and restore blocking mode.
    if (m_socket.get() != INVALID_SOCKET)
    {
        WSAEventSelect(m_socket.get(), nullptr, 0);

        // WSAEventSelect with NULL event clears the event association but leaves
        // the socket in non-blocking mode. Use ioctlsocket to restore blocking.
        u_long nonBlocking = 0;
        ioctlsocket(m_socket.get(), FIONBIO, &nonBlocking);
    }

    return std::move(m_socket);
}

void SyncSocketTransport::Close()
{
    m_socket.reset();
}

// ============================================================================
// Factory
// ============================================================================

std::unique_ptr<SocketTransport> wsl::windows::common::CreateSocketTransport(
    _In_ wil::unique_socket&& socket, _In_opt_ HANDLE exitEvent)
{
    // Determine the address family of the socket.
    WSAPROTOCOL_INFOW protocolInfo{};
    int infoSize = sizeof(protocolInfo);
    if (getsockopt(socket.get(), SOL_SOCKET, SO_PROTOCOL_INFOW, reinterpret_cast<char*>(&protocolInfo), &infoSize) == 0)
    {
        if (protocolInfo.iAddressFamily == AF_UNIX)
        {
            return std::make_unique<SyncSocketTransport>(std::move(socket), exitEvent);
        }
    }

    return std::make_unique<OverlappedSocketTransport>(std::move(socket), exitEvent);
}
