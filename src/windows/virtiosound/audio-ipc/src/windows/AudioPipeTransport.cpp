// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <new>
#include <utility>
#include <vector>

#include "AudioPipeTransport.hpp"

namespace wsl::virtio_sound::audio_ipc::windows {
namespace {

    class Deadline final
    {
    public:
        explicit Deadline(DWORD timeoutMs) noexcept : m_endTick(GetTickCount64() + static_cast<std::uint64_t>(timeoutMs))
        {
        }

        bool Remaining(DWORD& remainingMs) const noexcept
        {
            const ULONGLONG now = GetTickCount64();
            if (now >= m_endTick)
            {
                remainingMs = 0U;
                return false;
            }
            const std::uint64_t remaining = m_endTick - now;
            remainingMs = static_cast<DWORD>((std::min)(remaining, static_cast<std::uint64_t>(MAXDWORD)));
            return true;
        }

    private:
        std::uint64_t m_endTick;
    };

    bool IsDisconnectedError(DWORD error) noexcept
    {
        return error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED || error == ERROR_NO_DATA || error == ERROR_INVALID_HANDLE;
    }

    PipeResult ResultFromError(DWORD error) noexcept
    {
        return IsDisconnectedError(error) ? PipeResult::Disconnected : PipeResult::IoError;
    }

    void CancelAndDrain(HANDLE pipe, OVERLAPPED& operation) noexcept
    {
        (void)CancelIoEx(pipe, &operation);
        // The OVERLAPPED and its event must remain alive until the kernel reports
        // completion, including ERROR_OPERATION_ABORTED, after cancellation. This
        // drain is required for memory safety and has no hard wall-clock bound if
        // the OS stalls completion.
        const DWORD waitResult = WaitForSingleObject(operation.hEvent, INFINITE);
        if (waitResult == WAIT_OBJECT_0)
        {
            DWORD ignoredBytes = 0U;
            (void)GetOverlappedResult(pipe, &operation, &ignoredBytes, FALSE);
        }
    }

    PipeResult TransferOne(
        HANDLE pipe,
        HANDLE stopEvent,
        const Deadline& deadline,
        HANDLE operationEvent,
        bool writing,
        std::uint8_t* readDestination,
        const std::uint8_t* writeSource,
        DWORD requestedBytes,
        DWORD& transferredBytes) noexcept
    {
        transferredBytes = 0U;
        if (pipe == nullptr || pipe == INVALID_HANDLE_VALUE || requestedBytes == 0U ||
            (writing ? writeSource == nullptr : readDestination == nullptr))
        {
            return PipeResult::IoError;
        }
        if (stopEvent != nullptr)
        {
            const DWORD stopState = WaitForSingleObject(stopEvent, 0U);
            if (stopState == WAIT_OBJECT_0)
            {
                return PipeResult::Cancelled;
            }
            if (stopState == WAIT_FAILED)
            {
                return PipeResult::IoError;
            }
        }

        DWORD remainingMs = 0U;
        if (!deadline.Remaining(remainingMs))
        {
            return PipeResult::Timeout;
        }

        if (!ResetEvent(operationEvent))
        {
            return PipeResult::IoError;
        }
        OVERLAPPED operation{};
        operation.hEvent = operationEvent;

        const BOOL completedSynchronously = writing ? WriteFile(pipe, writeSource, requestedBytes, nullptr, &operation)
                                                    : ReadFile(pipe, readDestination, requestedBytes, nullptr, &operation);
        if (completedSynchronously != FALSE)
        {
            if (GetOverlappedResult(pipe, &operation, &transferredBytes, FALSE) == FALSE)
            {
                return ResultFromError(GetLastError());
            }
            if (transferredBytes == 0U)
            {
                return PipeResult::Disconnected;
            }
            return PipeResult::Success;
        }

        const DWORD startError = GetLastError();
        if (startError != ERROR_IO_PENDING)
        {
            return ResultFromError(startError);
        }

        HANDLE waitHandles[2] = {operationEvent, stopEvent};
        const DWORD handleCount = stopEvent == nullptr ? 1U : 2U;
        remainingMs = 0U;
        if (!deadline.Remaining(remainingMs))
        {
            CancelAndDrain(pipe, operation);
            return PipeResult::Timeout;
        }

        const DWORD waitResult = WaitForMultipleObjects(handleCount, waitHandles, FALSE, remainingMs);
        if (waitResult == WAIT_OBJECT_0 + 1U && handleCount == 2U)
        {
            CancelAndDrain(pipe, operation);
            return PipeResult::Cancelled;
        }
        if (waitResult == WAIT_TIMEOUT)
        {
            CancelAndDrain(pipe, operation);
            return PipeResult::Timeout;
        }
        if (waitResult != WAIT_OBJECT_0)
        {
            CancelAndDrain(pipe, operation);
            return PipeResult::IoError;
        }

        if (GetOverlappedResult(pipe, &operation, &transferredBytes, FALSE) == FALSE)
        {
            const DWORD completionError = GetLastError();
            if (completionError == ERROR_MORE_DATA && !writing && transferredBytes != 0U)
            {
                return PipeResult::Success;
            }
            if (completionError == ERROR_OPERATION_ABORTED && stopEvent != nullptr && WaitForSingleObject(stopEvent, 0U) == WAIT_OBJECT_0)
            {
                return PipeResult::Cancelled;
            }
            return ResultFromError(completionError);
        }
        if (transferredBytes == 0U)
        {
            return PipeResult::Disconnected;
        }
        return PipeResult::Success;
    }

