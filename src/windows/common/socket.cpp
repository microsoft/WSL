/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    socket.cpp

Abstract:

    This file contains socket helper function definitions.

--*/

#include "precomp.h"
#include <mutex>
#include "socket.hpp"
#pragma hdrstop

void wsl::windows::common::socket::Accept(_In_ SOCKET ListenSocket, _In_ SOCKET Socket, _In_ int Timeout, _In_opt_ HANDLE ExitHandle)
{
    CHAR AcceptBuffer[2 * sizeof(SOCKADDR_STORAGE)]{};
    DWORD BytesReturned;
    OVERLAPPED Overlapped{};
    const wil::unique_event OverlappedEvent(wil::EventOptions::ManualReset);
    Overlapped.hEvent = OverlappedEvent.get();
    const BOOL Success =
        AcceptEx(ListenSocket, Socket, AcceptBuffer, 0, sizeof(SOCKADDR_STORAGE), sizeof(SOCKADDR_STORAGE), &BytesReturned, &Overlapped);

    if (!Success)
    {
        GetResult(ListenSocket, Overlapped, Timeout, ExitHandle);
    }

    // Set the accept context to mark the socket as connected.
    THROW_LAST_ERROR_IF(
        setsockopt(Socket, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT, reinterpret_cast<char*>(&ListenSocket), sizeof(ListenSocket)) == SOCKET_ERROR);

    return;
}

std::pair<DWORD, DWORD> wsl::windows::common::socket::GetResult(_In_ SOCKET Socket, _In_ OVERLAPPED& Overlapped, _In_ DWORD Timeout, _In_ HANDLE ExitHandle)
{
    const int error = WSAGetLastError();
    THROW_HR_IF(HRESULT_FROM_WIN32(error), error != WSA_IO_PENDING);

    std::vector<HANDLE> waitObjects{};
    waitObjects.push_back(Overlapped.hEvent);
    if (ARGUMENT_PRESENT(ExitHandle))
    {
        waitObjects.push_back(ExitHandle);
    }

    DWORD bytesProcessed;
    DWORD flagsReturned;
    auto cancelFunction = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&] {
        CancelIoEx(reinterpret_cast<HANDLE>(Socket), &Overlapped);
        WSAGetOverlappedResult(Socket, &Overlapped, &bytesProcessed, TRUE, &flagsReturned);
    });

    const DWORD waitStatus = WaitForMultipleObjects(gsl::narrow_cast<DWORD>(waitObjects.size()), waitObjects.data(), FALSE, Timeout);
    if (waitObjects.size() > 1 && waitStatus == WAIT_OBJECT_0 + 1)
    {
        return {0, 0};
    }

    THROW_HR_IF(HCS_E_CONNECTION_TIMEOUT, (waitStatus != WAIT_OBJECT_0));

    cancelFunction.release();
    const bool result = WSAGetOverlappedResult(Socket, &Overlapped, &bytesProcessed, FALSE, &flagsReturned);
    if (!result)
    {
        const auto lastError = WSAGetLastError();
        if (lastError != WSAECONNABORTED || (ExitHandle != nullptr && WaitForSingleObject(ExitHandle, 0) == WAIT_TIMEOUT))
        {
            THROW_WIN32(lastError);
        }
        else
        {
            return {0, 0};
        }
    }
    return {bytesProcessed, flagsReturned};
}

int wsl::windows::common::socket::Receive(_In_ SOCKET Socket, _In_ gsl::span<gsl::byte> Buffer, _In_opt_ HANDLE ExitHandle, _In_ DWORD Flags, _In_ DWORD Timeout)
{
    const int BytesRead = ReceiveNoThrow(Socket, Buffer, ExitHandle, Flags, Timeout);
    THROW_LAST_ERROR_IF(BytesRead == SOCKET_ERROR);

    return BytesRead;
}

int wsl::windows::common::socket::ReceiveNoThrow(
    _In_ SOCKET Socket, _In_ gsl::span<gsl::byte> Buffer, _In_opt_ HANDLE ExitHandle, _In_ DWORD Flags, _In_ DWORD Timeout)
{
    OVERLAPPED Overlapped{};
    const wil::unique_event OverlappedEvent(wil::EventOptions::ManualReset);
    WSABUF VectorBuffer = {gsl::narrow_cast<ULONG>(Buffer.size()), reinterpret_cast<CHAR*>(Buffer.data())};
    Overlapped.hEvent = OverlappedEvent.get();
    DWORD BytesReturned{};
    if (WSARecv(Socket, &VectorBuffer, 1, &BytesReturned, &Flags, &Overlapped, nullptr) != 0)
        try
        {
            BytesReturned = SOCKET_ERROR;
            auto [innerBytes, Flags] = GetResult(Socket, Overlapped, Timeout, ExitHandle);
            BytesReturned = innerBytes;
        }
        catch (...)
        {
            LOG_CAUGHT_EXCEPTION();
            // Receive will call GetLastError to look for the error code
            SetLastError(wil::ResultFromCaughtException());
        }

    return BytesReturned;
}

std::vector<gsl::byte> wsl::windows::common::socket::Receive(_In_ SOCKET Socket, _In_opt_ HANDLE ExitHandle, _In_ DWORD Timeout)
{
    Receive(Socket, {}, ExitHandle, MSG_PEEK);

    ULONG Size = 0;
    THROW_LAST_ERROR_IF(ioctlsocket(Socket, FIONREAD, &Size) == SOCKET_ERROR);

    std::vector<gsl::byte> Buffer(Size);
    WI_VERIFY(Receive(Socket, gsl::make_span(Buffer), ExitHandle, Timeout) == static_cast<int>(Size));

    return Buffer;
}

int wsl::windows::common::socket::Send(_In_ SOCKET Socket, _In_ gsl::span<const gsl::byte> Buffer, _In_opt_ HANDLE ExitHandle)
{
    OVERLAPPED Overlapped{};
    const wil::unique_event OverlappedEvent(wil::EventOptions::ManualReset);
    WSABUF VectorBuffer = {gsl::narrow_cast<ULONG>(Buffer.size()), const_cast<CHAR*>(reinterpret_cast<const CHAR*>(Buffer.data()))};
    Overlapped.hEvent = OverlappedEvent.get();
    DWORD BytesWritten{};
    if (WSASend(Socket, &VectorBuffer, 1, &BytesWritten, 0, &Overlapped, nullptr) != 0)
    {
        DWORD Flags;
        std::tie(BytesWritten, Flags) = GetResult(Socket, Overlapped, INFINITE, ExitHandle);
    }

    WI_ASSERT(BytesWritten == gsl::narrow_cast<DWORD>(Buffer.size()));

    return BytesWritten;
}
