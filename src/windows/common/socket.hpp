/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    socket.hpp

Abstract:

    This file contains socket helper function declarations.

--*/

#pragma once

#include <wil/resource.h>

namespace wsl::windows::common::socket {

// Sets SO_UPDATE_ACCEPT_CONTEXT on a socket accepted via AcceptEx to mark it as connected.
void SetAcceptContext(_In_ SOCKET AcceptedSocket, _In_ SOCKET ListenSocket, _In_ const std::source_location& Location = std::source_location::current());

std::optional<wil::unique_socket> CancellableAccept(
    _In_ SOCKET ListenSocket,
    _In_ DWORD Timeout,
    _In_opt_ HANDLE ExitHandle,
    _In_ const std::source_location& Location = std::source_location::current());

std::pair<DWORD, DWORD> GetResult(
    _In_ SOCKET Socket, _In_ OVERLAPPED& Overlapped, _In_ DWORD Timeout, _In_ HANDLE ExitHandle, _In_ const std::source_location& Location);

int Receive(
    _In_ SOCKET Socket,
    _In_ gsl::span<gsl::byte> Buffer,
    _In_opt_ HANDLE ExitHandle = nullptr,
    _In_ DWORD Flags = MSG_WAITALL,
    _In_ DWORD Timeout = INFINITE,
    _In_ const std::source_location& Location = std::source_location::current());

std::vector<gsl::byte> Receive(
    _In_ SOCKET Socket,
    _In_opt_ HANDLE ExitHandle = nullptr,
    _In_ DWORD Timeout = INFINITE,
    _In_ const std::source_location& Location = std::source_location::current());

int ReceiveNoThrow(
    _In_ SOCKET Socket,
    _In_ gsl::span<gsl::byte> Buffer,
    _In_opt_ HANDLE ExitHandle = nullptr,
    _In_ DWORD Flags = MSG_WAITALL,
    _In_ DWORD Timeout = INFINITE,
    _In_ const std::source_location& Location = std::source_location::current());

int Send(
    _In_ SOCKET Socket,
    _In_ gsl::span<const gsl::byte> Buffer,
    _In_opt_ HANDLE ExitHandle = nullptr,
    _In_ const std::source_location& Location = std::source_location::current());

} // namespace wsl::windows::common::socket
