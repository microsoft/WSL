/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    socketshared.h

Abstract:

    This file contains shared socket helper functions.

--*/

#pragma once
#include <cassert>

namespace wsl::shared::socket {

#if defined(_MSC_VER)

// RecvMessage overload that takes a ReceiveFn callable.
// ReceiveFn signature: int(gsl::span<gsl::byte> buffer, DWORD flags, DWORD timeout)
// Returns bytes read, 0 on close, or SOCKET_ERROR on error.
template <typename ReceiveFn>
inline gsl::span<gsl::byte> RecvMessageWith(ReceiveFn&& recvFn, std::vector<gsl::byte>& Buffer, DWORD Timeout = INFINITE)
try
{
    auto MessageSize = sizeof(MESSAGE_HEADER);
    if (Buffer.size() < MessageSize)
    {
        Buffer.resize(MessageSize);
    }

    auto Message = gsl::make_span(Buffer.data(), MessageSize);
    auto BytesRead = recvFn(Message, MSG_WAITALL, Timeout);
    THROW_LAST_ERROR_IF(BytesRead == SOCKET_ERROR);

    if (BytesRead == 0)
    {
        return {};
    }
    else if (BytesRead < MessageSize)
    {
        THROW_HR(E_UNEXPECTED);
    }

    // Grow the message buffer if needed and read the rest of the message.
    MessageSize = gslhelpers::get_struct<MESSAGE_HEADER>(Message)->MessageSize;
    if (MessageSize < sizeof(MESSAGE_HEADER))
    {
        THROW_HR_MSG(E_UNEXPECTED, "Unexpected message size: %llu", MessageSize);
    }

    if (MessageSize > 4 * 1024 * 1024) // 4 MiB
    {
        THROW_HR_MSG(E_UNEXPECTED, "Message size too large: %llu", MessageSize);
    }

    if (Buffer.size() < MessageSize)
    {
        Buffer.resize(MessageSize);
    }

    Message = gsl::make_span(Buffer.data(), MessageSize).subspan(sizeof(MESSAGE_HEADER));
    while (Message.size() > 0)
    {
        BytesRead = recvFn(Message, 0, INFINITE);
        THROW_LAST_ERROR_IF(BytesRead == SOCKET_ERROR);

        if (BytesRead <= 0)
        {
            const auto* Header = reinterpret_cast<const MESSAGE_HEADER*>(Buffer.data());
            LOG_HR_MSG(
                E_UNEXPECTED,
                "Socket closed while reading message. Size: %u, type: %i, id: %u",
                Header->MessageSize,
                Header->MessageType,
                Header->TransactionId);

            return {};
        }

        Message = Message.subspan(BytesRead);
    }

    return gsl::make_span(Buffer.data(), MessageSize);
}
catch (...)
{
    LOG_CAUGHT_EXCEPTION();
    errno = wil::ResultFromCaughtException();
    return {};
}

inline gsl::span<gsl::byte> RecvMessage(SOCKET Socket, std::vector<gsl::byte>& Buffer, std::optional<HANDLE> ExitHandle = {}, DWORD Timeout = INFINITE)
{
    return RecvMessageWith(
        [&](gsl::span<gsl::byte> buf, DWORD flags, DWORD timeout) {
            return wsl::windows::common::socket::ReceiveNoThrow(Socket, buf, ExitHandle.value_or(nullptr), flags, timeout);
        },
        Buffer,
        Timeout);
}

#elif defined(__GNUC__)
inline gsl::span<gsl::byte> RecvMessage(int Socket, std::vector<gsl::byte>& Buffer, const timeval* Timeout = nullptr)
try
{
    auto MessageSize = sizeof(MESSAGE_HEADER);
    if (Buffer.size() < MessageSize)
    {
        Buffer.resize(MessageSize);
    }

    auto Message = gsl::make_span(Buffer.data(), MessageSize);
    // 'Timeout' is not implemented on Linux.
    assert(Timeout == nullptr);

    auto BytesRead = TEMP_FAILURE_RETRY(recv(Socket, Message.data(), Message.size(), MSG_WAITALL));
    THROW_LAST_ERROR_IF(BytesRead < 0);

    if (BytesRead == 0)
    {
        return {};
    }
    else if (BytesRead < MessageSize)
    {
        THROW_UNEXPECTED();
    }

    // Grow the message buffer if needed and read the rest of the message.
    MessageSize = gslhelpers::get_struct<MESSAGE_HEADER>(Message)->MessageSize;
    if (MessageSize < sizeof(MESSAGE_HEADER))
    {
        THROW_UNEXPECTED();
    }

    if (MessageSize > 4 * 1024 * 1024) // 4 MiB
    {
        THROW_UNEXPECTED();
    }

    if (Buffer.size() < MessageSize)
    {
        Buffer.resize(MessageSize);
    }

    Message = gsl::make_span(Buffer.data(), MessageSize).subspan(sizeof(MESSAGE_HEADER));
    while (Message.size() > 0)
    {
        BytesRead = TEMP_FAILURE_RETRY(recv(Socket, Message.data(), Message.size(), 0));
        THROW_LAST_ERROR_IF(BytesRead < 0);

        if (BytesRead <= 0)
        {
            const auto* Header = reinterpret_cast<const MESSAGE_HEADER*>(Buffer.data());
            LOG_ERROR(
                "Socket closed while reading message. Size: {}, type: {}, id: {}",
                Header->MessageSize,
                Header->MessageType,
                Header->TransactionId);

            return {};
        }

        Message = Message.subspan(BytesRead);
    }

    return gsl::make_span(Buffer.data(), MessageSize);
}
catch (...)
{
    LOG_CAUGHT_EXCEPTION();
    errno = wil::ResultFromCaughtException();
    return {};
}
#endif

} // namespace wsl::shared::socket
