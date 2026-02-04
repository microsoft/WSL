/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    relay.cpp

Abstract:

    This file contains function definitions for relay worker thread routines.

--*/

#include "precomp.h"
#include "relay.hpp"
#pragma hdrstop

using wsl::windows::common::relay::EventHandle;
using wsl::windows::common::relay::HandleWrapper;
using wsl::windows::common::relay::IOHandleStatus;
using wsl::windows::common::relay::MultiHandleWait;
using wsl::windows::common::relay::OverlappedIOHandle;
using wsl::windows::common::relay::ScopedMultiRelay;
using wsl::windows::common::relay::ScopedRelay;
using wsl::windows::common::relay::SingleAcceptHandle;

namespace {

LARGE_INTEGER InitializeFileOffset(HANDLE File)
{
    LARGE_INTEGER Offset{};
    if (GetFileType(File) == FILE_TYPE_DISK)
    {
        LOG_IF_WIN32_BOOL_FALSE(SetFilePointerEx(File, {}, &Offset, FILE_CURRENT));
    }

    return Offset;
}

} // namespace

std::thread wsl::windows::common::relay::CreateThread(_In_ HANDLE InputHandle, _In_ HANDLE OutputHandle, _In_opt_ HANDLE ExitHandle, _In_ size_t BufferSize)
{
    return std::thread([InputHandle, OutputHandle, ExitHandle, BufferSize]() {
        try
        {
            wsl::windows::common::wslutil::SetThreadDescription(L"IO Relay");
            InterruptableRelay(InputHandle, OutputHandle, ExitHandle, BufferSize);
        }
        CATCH_LOG()
    });
}

std::thread wsl::windows::common::relay::CreateThread(
    _In_ wil::unique_handle&& InputHandle, _In_ HANDLE OutputHandle, _In_opt_ HANDLE ExitHandle, _In_ size_t BufferSize)
{
    return std::thread([InputHandle = std::move(InputHandle), OutputHandle, ExitHandle, BufferSize]() {
        try
        {
            wsl::windows::common::wslutil::SetThreadDescription(L"IO Relay");
            InterruptableRelay(InputHandle.get(), OutputHandle, ExitHandle, BufferSize);
        }
        CATCH_LOG()
    });
}

std::thread wsl::windows::common::relay::CreateThread(
    _In_ HANDLE InputHandle, _In_ wil::unique_handle&& OutputHandle, _In_opt_ HANDLE ExitHandle, _In_ size_t BufferSize)
{
    return std::thread([InputHandle, OutputHandle = std::move(OutputHandle), ExitHandle, BufferSize]() {
        try
        {
            wsl::windows::common::wslutil::SetThreadDescription(L"IO Relay");
            InterruptableRelay(InputHandle, OutputHandle.get(), ExitHandle, BufferSize);
        }
        CATCH_LOG()
    });
}

std::thread wsl::windows::common::relay::CreateThread(
    _In_ wil::unique_handle&& InputHandle, _In_ wil::unique_handle&& OutputHandle, _In_opt_ HANDLE ExitHandle, _In_ size_t BufferSize)
{
    return std::thread([InputHandle = std::move(InputHandle), OutputHandle = std::move(OutputHandle), ExitHandle, BufferSize]() {
        try
        {
            wsl::windows::common::wslutil::SetThreadDescription(L"IO Relay");
            InterruptableRelay(InputHandle.get(), OutputHandle.get(), ExitHandle, BufferSize);
        }
        CATCH_LOG()
    });
}

