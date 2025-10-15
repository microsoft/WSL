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
inline gsl::span<gsl::byte> RecvMessage(SOCKET Socket, std::vector<gsl::byte>& Buffer, std::optional<HANDLE> ExitHandle = {}, DWORD Timeout = INFINITE)
#elif defined(__GNUC__)
inline gsl::span<gsl::byte> RecvMessage(int Socket, std::vector<gsl::byte>& Buffer, const timeval* Timeout = nullptr)
#endif
try
{
    auto MessageSize = sizeof(MESSAGE_HEADER);
    if (Buffer.size() < MessageSize)
    {
        Buffer.resize(MessageSize);
    }

    auto Message = gsl::make_span(Buffer.data(), MessageSize);
#if defined(_MSC_VER)
    auto BytesRead = wsl::windows::common::socket::Receive(Socket, Message, ExitHandle.value_or(nullptr), MSG_WAITALL, Timeout);
#elif defined(__GNUC__)
    // 'Timeout' is not implemented on Linux.
    assert(Timeout == nullptr);

    auto BytesRead = TEMP_FAILURE_RETRY(recv(Socket, Message.data(), Message.size(), MSG_WAITALL));
    THROW_LAST_ERROR_IF(BytesRead < 0);
#endif
    if (BytesRead == 0)
    {
        return {};
    }
    else if (BytesRead < MessageSize)
    {
#if defined(_MSC_VER)
        THROW_HR(E_UNEXPECTED);
#elif defined(__GNUC__)
        THROW_UNEXCEPTED();
#endif
    }

    // Grow the message buffer if needed and read the rest of the message.
    MessageSize = gslhelpers::get_struct<MESSAGE_HEADER>(Message)->MessageSize;
    if (MessageSize < sizeof(MESSAGE_HEADER))
    {
#if defined(_MSC_VER)
        THROW_HR_MSG(E_UNEXPECTED, "Unexpected message size: %llu", MessageSize);
#elif defined(__GNUC__)
        THROW_UNEXCEPTED();
#endif
    }

    if (Buffer.size() < MessageSize)
    {
        Buffer.resize(MessageSize);
    }

    Message = gsl::make_span(Buffer.data(), MessageSize).subspan(sizeof(MESSAGE_HEADER));
    while (Message.size() > 0)
    {
#if defined(_MSC_VER)
        BytesRead = wsl::windows::common::socket::Receive(Socket, Message, ExitHandle.value_or(nullptr), 0);
#elif defined(__GNUC__)
        BytesRead = TEMP_FAILURE_RETRY(recv(Socket, Message.data(), Message.size(), 0));
        THROW_LAST_ERROR_IF(BytesRead < 0);
#endif
        if (BytesRead <= 0)
        {
            const auto* Header = reinterpret_cast<const MESSAGE_HEADER*>(Buffer.data());

#if defined(_MSC_VER)

            LOG_HR_MSG(
                E_UNEXPECTED,
                "Socket closed while reading message. Size: %u, type: %i, sequence: %u",
                Header->MessageSize,
                Header->MessageType,
                Header->SequenceNumber);

#elif defined(__GNUC__)

            LOG_ERROR(
                "Socket closed while reading message. Size: {}, type: {}, sequence: {}",
                Header->MessageSize,
                Header->MessageType,
                Header->SequenceNumber);

#endif

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

} // namespace wsl::shared::socket
