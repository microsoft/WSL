/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    relay.hpp

Abstract:

    This file contains function declarations for the relay worker thread routines.

--*/

#pragma once

#include <winsock2.h>
#include "svccommio.hpp"

#define LX_RELAY_BUFFER_SIZE 0x1000

namespace wsl::windows::common::relay {

std::thread CreateThread(_In_ HANDLE InputHandle, _In_ HANDLE OutputHandle, _In_opt_ HANDLE ExitHandle = nullptr, _In_ size_t BufferSize = LX_RELAY_BUFFER_SIZE);

std::thread CreateThread(_In_ wil::unique_handle&& InputHandle, _In_ HANDLE OutputHandle, _In_opt_ HANDLE ExitHandle = nullptr, _In_ size_t BufferSize = LX_RELAY_BUFFER_SIZE);

std::thread CreateThread(_In_ HANDLE InputHandle, _In_ wil::unique_handle&& OutputHandle, _In_opt_ HANDLE ExitHandle = nullptr, _In_ size_t BufferSize = LX_RELAY_BUFFER_SIZE);

std::thread CreateThread(
    _In_ wil::unique_handle&& InputHandle,
    _In_ wil::unique_handle&& OutputHandle,
    _In_opt_ HANDLE ExitHandle = nullptr,
    _In_ size_t BufferSize = LX_RELAY_BUFFER_SIZE);

DWORD
InterruptableRead(_In_ HANDLE InputHandle, _In_ gsl::span<gsl::byte> Buffer, _In_ const std::vector<HANDLE>& ExitHandles, _In_opt_ LPOVERLAPPED Overlapped = nullptr);

void InterruptableRelay(_In_ HANDLE InputHandle, _In_opt_ HANDLE OutputHandle, _In_opt_ HANDLE ExitHandle = nullptr, _In_ size_t BufferSize = LX_RELAY_BUFFER_SIZE);

bool InterruptableWait(_In_ HANDLE WaitObject, _In_ const std::vector<HANDLE>& ExitHandles = {});

DWORD
InterruptableWrite(_In_ HANDLE OutputHandle, _In_ gsl::span<const gsl::byte> Buffer, _In_ const std::vector<HANDLE>& ExitHandles, _In_ LPOVERLAPPED Overlapped);

void StandardInputRelay(HANDLE ConsoleHandle, HANDLE OutputHandle, const std::function<void()>& UpdateTerminalSize, HANDLE ExitEvent);

enum class RelayFlags
{
    None = 0,
    LeftIsSocket = 1,
    RightIsSocket = 2
};

DEFINE_ENUM_FLAG_OPERATORS(RelayFlags);

void BidirectionalRelay(_In_ HANDLE LeftHandle, _In_ HANDLE RightHandle, _In_ size_t BufferSize = LX_RELAY_BUFFER_SIZE, _In_ RelayFlags Flags = RelayFlags::None);

void SocketRelay(_In_ SOCKET LeftSocket, _In_ SOCKET RightSocket, _In_ size_t BufferSize = LX_RELAY_BUFFER_SIZE);

class ScopedMultiRelay
{
public:
    using TWriteMethod = std::function<void(size_t, const gsl::span<gsl::byte>& buffer)>;
    ScopedMultiRelay(const std::vector<HANDLE>& Inputs, const TWriteMethod& Write, size_t BufferSize = LX_RELAY_BUFFER_SIZE);

    ~ScopedMultiRelay();

    ScopedMultiRelay(ScopedMultiRelay&& other) = default;
    ScopedMultiRelay(const ScopedMultiRelay&) = delete;

    ScopedMultiRelay& operator=(const ScopedMultiRelay&) = delete;
    ScopedMultiRelay& operator=(ScopedMultiRelay&&) = delete;

    // Blocks until the relaying is complete.
    // This is useful for situations where the relay should make sure that all
    // the content has been flushed before exiting.
    void Sync();

private:
    void Run(const std::vector<HANDLE>& Inputs, const TWriteMethod& Write, size_t BufferSize = LX_RELAY_BUFFER_SIZE) const;