DWORD
wsl::windows::common::relay::InterruptableRead(
    _In_ HANDLE InputHandle, _In_ gsl::span<gsl::byte> Buffer, _In_ const std::vector<HANDLE>& ExitHandles, _In_opt_ LPOVERLAPPED Overlapped)
{
    // Initialize an overlapped structure if one was not provided by the caller.
    OVERLAPPED overlapped = {};
    wil::unique_event overlappedEvent = {};
    if (!ARGUMENT_PRESENT(Overlapped))
    {
        overlappedEvent.create(wil::EventOptions::ManualReset);
        overlapped.hEvent = overlappedEvent.get();
        Overlapped = &overlapped;
    }

    DWORD bytesRead = 0;
    if (!ReadFile(InputHandle, Buffer.data(), gsl::narrow_cast<DWORD>(Buffer.size()), &bytesRead, Overlapped))
    {
        auto lastError = GetLastError();
        if ((lastError == ERROR_HANDLE_EOF) || (lastError == ERROR_BROKEN_PIPE))
        {
            return 0;
        }

        THROW_LAST_ERROR_IF_MSG(lastError != ERROR_IO_PENDING, "Handle: 0x%p", (void*)InputHandle);

        auto cancelRead = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&] {
            CancelIoEx(InputHandle, Overlapped);
            GetOverlappedResult(InputHandle, Overlapped, &bytesRead, TRUE);
        });

        // Wait for the read to complete, or the client to exit.
        if (!InterruptableWait(Overlapped->hEvent, ExitHandles))
        {
            return 0;
        }

        if (!GetOverlappedResult(InputHandle, Overlapped, &bytesRead, FALSE))
        {
            lastError = GetLastError();
            if ((lastError == ERROR_HANDLE_EOF) || (lastError == ERROR_BROKEN_PIPE))
            {
                return 0;
            }

            THROW_LAST_ERROR();
        }

        cancelRead.release();
    }

    return bytesRead;
}

void wsl::windows::common::relay::InterruptableRelay(_In_ HANDLE InputHandle, _In_opt_ HANDLE OutputHandle, _In_opt_ HANDLE ExitHandle, _In_ size_t BufferSize)
{
    // If the handle file is seekable, make sure to respect the offset.
    // This is useful in cases when WSL is invoked on an existing file, like: wsl.exe echo foo >> file
    // See: https://github.com/microsoft/WSL/issues/11799

    LARGE_INTEGER writeOffset = InitializeFileOffset(OutputHandle);
    LARGE_INTEGER readOffset = InitializeFileOffset(InputHandle);

    std::vector<gsl::byte> buffer(BufferSize);
    const auto readSpan = gsl::make_span(buffer);

    std::vector<HANDLE> exitHandles;
    if (ExitHandle)
    {
        exitHandles.push_back(ExitHandle);
    }

    OVERLAPPED overlapped = {0};
    const wil::unique_event overlappedEvent(wil::EventOptions::ManualReset);
    overlapped.hEvent = overlappedEvent.get();
    for (;;)
    {
        overlapped.Offset = readOffset.LowPart;
        overlapped.OffsetHigh = readOffset.HighPart;
        const auto bytesRead = InterruptableRead(InputHandle, readSpan, exitHandles, &overlapped);
        if (bytesRead == 0)
        {
            break;
        }

        readOffset.QuadPart += bytesRead;

        if (OutputHandle)
        {
            overlapped.Offset = writeOffset.LowPart;
            overlapped.OffsetHigh = writeOffset.HighPart;
            auto writeSpan = readSpan.first(bytesRead);
            const auto bytesWritten = InterruptableWrite(OutputHandle, writeSpan, exitHandles, &overlapped);
            if (bytesWritten == 0)
            {
                break;
            }

            WI_ASSERT(bytesWritten == bytesRead);
        }

        writeOffset.QuadPart += bytesRead;
    }
}

