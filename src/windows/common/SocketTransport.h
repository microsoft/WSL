// Copyright (C) Microsoft Corporation. All rights reserved.

/*++

Module Name:

    SocketTransport.h

Abstract:

    This file defines the SocketTransport abstraction for socket I/O.

    SocketTransport provides a common interface for sending and receiving data
    on sockets that may or may not support overlapped I/O. This allows
    SocketChannel to work transparently with both AF_HYPERV sockets (which
    support overlapped I/O) and AF_UNIX sockets (which do not).

    Two implementations are provided:
    - OverlappedSocketTransport: Uses WSARecv/WSASend with OVERLAPPED for
      sockets that support overlapped I/O (AF_HYPERV, AF_INET, etc.)
    - SyncSocketTransport: Uses WSAEventSelect + synchronous recv/send for
      sockets that do not support overlapped I/O (AF_UNIX)

    The CreateSocketTransport() factory auto-detects the socket type.

--*/

#pragma once

#include <wil/resource.h>

namespace wsl::windows::common {

// Abstract base class for socket I/O transport.
//
// Implementations handle the difference between overlapped and non-overlapped
// sockets, providing a uniform interface for SocketChannel.
class SocketTransport
{
public:
    NON_COPYABLE(SocketTransport);
    NON_MOVABLE(SocketTransport);

    SocketTransport() = default;
    virtual ~SocketTransport() = default;

    // Receive data from the socket into Buffer.
    // Returns the number of bytes read, 0 on connection close / exit event,
    // or SOCKET_ERROR on failure (with last error set).
    virtual int Receive(
        _In_ gsl::span<gsl::byte> Buffer,
        _In_ DWORD Flags = MSG_WAITALL,
        _In_ DWORD Timeout = INFINITE,
        _In_ const std::source_location& Location = std::source_location::current()) = 0;

    // Send all bytes in Buffer to the socket.
    // Returns the number of bytes sent.
    // Throws on failure or if the exit event is signaled.
    virtual int Send(
        _In_ gsl::span<const gsl::byte> Buffer,
        _In_ const std::source_location& Location = std::source_location::current()) = 0;

    // Returns the underlying SOCKET handle (non-owning).
    virtual SOCKET Socket() const = 0;

    // Release ownership of the underlying socket and return it.
    virtual wil::unique_socket Release() = 0;

    // Close the underlying socket.
    virtual void Close() = 0;
};

// Transport for sockets that support overlapped I/O (AF_HYPERV, AF_INET, etc.).
// Uses WSARecv/WSASend with OVERLAPPED structures and WaitForMultipleObjects
// for cancellation and timeout support.
class OverlappedSocketTransport final : public SocketTransport
{
public:
    OverlappedSocketTransport(_In_ wil::unique_socket&& socket, _In_opt_ HANDLE exitEvent);

    int Receive(
        _In_ gsl::span<gsl::byte> Buffer,
        _In_ DWORD Flags = MSG_WAITALL,
        _In_ DWORD Timeout = INFINITE,
        _In_ const std::source_location& Location = std::source_location::current()) override;

    int Send(
        _In_ gsl::span<const gsl::byte> Buffer,
        _In_ const std::source_location& Location = std::source_location::current()) override;

    SOCKET Socket() const override;
    wil::unique_socket Release() override;
    void Close() override;

private:
    wil::unique_socket m_socket;
    HANDLE m_exitEvent{};
};

// Transport for sockets that do NOT support overlapped I/O (AF_UNIX).
// Uses WSAEventSelect to associate a kernel event with the socket, then
// WaitForMultipleObjects for cancellation/timeout, and synchronous recv/send.
//
// Note: WSAEventSelect puts the socket into non-blocking mode. This means
// recv() may return WSAEWOULDBLOCK even when MSG_WAITALL is specified.
// The implementation manually accumulates bytes in a loop to emulate
// MSG_WAITALL semantics.
class SyncSocketTransport final : public SocketTransport
{
public:
    SyncSocketTransport(_In_ wil::unique_socket&& socket, _In_opt_ HANDLE exitEvent);

    int Receive(
        _In_ gsl::span<gsl::byte> Buffer,
        _In_ DWORD Flags = MSG_WAITALL,
        _In_ DWORD Timeout = INFINITE,
        _In_ const std::source_location& Location = std::source_location::current()) override;

    int Send(
        _In_ gsl::span<const gsl::byte> Buffer,
        _In_ const std::source_location& Location = std::source_location::current()) override;

    SOCKET Socket() const override;
    wil::unique_socket Release() override;
    void Close() override;

private:
    // Wait for the socket to become readable or writable, or for the exit
    // event to be signaled. Returns true if the socket is ready, false if
    // the exit event was signaled or timeout expired.
    bool WaitForSocketEvent(_In_ long eventMask, _In_ DWORD timeout, _In_ const std::source_location& location);

    wil::unique_socket m_socket;
    HANDLE m_exitEvent{};
    wil::unique_event m_networkEvent{wil::EventOptions::ManualReset};
};

// Factory: creates the appropriate SocketTransport based on the socket's
// address family. AF_UNIX sockets get SyncSocketTransport; all others get
// OverlappedSocketTransport.
std::unique_ptr<SocketTransport> CreateSocketTransport(
    _In_ wil::unique_socket&& socket,
    _In_opt_ HANDLE exitEvent = nullptr);

} // namespace wsl::windows::common