    std::thread m_thread;
    wil::unique_event m_exitEvent{wil::EventOptions::ManualReset};
};

// Helper class to relay the output of a handle to another.
// Note: The relay can take ownership of the handles if desired.
// Doing that will cause the handle to be released when the relaying is complete.

class ScopedRelay
{
public:
    template <typename TInput, typename TOutput>
    ScopedRelay(
        TInput&& Input, TOutput&& Output, size_t BufferSize = LX_RELAY_BUFFER_SIZE, std::function<void()>&& OnDestroy = []() {}) :
        m_onDestroy(std::move(OnDestroy))
    {
        m_thread = std::thread{[this, Input = std::move(Input), Output = std::move(Output), BufferSize = BufferSize]() {
            try
            {
                Run(GetUnderlyingHandle(Input), GetUnderlyingHandle(Output), BufferSize);
            }
            CATCH_LOG();
        }};
    }

    ~ScopedRelay();

    ScopedRelay(ScopedRelay&& other) = default;
    ScopedRelay(const ScopedRelay&) = delete;

    ScopedRelay& operator=(const ScopedRelay&) = delete;
    ScopedRelay& operator=(ScopedRelay&&) = delete;

    // Blocks until the relaying is complete.
    // This is useful for situations where the relay should make sure that all
    // the content has been flushed before exiting.
    void Sync();

private:
    template <typename THandle>
    static HANDLE GetUnderlyingHandle(THandle& handle)
    {
        if constexpr (std::is_same_v<std::remove_cv_t<THandle>, HANDLE>)
        {
            return handle;
        }
        else if constexpr (std::is_same_v<std::remove_cv_t<THandle>, wil::unique_handle>)
        {
            return handle.get();
        }
        else if constexpr (std::is_same_v<std::remove_cv_t<THandle>, wil::unique_socket>)
        {
            return reinterpret_cast<HANDLE>(handle.get());
        }
        else if constexpr (std::is_same_v<std::remove_cv_t<THandle>, SOCKET>)
        {
            return reinterpret_cast<HANDLE>(handle);
        }
        else
        {
            // If this assert fails, an invalid type was passed to ScopedRelay
            static_assert(sizeof(THandle) != sizeof(THandle));
        }
    }

    void Run(_In_ HANDLE Input, _In_ HANDLE Output, size_t BufferSize) const;

    std::thread m_thread;
    wil::unique_event m_exitEvent{wil::EventOptions::ManualReset};
    std::function<void()> m_onDestroy;
};

enum class IOHandleStatus
{
    Standby,
    Pending,
    Completed
};

class IOHandle
{
public:
    virtual ~IOHandle()
    {
    }
    virtual void Schedule() = 0;
    virtual void Collect() = 0;
    virtual HANDLE GetHandle() = 0;

    IOHandleStatus GetState() const
    {
        return State;
    }

protected:
    IOHandleStatus State = IOHandleStatus::Standby;
};

struct EventHandle : IOHandle // TODO: move to .cpp
{
    wil::unique_handle Handle;
    std::function<void()> OnSignalled;

    NON_COPYABLE(EventHandle);
    NON_MOVABLE(EventHandle);

    EventHandle(wil::unique_handle&& Handle, std::function<void()>&& OnSignalled) :
        Handle(std::move(Handle)), OnSignalled(std::move(OnSignalled))
    {
    }

    void Schedule() override
    {
        State = IOHandleStatus::Pending;
    }

    void Collect() override
    {
        State = IOHandleStatus::Completed;
        OnSignalled();
    }

    HANDLE GetHandle() override
    {
        return Handle.get();
    }
};

struct ReadHandle : IOHandle // TODO: move to .cpp
{
    wil::unique_handle Handle;
    std::function<void(const gsl::span<char>& Buffer)> OnRead;
    wil::unique_event Event{wil::EventOptions::ManualReset};
    OVERLAPPED Overlapped{};
    std::vector<char> Buffer;

    NON_COPYABLE(ReadHandle);
    NON_MOVABLE(ReadHandle);

    ReadHandle(wil::unique_handle&& Handle, std::function<void(const gsl::span<char>& Buffer)>&& OnRead) :
        Handle(std::move(Handle)), OnRead(OnRead), Buffer(LX_RELAY_BUFFER_SIZE)
    {
        Overlapped.hEvent = Event.get();
    }

    ~ReadHandle()
    {
        if (State == IOHandleStatus::Pending)
        {
            DWORD bytesRead{};
            LOG_IF_WIN32_BOOL_FALSE(CancelIoEx(reinterpret_cast<HANDLE>(Handle.get()), &Overlapped));
            LOG_IF_WIN32_BOOL_FALSE(GetOverlappedResult(Handle.get(), &Overlapped, &bytesRead, true));
        }
    }