bool wsl::windows::common::relay::InterruptableWait(_In_ HANDLE WaitObject, _In_ const std::vector<HANDLE>& ExitHandles)
{
    // Wait for the object to become signaled or one of the exit handles to be signaled.
    std::vector<HANDLE> waitObjects{WaitObject};
    for (const auto& exitHandle : ExitHandles)
    {
        waitObjects.push_back(exitHandle);
    }

    const DWORD waitResult = WaitForMultipleObjects(gsl::narrow_cast<DWORD>(waitObjects.size()), waitObjects.data(), FALSE, INFINITE);
    if (waitResult != WAIT_OBJECT_0)
    {
        if (waitResult > WAIT_OBJECT_0 && waitResult <= WAIT_OBJECT_0 + waitObjects.size())
        {
            return false;
        }

        THROW_HR_MSG(E_FAIL, "WaitForMultipleObjects %d", waitResult);
    }

    return true;
}

DWORD
wsl::windows::common::relay::InterruptableWrite(
    _In_ HANDLE OutputHandle, _In_ gsl::span<const gsl::byte> Buffer, _In_ const std::vector<HANDLE>& ExitHandles, _In_ LPOVERLAPPED Overlapped)
{
    const DWORD bytesToWrite = gsl::narrow_cast<DWORD>(Buffer.size());
    DWORD bytesWritten = 0;
    BOOL success = WriteFile(OutputHandle, Buffer.data(), bytesToWrite, &bytesWritten, Overlapped);
    if (!success)
    {
        const auto lastError = GetLastError();
        if (lastError == ERROR_NO_DATA)
        {
            return 0;
        }

        THROW_LAST_ERROR_IF(lastError != ERROR_IO_PENDING);

        auto cancelWrite = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&] {
            CancelIoEx(OutputHandle, Overlapped);
            GetOverlappedResult(OutputHandle, Overlapped, &bytesWritten, TRUE);
        });

        if (InterruptableWait(Overlapped->hEvent, ExitHandles))
        {
            success = GetOverlappedResult(OutputHandle, Overlapped, &bytesWritten, FALSE);
            if (success)
            {
                cancelWrite.release();
            }
        }
    }

    WI_ASSERT(!success || (bytesWritten == bytesToWrite));

    return bytesWritten;
}