    PipeResult ReadExact(HANDLE pipe, HANDLE stopEvent, const Deadline& deadline, HANDLE operationEvent, std::uint8_t* destination, std::size_t byteCount) noexcept
    {
        std::size_t offset = 0U;
        while (offset < byteCount)
        {
            const std::size_t remaining = byteCount - offset;
            const DWORD chunk = static_cast<DWORD>((std::min)(remaining, static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
            DWORD transferred = 0U;
            const PipeResult result =
                TransferOne(pipe, stopEvent, deadline, operationEvent, false, destination + offset, nullptr, chunk, transferred);
            if (result != PipeResult::Success)
            {
                return result;
            }
            if (transferred > chunk)
            {
                return PipeResult::IoError;
            }
            offset += transferred;
        }
        return PipeResult::Success;
    }

    PipeResult WriteExact(HANDLE pipe, HANDLE stopEvent, const Deadline& deadline, HANDLE operationEvent, const std::uint8_t* source, std::size_t byteCount) noexcept
    {
        std::size_t offset = 0U;
        while (offset < byteCount)
        {
            const std::size_t remaining = byteCount - offset;
            const DWORD chunk = static_cast<DWORD>((std::min)(remaining, static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
            DWORD transferred = 0U;
            const PipeResult result =
                TransferOne(pipe, stopEvent, deadline, operationEvent, true, nullptr, source + offset, chunk, transferred);
            if (result != PipeResult::Success)
            {
                return result;
            }
            if (transferred > chunk)
            {
                return PipeResult::IoError;
            }
            offset += transferred;
        }
        return PipeResult::Success;
    }

} // namespace

PipeResult ReadMessage(HANDLE pipe, HANDLE stopEvent, DWORD timeoutMs, Message& out) noexcept
{
    if (timeoutMs == INFINITE || pipe == nullptr || pipe == INVALID_HANDLE_VALUE)
    {
        return PipeResult::IoError;
    }

    wil::unique_event event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    if (!event)
    {
        return PipeResult::IoError;
    }
    const Deadline deadline(timeoutMs);

    std::array<std::uint8_t, c_headerBytes> wireHeader{};
    PipeResult result = ReadExact(pipe, stopEvent, deadline, event.get(), wireHeader.data(), wireHeader.size());
    if (result != PipeResult::Success)
    {
        return result;
    }

    Header header{};
    Error codecError = Error::None;
    if (!DecodeHeader(wireHeader.data(), wireHeader.size(), header, codecError))
    {
        return PipeResult::InvalidMessage;
    }

    Message decoded{};
    decoded.m_header = header;
    try
    {
        decoded.m_payload.resize(header.m_payloadBytes);
    }
    catch (...)
    {
        return PipeResult::IoError;
    }

    if (!decoded.m_payload.empty())
    {
        result = ReadExact(pipe, stopEvent, deadline, event.get(), decoded.m_payload.data(), decoded.m_payload.size());
        if (result != PipeResult::Success)
        {
            return result;
        }
    }

    if (!ValidateMessage(decoded, codecError))
    {
        return PipeResult::InvalidMessage;
    }
    out = std::move(decoded);
    return PipeResult::Success;
}

PipeResult WriteMessage(HANDLE pipe, HANDLE stopEvent, DWORD timeoutMs, const Message& message) noexcept
{
    if (timeoutMs == INFINITE || pipe == nullptr || pipe == INVALID_HANDLE_VALUE)
    {
        return PipeResult::IoError;
    }

    Error codecError = Error::None;
    if (!ValidateMessage(message, codecError))
    {
        return PipeResult::InvalidMessage;
    }

    std::vector<std::uint8_t> wire;
    try
    {
        if (!EncodeMessage(message, wire, codecError))
        {
            return PipeResult::InvalidMessage;
        }
    }
    catch (...)
    {
        return PipeResult::IoError;
    }

    wil::unique_event event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    if (!event)
    {
        return PipeResult::IoError;
    }
    const Deadline deadline(timeoutMs);
    return WriteExact(pipe, stopEvent, deadline, event.get(), wire.data(), wire.size());
}

} // namespace wsl::virtio_sound::audio_ipc::windows
