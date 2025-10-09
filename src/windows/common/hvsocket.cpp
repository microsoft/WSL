/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    hvsocket.cpp

Abstract:

    This file contains hvsocket helper function definitions.

--*/

#include "precomp.h"
#include <mutex>
#include "socket.hpp"
#include "hvsocket.hpp"
#pragma hdrstop

#define CONNECT_TIMEOUT (30 * 1000)

namespace {
void InitializeSocketAddress(_In_ const GUID& VmId, _In_ unsigned long Port, _Out_ PSOCKADDR_HV Address)
{
    RtlZeroMemory(Address, sizeof(*Address));
    Address->Family = AF_HYPERV;
    Address->VmId = VmId;
    Address->ServiceId = HV_GUID_VSOCK_TEMPLATE;
    Address->ServiceId.Data1 = Port;
}

void InitializeWildcardSocketAddress(_Out_ PSOCKADDR_HV Address)
{
    RtlZeroMemory(Address, sizeof(*Address));
    Address->Family = AF_HYPERV;
    Address->VmId = HV_GUID_WILDCARD;
    Address->ServiceId = HV_GUID_WILDCARD;
}
} // namespace

wil::unique_socket wsl::windows::common::hvsocket::Accept(
    _In_ SOCKET ListenSocket, _In_ int Timeout, _In_opt_ HANDLE ExitHandle, _In_ const std::source_location& Location)
{
    wil::unique_socket Socket = Create();
    wsl::windows::common::socket::Accept(ListenSocket, Socket.get(), Timeout, ExitHandle, Location);

    return Socket;
}

wil::unique_socket wsl::windows::common::hvsocket::Connect(
    _In_ const GUID& VmId, _In_ unsigned long Port, _In_opt_ HANDLE ExitHandle, _In_ const std::source_location& Location)
{
    OVERLAPPED Overlapped{};
    const wil::unique_event OverlappedEvent(wil::EventOptions::ManualReset);
    Overlapped.hEvent = OverlappedEvent.get();

    auto Socket = Create();

    static constexpr GUID ConnectExGuid = WSAID_CONNECTEX;
    LPFN_CONNECTEX ConnectFn{};
    DWORD BytesReturned;
    const auto Result = WSAIoctl(
        Socket.get(),
        SIO_GET_EXTENSION_FUNCTION_POINTER,
        const_cast<GUID*>(&ConnectExGuid),
        sizeof(ConnectExGuid),
        &ConnectFn,
        sizeof(ConnectFn),
        &BytesReturned,
        &Overlapped,
        nullptr);

    if (Result != 0)
    {
        socket::GetResult(Socket.get(), Overlapped, INFINITE, ExitHandle, Location);
    }

    ULONG Timeout = CONNECT_TIMEOUT;
    THROW_LAST_ERROR_IF(
        setsockopt(Socket.get(), HV_PROTOCOL_RAW, HVSOCKET_CONNECT_TIMEOUT, reinterpret_cast<char*>(&Timeout), sizeof(Timeout)) == SOCKET_ERROR);

    SOCKADDR_HV Addr;
    InitializeWildcardSocketAddress(&Addr);
    THROW_LAST_ERROR_IF(bind(Socket.get(), reinterpret_cast<sockaddr*>(&Addr), sizeof(Addr)) == SOCKET_ERROR);
    InitializeSocketAddress(VmId, Port, &Addr);
    OverlappedEvent.ResetEvent();
    const BOOL Success = ConnectFn(Socket.get(), reinterpret_cast<sockaddr*>(&Addr), sizeof(Addr), nullptr, 0, nullptr, &Overlapped);
    if (Success == FALSE)
    {
        socket::GetResult(Socket.get(), Overlapped, INFINITE, ExitHandle, Location);
    }

    return Socket;
}

wil::unique_socket wsl::windows::common::hvsocket::Create()
{
    wil::unique_socket Socket(WSASocket(AF_HYPERV, SOCK_STREAM, HV_PROTOCOL_RAW, nullptr, 0, WSA_FLAG_OVERLAPPED));
    THROW_LAST_ERROR_IF(!Socket);

    ULONG Enable = 1;
    THROW_LAST_ERROR_IF(
        setsockopt(Socket.get(), HV_PROTOCOL_RAW, HVSOCKET_CONNECTED_SUSPEND, reinterpret_cast<char*>(&Enable), sizeof(Enable)) == SOCKET_ERROR);

    return Socket;
}

wil::unique_socket wsl::windows::common::hvsocket::Listen(_In_ const GUID& VmId, _In_ unsigned long Port, _In_ int Backlog)
{
    SOCKADDR_HV Addr;
    InitializeSocketAddress(VmId, Port, &Addr);
    auto Socket = Create();
    THROW_LAST_ERROR_IF(bind(Socket.get(), reinterpret_cast<sockaddr*>(&Addr), sizeof(Addr)) == SOCKET_ERROR);
    THROW_LAST_ERROR_IF(listen(Socket.get(), Backlog) == SOCKET_ERROR);
    return Socket;
}