void wsl::windows::common::relay::BidirectionalRelay(_In_ HANDLE LeftHandle, _In_ HANDLE RightHandle, _In_ size_t BufferSize, _In_ RelayFlags Flags)
{
    std::vector<gsl::byte> leftBuffer(BufferSize);
    const auto leftReadSpan = gsl::make_span(leftBuffer);
    OVERLAPPED leftOverlapped = {0};
    const wil::unique_event leftOverlappedEvent(wil::EventOptions::None);
    leftOverlapped.hEvent = leftOverlappedEvent.get();
    LARGE_INTEGER leftOffset{};

    std::vector<gsl::byte> rightBuffer(BufferSize);
    const auto rightReadSpan = gsl::make_span(rightBuffer);
    OVERLAPPED rightOverlapped = {0};
    const wil::unique_event rightOverlappedEvent(wil::EventOptions::None);
    rightOverlapped.hEvent = rightOverlappedEvent.get();
    LARGE_INTEGER rightOffset{};

    bool leftReadPending = false;
    bool rightReadPending = false;
    auto cancelReads = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&] {
        DWORD bytes;
        if (leftReadPending)
        {
            CancelIoEx(LeftHandle, &leftOverlapped);
            GetOverlappedResult(LeftHandle, &leftOverlapped, &bytes, TRUE);
        }

        if (rightReadPending)
        {
            CancelIoEx(RightHandle, &rightOverlapped);
            GetOverlappedResult(RightHandle, &rightOverlapped, &bytes, TRUE);
        }
    });

    DWORD bytesWritten;
    const HANDLE waitObjects[] = {leftOverlapped.hEvent, rightOverlapped.hEvent};
    for (;;)
    {
        if ((LeftHandle == nullptr) || (RightHandle == nullptr))
        {
            break;
        }

        DWORD leftBytesRead = 0;
        if (!leftReadPending && LeftHandle)
        {
            if (!ReadFile(LeftHandle, leftReadSpan.data(), gsl::narrow_cast<DWORD>(leftReadSpan.size()), &leftBytesRead, &leftOverlapped))
            {
                THROW_LAST_ERROR_IF(GetLastError() != ERROR_IO_PENDING);
            }

            leftReadPending = true;
        }

        DWORD rightBytesRead = 0;
        if (!rightReadPending && RightHandle)
        {
            if (!ReadFile(RightHandle, rightReadSpan.data(), gsl::narrow_cast<DWORD>(rightReadSpan.size()), &rightBytesRead, &rightOverlapped))
            {
                THROW_LAST_ERROR_IF(GetLastError() != ERROR_IO_PENDING);
            }

            rightReadPending = true;
        }

        const DWORD waitResult = WaitForMultipleObjects(RTL_NUMBER_OF(waitObjects), waitObjects, FALSE, INFINITE);
        if (waitResult == WAIT_OBJECT_0)
        {
            LOG_LAST_ERROR_IF_MSG(
                !GetOverlappedResult(LeftHandle, &leftOverlapped, &leftBytesRead, FALSE), "WSAGetLastError %d", WSAGetLastError());

            leftReadPending = false;
            if (leftBytesRead == 0)
            {
                LeftHandle = nullptr;
                if (WI_IsFlagSet(Flags, RelayFlags::RightIsSocket))
                {
                    LOG_LAST_ERROR_IF(shutdown(reinterpret_cast<SOCKET>(RightHandle), SD_SEND) == SOCKET_ERROR);
                }
            }
            else if (RightHandle != nullptr)
            {
                auto writeSpan = leftReadSpan.first(leftBytesRead);
                bytesWritten = InterruptableWrite(RightHandle, writeSpan, {}, &leftOverlapped);
                if (bytesWritten == 0)
                {
                    break;
                }

                leftOffset.QuadPart += leftBytesRead;
                leftOverlapped.Offset = leftOffset.LowPart;
                leftOverlapped.OffsetHigh = leftOffset.HighPart;
            }
        }
        else if (waitResult == (WAIT_OBJECT_0 + 1))
        {
            LOG_LAST_ERROR_IF_MSG(
                !GetOverlappedResult(RightHandle, &rightOverlapped, &rightBytesRead, FALSE), "WSAGetLastError %d", WSAGetLastError());

            rightReadPending = false;
            if (rightBytesRead == 0)
            {
                RightHandle = nullptr;
                if (WI_IsFlagSet(Flags, RelayFlags::LeftIsSocket))
                {
                    LOG_LAST_ERROR_IF(shutdown(reinterpret_cast<SOCKET>(LeftHandle), SD_SEND) == SOCKET_ERROR);
                }
            }
            else if (LeftHandle != nullptr)
            {
                auto writeSpan = rightReadSpan.first(rightBytesRead);
                bytesWritten = InterruptableWrite(LeftHandle, writeSpan, {}, &rightOverlapped);
                if (bytesWritten == 0)
                {
                    break;
                }

                rightOffset.QuadPart += rightBytesRead;
                rightOverlapped.Offset = rightOffset.LowPart;
                rightOverlapped.OffsetHigh = rightOffset.HighPart;
            }
        }
        else
        {
            THROW_HR_MSG(E_FAIL, "WaitForMultipleObjects %d", waitResult);
        }
    }
}

void wsl::windows::common::relay::SocketRelay(_In_ SOCKET LeftSocket, _In_ SOCKET RightSocket, _In_ size_t BufferSize)
{
    constexpr RelayFlags flags = RelayFlags::LeftIsSocket | RelayFlags::RightIsSocket;
    BidirectionalRelay(reinterpret_cast<HANDLE>(LeftSocket), reinterpret_cast<HANDLE>(RightSocket), BufferSize, flags);
}

void ScopedRelay::Sync()
{
    if (m_thread.joinable())
    {
        m_thread.join();
    }
}

