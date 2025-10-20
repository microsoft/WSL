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

void wsl::windows::common::socket::Accept(
    _In_ SOCKET ListenSocket, _In_ SOCKET Socket, _In_ int Timeout, _In_opt_ HANDLE ExitHandle, _In_ const std::source_location& Location)
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
        GetResult(ListenSocket, Overlapped, Timeout, ExitHandle, Location);
    }

    // Set the accept context to mark the socket as connected.
    THROW_LAST_ERROR_IF_MSG(
        setsockopt(Socket, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT, reinterpret_cast<char*>(&ListenSocket), sizeof(ListenSocket)) == SOCKET_ERROR,
        "From: %hs",
        std::format("{}", Location).c_str());

    return;
}

std::pair<DWORD, DWORD> wsl::windows::common::socket::GetResult(
    _In_ SOCKET Socket, _In_ OVERLAPPED& Overlapped, _In_ DWORD Timeout, _In_ HANDLE ExitHandle, _In_ const std::source_location& Location)
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

    THROW_HR_IF_MSG(HCS_E_CONNECTION_TIMEOUT, (waitStatus != WAIT_OBJECT_0), "From: %hs", std::format("{}", Location).c_str());

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

int wsl::windows::common::socket::Receive(
    _In_ SOCKET Socket, _In_ gsl::span<gsl::byte> Buffer, _In_opt_ HANDLE ExitHandle, _In_ DWORD Flags, _In_ DWORD Timeout, _In_ const std::source_location& Location)
{
    const int BytesRead = ReceiveNoThrow(Socket, Buffer, ExitHandle, Flags, Timeout, Location);
    THROW_LAST_ERROR_IF(BytesRead == SOCKET_ERROR);

    return BytesRead;
}

int wsl::windows::common::socket::ReceiveNoThrow(
    _In_ SOCKET Socket, _In_ gsl::span<gsl::byte> Buffer, _In_opt_ HANDLE ExitHandle, _In_ DWORD Flags, _In_ DWORD Timeout, _In_ const std::source_location& Location)
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
            auto [innerBytes, Flags] = GetResult(Socket, Overlapped, Timeout, ExitHandle, Location);
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

std::vector<gsl::byte> wsl::windows::common::socket::Receive(
    _In_ SOCKET Socket, _In_opt_ HANDLE ExitHandle, _In_ DWORD Timeout, _In_ const std::source_location& Location)
{
    Receive(Socket, {}, ExitHandle, MSG_PEEK, Timeout, Location);

    ULONG Size = 0;
    THROW_LAST_ERROR_IF(ioctlsocket(Socket, FIONREAD, &Size) == SOCKET_ERROR);

    std::vector<gsl::byte> Buffer(Size);
    WI_VERIFY(Receive(Socket, gsl::make_span(Buffer), ExitHandle, MSG_WAITALL, Timeout, Location) == static_cast<int>(Size));

    return Buffer;
}

int wsl::windows::common::socket::Send(
    _In_ SOCKET Socket, _In_ gsl::span<const gsl::byte> Buffer, _In_opt_ HANDLE ExitHandle, _In_ const std::source_location& Location)
{
    const wil::unique_event OverlappedEvent(wil::EventOptions::ManualReset);
    OVERLAPPED Overlapped{};
    Overlapped.hEvent = OverlappedEvent.get();

    DWORD Offset = 0;
    while (Offset < Buffer.size())
    {
        OverlappedEvent.ResetEvent();

        WSABUF VectorBuffer = {
            gsl::narrow_cast<ULONG>(Buffer.size() - Offset), const_cast<CHAR*>(reinterpret_cast<const CHAR*>(Buffer.data() + Offset))};

        DWORD BytesWritten{};
        if (WSASend(Socket, &VectorBuffer, 1, &BytesWritten, 0, &Overlapped, nullptr) != 0)
        {
            // If WSASend returns non-zero, expect WSA_IO_PENDING.
            if (auto error = WSAGetLastError(); error != WSA_IO_PENDING)
            {
                THROW_WIN32_MSG(error, "WSASend failed. From: %hs", std::format("{}", Location).c_str());
            }

            DWORD Flags;
            std::tie(BytesWritten, Flags) = GetResult(Socket, Overlapped, INFINITE, ExitHandle, Location);
            if (BytesWritten == 0)
            {
                THROW_WIN32_MSG(ERROR_CONNECTION_ABORTED, "Socket closed during WSASend(). From: %hs", std::format("{}", Location).c_str());
            }
        }

        Offset += BytesWritten;
        if (Offset < Buffer.size())
        {
            WSL_LOG("PartialSocketWrite", TraceLoggingValue(Buffer.size(), "MessagSize"), TraceLoggingValue(Offset, "Offset"));
        }
    }

    WI_ASSERT(Offset == gsl::narrow_cast<DWORD>(Buffer.size()));

    return Offset;
}
