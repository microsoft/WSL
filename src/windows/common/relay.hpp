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

class OverlappedIOHandle
{
public:
    NON_COPYABLE(OverlappedIOHandle)
    NON_MOVABLE(OverlappedIOHandle)

    OverlappedIOHandle() = default;
    virtual ~OverlappedIOHandle() = default;
    virtual void Schedule() = 0;
    virtual void Collect() = 0;
    virtual HANDLE GetHandle() const = 0;
    IOHandleStatus GetState() const;

protected:
    IOHandleStatus State = IOHandleStatus::Standby;
};

class EventHandle : public OverlappedIOHandle
{
public:
    NON_COPYABLE(EventHandle)
    NON_MOVABLE(EventHandle)

    EventHandle(wil::unique_event&& EventHandle, std::function<void()>&& OnSignalled);
    void Schedule() override;
    void Collect() override;
    HANDLE GetHandle() const override;

private:
    wil::unique_event Handle;
    std::function<void()> OnSignalled;
};

class ReadHandle : public OverlappedIOHandle
{
public:
    NON_COPYABLE(ReadHandle);
    NON_MOVABLE(ReadHandle);

    ReadHandle(wil::unique_handle&& MovedHandle, std::function<void(const gsl::span<char>& Buffer)>&& OnRead);
    ~ReadHandle();
    void Schedule() override;
    void Collect() override;
    HANDLE GetHandle() const override;

private:
    wil::unique_handle Handle;
    std::function<void(const gsl::span<char>& Buffer)> OnRead;
    wil::unique_event Event{wil::EventOptions::ManualReset};
    OVERLAPPED Overlapped{};
    std::vector<char> Buffer = std::vector<char>(LX_RELAY_BUFFER_SIZE);
};

class WriteHandle : public OverlappedIOHandle
{
public:
    NON_COPYABLE(WriteHandle);
    NON_MOVABLE(WriteHandle);

    WriteHandle(wil::unique_handle&& MovedHandle, const std::vector<char>& Buffer);
    ~WriteHandle();
    void Schedule() override;
    void Collect() override;
    HANDLE GetHandle() const override;

private:
    wil::unique_handle Handle;
    wil::unique_event Event{wil::EventOptions::ManualReset};
    OVERLAPPED Overlapped{};
    const std::vector<char>& Buffer;
    DWORD Offset = 0;
};

class MultiHandleWait
{
public:
    MultiHandleWait() = default;

    void AddHandle(std::unique_ptr<OverlappedIOHandle>&& handle);
    void Run(std::optional<std::chrono::milliseconds> Timeout);

private:
    std::vector<std::unique_ptr<OverlappedIOHandle>> m_handles;
};

} // namespace wsl::windows::common::relay