ScopedRelay::~ScopedRelay()
{
    try
    {
        m_onDestroy();
    }
    CATCH_LOG();

    m_exitEvent.SetEvent();
    Sync();
}

void ScopedRelay::Run(_In_ HANDLE Input, _In_ HANDLE Output, size_t BufferSize) const
{
    wsl::windows::common::wslutil::SetThreadDescription(L"ScopedRelay");

    try
    {
        InterruptableRelay(Input, Output, m_exitEvent.get(), BufferSize);
    }
    CATCH_LOG();
}

ScopedMultiRelay::ScopedMultiRelay(const std::vector<HANDLE>& Inputs, const TWriteMethod& Write, size_t BufferSize)
{
    m_thread = std::thread{[this, BufferSize = BufferSize, Inputs = std::move(Inputs), Write = std::move(Write)]() {
        Run(Inputs, Write, BufferSize);
    }};
}

void ScopedMultiRelay::Sync()
{
    if (m_thread.joinable())
    {
        m_thread.join();
    }
}

ScopedMultiRelay::~ScopedMultiRelay()
{
    m_exitEvent.SetEvent();
    Sync();
}

void ScopedMultiRelay::Run(const std::vector<HANDLE>& Handles, const TWriteMethod& Write, size_t BufferSize) const
try
{
    enum State
    {
        Standby,
        Pending,
        Eof
    };

    struct Input
    {
        HANDLE Handle;
        LARGE_INTEGER Offset;
        std::vector<std::byte> Buffer;
        wil::unique_event Event{wil::EventOptions::ManualReset};
        OVERLAPPED Overlapped;
        State State = Standby;

        Input(Input&&) = default;
        Input& operator=(Input&&) = default;

        Input(HANDLE Handle, LARGE_INTEGER Offset, size_t BufferSize) : Handle(Handle), Offset(Offset), Buffer(BufferSize)
        {
            Overlapped.hEvent = Event.get();
        }

        ~Input()
        {
            // Cancel outstanding IO, if any.
            if (State == Pending)
            {
                CancelIoEx(Handle, &Overlapped);
                DWORD bytesRead{};
                GetOverlappedResult(Handle, &Overlapped, &bytesRead, TRUE);
            }
        }
    };

    std::vector<Input> Inputs;
    for (const auto& e : Handles)
    {
        Inputs.emplace_back(e, InitializeFileOffset(e), BufferSize);
    }

    while (true)
    {
        // Exit if all inputs are completed, or if the exit event is set.
        if (m_exitEvent.is_signaled() || std::all_of(Inputs.begin(), Inputs.end(), [](const auto& e) { return e.State == Eof; }))
        {
            return;
        }

        for (size_t i = 0; i < Inputs.size(); i++)
        {
            auto& e = Inputs[i];

            // If a read has been scheduled, check if IO is available.
            if (e.State == Pending)
            {
                if (e.Event.is_signaled())
                {
                    DWORD Transferred{};
                    if (!GetOverlappedResult(e.Handle, &e.Overlapped, &Transferred, TRUE))
                    {
                        auto lastError = GetLastError();
                        if ((lastError == ERROR_HANDLE_EOF) || (lastError == ERROR_BROKEN_PIPE))
                        {
                            e.State = Eof;
                            continue;
                        }

                        THROW_LAST_ERROR_IF(lastError != ERROR_IO_PENDING);
                    }

                    // IO is available.
                    Write(i, gsl::make_span(e.Buffer.data(), Transferred));

                    // Update input state.
                    e.Offset.QuadPart += Transferred;
                    e.State = Standby;
                }
            }

            // If no read is pending, start one.
            if (e.State == Standby)
            {
                e.Event.ResetEvent();

                e.Overlapped.Offset = e.Offset.LowPart;
                e.Overlapped.OffsetHigh = e.Offset.HighPart;

                DWORD BytesRead{};
                if (ReadFile(e.Handle, e.Buffer.data(), static_cast<DWORD>(e.Buffer.size()), &BytesRead, &e.Overlapped))
                {
                    // IO is available.
                    Write(i, gsl::make_span(e.Buffer.data(), BytesRead));

                    // Update input state.
                    e.Offset.QuadPart += BytesRead;
                    e.State = Standby;
                }
                else
                {
                    auto lastError = GetLastError();
                    if ((lastError == ERROR_HANDLE_EOF) || (lastError == ERROR_BROKEN_PIPE))
                    {
                        e.State = Eof;
                        continue;
                    }

                    THROW_LAST_ERROR_IF(lastError != ERROR_IO_PENDING);
                    e.State = Pending;
                }
            }
        }

        // Only wait if all non-completed inputs have a scheduled ReadFile to avoid a pipe hang.
        if (std::all_of(Inputs.begin(), Inputs.end(), [](const auto& e) { return e.State == Eof || e.State == Pending; }))
        {
            // Wait until a handle is signaled.
            std::vector<HANDLE> waits{m_exitEvent.get()};
            for (const auto& e : Inputs)
            {
                if (e.State == Pending)
                {
                    waits.emplace_back(e.Event.get());
                }
            }

            THROW_LAST_ERROR_IF(WaitForMultipleObjects(static_cast<DWORD>(waits.size()), waits.data(), false, INFINITE) == WAIT_FAILED);
        }
    }
}
CATCH_LOG()