    void Schedule() override
    {
        WI_ASSERT(State == IOHandleStatus::Standby);

        Event.ResetEvent();

        // Schedule the read.
        DWORD bytesRead{};
        if (ReadFile(Handle.get(), Buffer.data(), static_cast<DWORD>(Buffer.size()), &bytesRead, &Overlapped))
        {
            // Signal the read.
            OnRead(gsl::make_span<char>(Buffer.data(), static_cast<size_t>(bytesRead)));

            // ReadFile completed immediately, process the result right away.
            if (bytesRead == 0)
            {
                State = IOHandleStatus::Completed;
                return; // Handle is completely read, don't try again.
            }

            // Read was done synchronously, remain in 'standby' state.
        }
        else
        {
            auto error = GetLastError();
            if (error == ERROR_HANDLE_EOF || error == ERROR_BROKEN_PIPE)
            {
                State = IOHandleStatus::Completed;
                return;
            }

            THROW_LAST_ERROR_IF_MSG(error != ERROR_IO_PENDING, "Handle: 0x%p", (void*)Handle.get());

            // The read is pending, update to 'Pending'
            State = IOHandleStatus::Pending;
        }
    }

    void Collect() override
    {
        WI_ASSERT(State == IOHandleStatus::Pending);

        // Transition back to standby
        State = IOHandleStatus::Standby;

        // Complete the read.
        DWORD bytesRead{};
        THROW_IF_WIN32_BOOL_FALSE(GetOverlappedResult(Handle.get(), &Overlapped, &bytesRead, false));

        // Signal the read.
        OnRead(gsl::make_span<char>(Buffer.data(), static_cast<size_t>(bytesRead)));

        // Transition to Complete if this was a zero byte read.
        if (bytesRead == 0)
        {
            State = IOHandleStatus::Completed;
        }
    }

    HANDLE GetHandle() override
    {
        return Overlapped.hEvent;
    }
};

struct WriteHandle : IOHandle // TODO: move to .cpp
{
    wil::unique_handle Handle;
    wil::unique_event Event{wil::EventOptions::ManualReset};
    OVERLAPPED Overlapped{};
    const std::vector<char>& Buffer;
    DWORD Offset = 0;

    NON_COPYABLE(WriteHandle);
    NON_MOVABLE(WriteHandle);

    WriteHandle(wil::unique_handle&& Handle, const std::vector<char>& Buffer) : Handle(std::move(Handle)), Buffer(Buffer)
    {
        Overlapped.hEvent = Event.get();
    }

    ~WriteHandle()
    {
        if (State == IOHandleStatus::Pending)
        {
            DWORD bytesRead{};
            LOG_IF_WIN32_BOOL_FALSE(CancelIoEx(reinterpret_cast<HANDLE>(Handle.get()), &Overlapped));
            LOG_IF_WIN32_BOOL_FALSE(GetOverlappedResult(Handle.get(), &Overlapped, &bytesRead, true));
        }
    }

    void Schedule() override
    {
        WI_ASSERT(State == IOHandleStatus::Standby);

        Event.ResetEvent();

        // Schedule the write.
        DWORD bytesWritten{};
        if (WriteFile(Handle.get(), Buffer.data() + Offset, static_cast<DWORD>(Buffer.size() - Offset), &bytesWritten, &Overlapped))
        {
            Offset += bytesWritten;
            if (Offset >= Buffer.size())
            {
                State = IOHandleStatus::Completed;
            }
        }
        else
        {
            auto error = GetLastError();
            THROW_LAST_ERROR_IF_MSG(error != ERROR_IO_PENDING, "Handle: 0x%p", (void*)Handle.get());

            // The write is pending, update to 'Pending'
            State = IOHandleStatus::Pending;
        }
    }

    void Collect() override
    {
        WI_ASSERT(State == IOHandleStatus::Pending);

        // Transition back to standby
        State = IOHandleStatus::Standby;

        // Complete the write.
        DWORD bytesWritten{};
        THROW_IF_WIN32_BOOL_FALSE(GetOverlappedResult(Handle.get(), &Overlapped, &bytesWritten, false));

        Offset += bytesWritten;
        if (Offset >= Buffer.size())
        {
            State = IOHandleStatus::Completed;
        }
    }

    HANDLE GetHandle() override
    {
        return Overlapped.hEvent;
    }
};

class MultiHandleWait
{
public:
    MultiHandleWait() = default;

    void AddHandle(std::unique_ptr<IOHandle>&& handle);

    void Run(std::optional<std::chrono::milliseconds> Timeout);

private:
    std::vector<std::unique_ptr<IOHandle>> m_handles;
};

} // namespace wsl::windows::common::relay