void MultiHandleWait::AddHandle(std::unique_ptr<OverlappedIOHandle>&& handle, Flags flags)
{
    m_handles.emplace_back(flags, std::move(handle));
}

void MultiHandleWait::Cancel()
{
    m_cancel = true;
}
bool MultiHandleWait::Run(std::optional<std::chrono::milliseconds> Timeout)
{
    m_cancel = false; // Run may be called multiple times.

    std::optional<std::chrono::steady_clock::time_point> deadline;

    if (Timeout.has_value())
    {
        deadline = std::chrono::steady_clock::now() + Timeout.value();
    }

    // Run until all handles are completed.

    while (!m_handles.empty() && !m_cancel)
    {
        // Schedule IO on each handle until all are either pending, or completed.
        for (size_t i = 0; i < m_handles.size(); i++)
        {
            while (m_handles[i].second->GetState() == IOHandleStatus::Standby)
            {
                try
                {
                    m_handles[i].second->Schedule();
                }
                catch (...)
                {
                    if (WI_IsFlagSet(m_handles[i].first, Flags::IgnoreErrors))
                    {
                        m_handles[i].second.reset(); // Reset the handle so it can be deleted.
                    }
                    else
                    {
                        throw;
                    }
                }
            }
        }

        // Remove completed handles from m_handles.
        for (auto it = m_handles.begin(); it != m_handles.end();)
        {
            if (!it->second)
            {
                it = m_handles.erase(it);
            }
            else if (it->second->GetState() == IOHandleStatus::Completed)
            {
                if (WI_IsFlagSet(it->first, Flags::CancelOnCompleted))
                {
                    m_cancel = true; // Cancel the IO if a handle with CancelOnCompleted is in the completed state.
                }

                it = m_handles.erase(it);
            }
            else
            {
                ++it;
            }
        }

        if (m_handles.empty() || m_cancel)
        {
            break;
        }

        // Wait for the next operation to complete.
        std::vector<HANDLE> waitHandles;
        for (const auto& e : m_handles)
        {
            waitHandles.emplace_back(e.second->GetHandle());
        }

        DWORD waitTimeout = INFINITE;
        if (deadline.has_value())
        {
            auto miliseconds =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline.value() - std::chrono::steady_clock::now()).count();

            waitTimeout = static_cast<DWORD>(std::max(0LL, miliseconds));
        }

        auto result = WaitForMultipleObjects(static_cast<DWORD>(waitHandles.size()), waitHandles.data(), false, waitTimeout);
        if (result == WAIT_TIMEOUT)
        {
            THROW_WIN32(ERROR_TIMEOUT);
        }
        else if (result >= WAIT_OBJECT_0 && result < WAIT_OBJECT_0 + m_handles.size())
        {
            auto index = result - WAIT_OBJECT_0;

            try
            {
                m_handles[index].second->Collect();
            }
            catch (...)
            {
                if (WI_IsFlagSet(m_handles[index].first, Flags::IgnoreErrors))
                {
                    m_handles.erase(m_handles.begin() + index);
                }
                else
                {
                    throw;
                }
            }
        }
        else
        {
            THROW_LAST_ERROR_MSG("Timeout: %lu, Count: %llu", waitTimeout, waitHandles.size());
        }
    }

    return !m_cancel;
}

IOHandleStatus OverlappedIOHandle::GetState() const
{
    return State;
}

EventHandle::EventHandle(HandleWrapper&& Handle, std::function<void()>&& OnSignalled) :
    Handle(std::move(Handle)), OnSignalled(std::move(OnSignalled))
{
}

void EventHandle::Schedule()
{
    State = IOHandleStatus::Pending;
}

void EventHandle::Collect()
{
    State = IOHandleStatus::Completed;
    OnSignalled();
}

HANDLE EventHandle::GetHandle() const
{
    return Handle.Get();
}

SingleAcceptHandle::SingleAcceptHandle(HandleWrapper&& ListenSocket, HandleWrapper&& AcceptedSocket, std::function<void()>&& OnAccepted) :
    ListenSocket(std::move(ListenSocket)), AcceptedSocket(std::move(AcceptedSocket)), OnAccepted(std::move(OnAccepted))
{
    Overlapped.hEvent = Event.get();
}

SingleAcceptHandle::~SingleAcceptHandle()
{
    if (State == IOHandleStatus::Pending)
    {
        LOG_IF_WIN32_BOOL_FALSE(CancelIoEx(ListenSocket.Get(), &Overlapped));

        DWORD bytesProcessed{};
        DWORD flagsReturned{};
        if (!WSAGetOverlappedResult((SOCKET)ListenSocket.Get(), &Overlapped, &bytesProcessed, TRUE, &flagsReturned))
        {
            auto error = GetLastError();
            LOG_LAST_ERROR_IF(error != ERROR_CONNECTION_ABORTED && error != ERROR_OPERATION_ABORTED);
        }
    }
}

void SingleAcceptHandle::Schedule()
{
    WI_ASSERT(State == IOHandleStatus::Standby);

    // Schedule the accept.
    DWORD bytesReturned{};
    if (AcceptEx((SOCKET)ListenSocket.Get(), (SOCKET)AcceptedSocket.Get(), &AcceptBuffer, 0, sizeof(SOCKADDR_STORAGE), sizeof(SOCKADDR_STORAGE), &bytesReturned, &Overlapped))
    {
        // Accept completed immediately.
        State = IOHandleStatus::Completed;
        OnAccepted();
    }
    else
    {
        auto error = WSAGetLastError();
        THROW_HR_IF_MSG(HRESULT_FROM_WIN32(error), error != ERROR_IO_PENDING, "Handle: 0x%p", (void*)ListenSocket.Get());

        State = IOHandleStatus::Pending;
    }
}

void SingleAcceptHandle::Collect()
{
    WI_ASSERT(State == IOHandleStatus::Pending);

    DWORD bytesReceived{};
    DWORD flagsReturned{};

    THROW_IF_WIN32_BOOL_FALSE(WSAGetOverlappedResult((SOCKET)ListenSocket.Get(), &Overlapped, &bytesReceived, false, &flagsReturned));

    State = IOHandleStatus::Completed;
    OnAccepted();
}

HANDLE SingleAcceptHandle::GetHandle() const
{
    return Event.get();
}