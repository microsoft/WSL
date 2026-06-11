// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "HandleIO.h"
#pragma hdrstop

using wsl::windows::common::io::BufferWrapper;
using wsl::windows::common::io::DockerIORelayHandle;
using wsl::windows::common::io::EventHandle;
using wsl::windows::common::io::HandleWrapper;
using wsl::windows::common::io::HTTPChunkBasedReadHandle;
using wsl::windows::common::io::IOHandleStatus;
using wsl::windows::common::io::LineBasedReadHandle;
using wsl::windows::common::io::MultiHandleWait;
using wsl::windows::common::io::OverlappedIOHandle;
using wsl::windows::common::io::ReadHandle;
using wsl::windows::common::io::ReadNamedPipe;
using wsl::windows::common::io::ReadSocketMessageHandle;
using wsl::windows::common::io::SingleAcceptHandle;
using wsl::windows::common::io::WriteHandle;
using wsl::windows::common::io::WriteNamedPipe;

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

DWORD CancelPendingIo(auto Handle, OVERLAPPED& Overlapped)
{
    DWORD bytesTransferred{};
    if (CancelIoEx((HANDLE)Handle, &Overlapped) || GetLastError() == ERROR_NOT_FOUND)
    {
        if constexpr (std::is_same_v<decltype(Handle), SOCKET>)
        {
            DWORD flagsReturned{};
            if (!WSAGetOverlappedResult(Handle, &Overlapped, &bytesTransferred, true, &flagsReturned))
            {
                auto error = WSAGetLastError();
                LOG_LAST_ERROR_IF(error != WSAECONNABORTED && error != WSA_OPERATION_ABORTED && error != WSAECONNRESET);
            }
        }
        else
        {
            static_assert(std::is_same_v<decltype(Handle), HANDLE>);
            if (!GetOverlappedResult(Handle, &Overlapped, &bytesTransferred, true))
            {
                auto error = GetLastError();
                LOG_LAST_ERROR_IF(error != ERROR_CONNECTION_ABORTED && error != ERROR_OPERATION_ABORTED);
            }
        }
    }
    else
    {
        LOG_LAST_ERROR_MSG("Unexpected error while cancelling IO on handle: 0x%p", (void*)Handle);
    }

    return bytesTransferred;
}

inline void UnregisterWait(HANDLE waitHandle) noexcept
{
    // INVALID_HANDLE_VALUE makes UnregisterWaitEx block until any in-flight wait callback returns.
    LOG_LAST_ERROR_IF(!UnregisterWaitEx(waitHandle, INVALID_HANDLE_VALUE));
}

using unique_registered_wait = wil::unique_any_handle_null<decltype(&UnregisterWait), &UnregisterWait>;

} // namespace

// HandleWrapper

HandleWrapper::HandleWrapper(wil::unique_handle&& handle, std::function<void()>&& OnClose) :
    Handle(handle.get()), OwnedHandle(std::move(handle)), OnClose(std::move(OnClose))
{
}

HandleWrapper::HandleWrapper(wil::unique_socket&& handle, std::function<void()>&& OnClose) :
    Handle((HANDLE)handle.get()), OwnedHandle(wil::unique_socket{handle.release()}), OnClose(std::move(OnClose))
{
}

HandleWrapper::HandleWrapper(wil::unique_event&& handle, std::function<void()>&& OnClose) :
    Handle(handle.get()), OwnedHandle(wil::unique_handle{handle.release()}), OnClose(std::move(OnClose))
{
}

HandleWrapper::HandleWrapper(SOCKET handle, std::function<void()>&& OnClose) :
    Handle(reinterpret_cast<HANDLE>(handle)), OnClose(std::move(OnClose))
{
}

HandleWrapper::HandleWrapper(HANDLE handle, std::function<void()>&& OnClose) : Handle(handle), OnClose(std::move(OnClose))
{
}

HandleWrapper::HandleWrapper(wil::unique_hfile&& handle, std::function<void()>&& OnClose) :
    Handle(handle.get()), OwnedHandle(wil::unique_handle{handle.release()}), OnClose(std::move(OnClose))
{
}

HandleWrapper::~HandleWrapper()
{
    Reset();
}

HANDLE HandleWrapper::Get() const
{
    return Handle;
}

void HandleWrapper::Reset()
{
    if (OnClose != nullptr)
    {
        OnClose();
        OnClose = nullptr;
    }

    OwnedHandle = {};
    Handle = nullptr;
}

// BufferWrapper

BufferWrapper::BufferWrapper(size_t size) : m_owned(std::in_place, size)
{
}

BufferWrapper::BufferWrapper(gsl::span<gsl::byte> span) : m_unowned(span)
{
}

bool BufferWrapper::Owned() const noexcept
{
    return m_owned.has_value();
}

void BufferWrapper::Resize(size_t size)
{
    THROW_HR_IF_MSG(E_UNEXPECTED, !Owned(), "BufferWrapper::Resize called on a non-owned buffer");
    m_owned->resize(size);
}

void BufferWrapper::Append(gsl::span<char> Span)
{
    THROW_HR_IF_MSG(E_UNEXPECTED, !Owned(), "BufferWrapper::Append called on a non-owned buffer");

    m_owned->insert(m_owned->end(), Span.begin(), Span.end());
}

void BufferWrapper::Consume(size_t bytes) noexcept
{
    WI_ASSERT(bytes <= Size());
    if (Owned())
    {
        m_owned->erase(m_owned->begin(), m_owned->begin() + bytes);
    }
    else
    {
        m_unowned = m_unowned.subspan(bytes);
    }
}

gsl::span<gsl::byte> BufferWrapper::Span() noexcept
{
    return Owned() ? gsl::make_span(reinterpret_cast<gsl::byte*>(m_owned->data()), m_owned->size()) : m_unowned;
}

size_t BufferWrapper::Size() const noexcept
{
    return Owned() ? m_owned->size() : m_unowned.size();
}

// OverlappedIOHandle

IOHandleStatus OverlappedIOHandle::GetState() const
{
    return State;
}

// EventHandle

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

// ReadHandle

ReadHandle::ReadHandle(HandleWrapper&& MovedHandle, std::function<void(const gsl::span<char>& Buffer)>&& OnRead) :
    Handle(std::move(MovedHandle)), OnRead(OnRead), Offset(InitializeFileOffset(Handle.Get()))
{
    Overlapped.hEvent = Event.get();
}

ReadHandle::~ReadHandle()
{
    if (State == IOHandleStatus::Pending)
    {
        CancelPendingIo(Handle.Get(), Overlapped);
    }
}

void ReadHandle::Schedule()
{
    WI_ASSERT(State == IOHandleStatus::Standby);

    Event.ResetEvent();

    // Schedule the read.
    DWORD bytesRead{};
    Overlapped.Offset = Offset.LowPart;
    Overlapped.OffsetHigh = Offset.HighPart;
    auto* bufferData = reinterpret_cast<char*>(Buffer.Span().data());
    if (ReadFile(Handle.Get(), bufferData, static_cast<DWORD>(Buffer.Size()), &bytesRead, &Overlapped))
    {
        Offset.QuadPart += bytesRead;

        // Signal the read.
        OnRead(gsl::make_span<char>(bufferData, static_cast<size_t>(bytesRead)));

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
            // Signal an empty read for EOF.
            OnRead({});

            State = IOHandleStatus::Completed;
            return;
        }

        THROW_LAST_ERROR_IF_MSG(error != ERROR_IO_PENDING, "Handle: 0x%p", (void*)Handle.Get());

        // The read is pending, update to 'Pending'
        State = IOHandleStatus::Pending;
    }
}

void ReadHandle::Collect()
{
    WI_ASSERT(State == IOHandleStatus::Pending);

    // Transition back to standby
    State = IOHandleStatus::Standby;

    // Complete the read.
    DWORD bytesRead{};
    if (!GetOverlappedResult(Handle.Get(), &Overlapped, &bytesRead, false))
    {
        auto error = GetLastError();
        THROW_WIN32_IF(error, error != ERROR_HANDLE_EOF && error != ERROR_BROKEN_PIPE);

        // We received ERROR_HANDLE_EOF or ERROR_BROKEN_PIPE. Validate that this was indeed a zero byte read.
        WI_ASSERT(bytesRead == 0);
    }

    Offset.QuadPart += bytesRead;

    // Signal the read.
    OnRead(gsl::make_span<char>(reinterpret_cast<char*>(Buffer.Span().data()), static_cast<size_t>(bytesRead)));

    // Transition to Complete if this was a zero byte read.
    if (bytesRead == 0)
    {
        State = IOHandleStatus::Completed;
    }
}

HANDLE ReadHandle::GetHandle() const
{
    return Event.get();
}

// ReadNamedPipe

ReadNamedPipe::ReadNamedPipe(HandleWrapper&& Pipe, std::function<void(const gsl::span<char>& Buffer)>&& OnRead) :
    ReadHandle(std::move(Pipe), std::move(OnRead))
{
}

void ReadNamedPipe::Schedule()
{
    if (!m_connected)
    {
        WI_ASSERT(State == IOHandleStatus::Standby);

        if (!ConnectNamedPipe(Handle.Get(), &Overlapped))
        {
            const auto error = GetLastError();
            if (error == ERROR_IO_PENDING)
            {
                State = IOHandleStatus::Pending;
                return;
            }

            THROW_HR_IF_MSG(HRESULT_FROM_WIN32(error), error != ERROR_PIPE_CONNECTED, "Handle: 0x%p", (void*)Handle.Get());
        }

        m_connected = true;
    }

    ReadHandle::Schedule();
}

void ReadNamedPipe::Collect()
{
    if (!m_connected)
    {
        WI_ASSERT(State == IOHandleStatus::Pending);

        DWORD bytes{};
        if (!GetOverlappedResult(Handle.Get(), &Overlapped, &bytes, FALSE))
        {
            const auto error = GetLastError();
            THROW_HR_IF_MSG(HRESULT_FROM_WIN32(error), error != ERROR_PIPE_CONNECTED, "Handle: 0x%p", (void*)Handle.Get());
        }

        m_connected = true;

        // Transition back to standby so the IO loop schedules the first read.
        State = IOHandleStatus::Standby;
        return;
    }

    ReadHandle::Collect();
}

// SingleAcceptHandle

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

// LineBasedReadHandle

LineBasedReadHandle::LineBasedReadHandle(HandleWrapper&& Handle, std::function<void(const gsl::span<char>& Line)>&& OnLine, bool Crlf) :
    ReadHandle(std::move(Handle), [this](const gsl::span<char>& Buffer) { OnRead(Buffer); }), OnLine(OnLine), Crlf(Crlf)
{
}

LineBasedReadHandle::~LineBasedReadHandle()
{
    // N.B. PendingBuffer can contain remaining data is an exception was thrown during parsing.
}

void LineBasedReadHandle::OnRead(const gsl::span<char>& Buffer)
{
    // If we reach of the end, signal a line with the remaining buffer.
    if (Buffer.empty() && !PendingBuffer.empty())
    {
        OnLine(PendingBuffer);
        PendingBuffer.clear();
        return;
    }

    auto begin = Buffer.begin();
    auto end = std::ranges::find(Buffer, Crlf ? '\r' : '\n');
    while (end != Buffer.end())
    {
        if (Crlf)
        {
            end++; // Move to the following '\n'

            if (end == Buffer.end() || *end != '\n') // Incomplete CRLF sequence. Append to buffer and continue.
            {
                PendingBuffer.insert(PendingBuffer.end(), begin, end);
                begin = end;
                end = std::ranges::find(end, Buffer.end(), '\r');
                continue;
            }
        }

        // Discard the '\r' in CRLF mode.
        PendingBuffer.insert(PendingBuffer.end(), begin, Crlf ? end - 1 : end);

        if (!PendingBuffer.empty())
        {
            OnLine(PendingBuffer);
            PendingBuffer.clear();
        }

        begin = end + 1;
        end = std::ranges::find(begin, Buffer.end(), Crlf ? '\r' : '\n');
    }

    PendingBuffer.insert(PendingBuffer.end(), begin, end);
}

// HTTPChunkBasedReadHandle

HTTPChunkBasedReadHandle::HTTPChunkBasedReadHandle(HandleWrapper&& MovedHandle, std::function<void(const gsl::span<char>& Line)>&& OnChunk) :
    ReadHandle(std::move(MovedHandle), [this](const gsl::span<char>& Buffer) { OnRead(Buffer); }), OnChunk(std::move(OnChunk))
{
}

HTTPChunkBasedReadHandle::~HTTPChunkBasedReadHandle()
{
    // N.B. PendingBuffer can contain remaining data is an exception was thrown during parsing.
    LOG_HR_IF(E_UNEXPECTED, !PendingBuffer.empty() || PendingChunkSize != 0 || ExpectHeader);
}

void HTTPChunkBasedReadHandle::OnRead(const gsl::span<char>& Input)
{
    // See: https://httpwg.org/specs/rfc9112.html#field.transfer-encoding

    if (Input.empty())
    {
        // N.B. The body can be terminated by a zero-length chunk.
        THROW_HR_IF(E_INVALIDARG, PendingChunkSize != 0 || ExpectHeader);
    }

    auto buffer = Input;

    auto advance = [&](size_t count) {
        WI_ASSERT(buffer.size() >= count);
        buffer = buffer.subspan(count);
    };

    while (!buffer.empty())
    {
        if (PendingChunkSize == 0)
        {
            // Consume CRLF's between chunks.
            if (PendingBuffer.empty() && (buffer.front() == '\r' || buffer.front() == '\n'))
            {
                advance(1);
                continue;
            }

            ExpectHeader = true;

            auto end = std::ranges::find(buffer, '\n');
            PendingBuffer.insert(PendingBuffer.end(), buffer.begin(), end);
            if (end == buffer.end())
            {
                // Incomplete size header, buffer until next read.
                break;
            }
            // Advance beyond the LF
            advance(end - buffer.begin() + 1);

            THROW_HR_IF_MSG(
                E_INVALIDARG,
                PendingBuffer.size() < 2 || PendingBuffer.back() != '\r',
                "Malformed chunk header: %hs",
                PendingBuffer.c_str());
            PendingBuffer.erase(PendingBuffer.end() - 1, PendingBuffer.end()); // Remove CR.

#ifdef WSLC_HTTP_DEBUG

            WSL_LOG("HTTPChunkHeader", TraceLoggingValue(PendingBuffer.c_str(), "Size"));

#endif

            try
            {
                size_t parsed{};
                PendingChunkSize = std::stoul(PendingBuffer.c_str(), &parsed, 16);
                THROW_HR_IF(E_INVALIDARG, parsed != PendingBuffer.size());
            }
            catch (...)
            {
                THROW_HR_MSG(E_INVALIDARG, "Failed to parse chunk size: %hs", PendingBuffer.c_str());
            }

            ExpectHeader = false;
            PendingBuffer.clear();
        }
        else
        {
            // Consume the chunk.
            auto consumedBytes = std::min(PendingChunkSize, buffer.size());
            PendingBuffer.append(buffer.data(), consumedBytes);
            advance(consumedBytes);

            WI_ASSERT(PendingChunkSize >= consumedBytes);
            PendingChunkSize -= consumedBytes;

            if (PendingChunkSize == 0)
            {

#ifdef WSLC_HTTP_DEBUG

                WSL_LOG("HTTPChunk", TraceLoggingValue(PendingBuffer.c_str(), "Content"));

#endif
                OnChunk(PendingBuffer);
                PendingBuffer.clear();
            }
        }
    }
}

// ReadSocketMessageHandle

ReadSocketMessageHandle::ReadSocketMessageHandle(
    HandleWrapper&& MovedSocket,
    std::vector<gsl::byte>& Buffer,
    std::vector<gsl::byte>& PendingBytes,
    std::function<void(const gsl::span<gsl::byte>& Message)>&& OnMessage) :
    Socket(std::move(MovedSocket)), Buffer(Buffer), PendingBytes(PendingBytes), OnMessage(std::move(OnMessage))
{
    Overlapped.hEvent = Event.get();

    if (Buffer.size() < sizeof(MESSAGE_HEADER))
    {
        Buffer.resize(sizeof(MESSAGE_HEADER));
    }

    if (PendingBytes.empty())
    {
        return;
    }

    // If bytes from a previously cancelled transaction are passed, process them now.
    if (Buffer.size() < PendingBytes.size())
    {
        Buffer.resize(PendingBytes.size());
    }

    std::copy(PendingBytes.begin(), PendingBytes.end(), Buffer.begin());
    CurrentOffset = PendingBytes.size();
    PendingBytes.clear();

    if (CurrentOffset < sizeof(MESSAGE_HEADER))
    {
        BytesRemaining = sizeof(MESSAGE_HEADER) - CurrentOffset;
    }
    else
    {
        BytesRemaining = 0;
    }
}

ReadSocketMessageHandle::~ReadSocketMessageHandle()
{
    if (State != IOHandleStatus::Completed)
    {
        auto pendingSize = CurrentOffset;

        if (State == IOHandleStatus::Pending)
        {
            // Cancel the pending receive and move any bytes already buffered for the in-flight message into PendingBytes
            const auto socket = reinterpret_cast<SOCKET>(Socket.Get());
            pendingSize += CancelPendingIo(socket, Overlapped);
        }

        if (pendingSize > 0)
        {
            WI_ASSERT(pendingSize <= Buffer.size());
            PendingBytes.assign(Buffer.begin(), Buffer.begin() + pendingSize);

            WSL_LOG(
                "CanceledMessageRead", TraceLoggingValue(pendingSize, "TotalBytes"), TraceLoggingValue(Socket.Get(), "Socket"));
        }
    }
}

void ReadSocketMessageHandle::ScheduleRecv()
{
    Event.ResetEvent();

    auto target = gsl::make_span(Buffer).subspan(CurrentOffset, BytesRemaining);
    WSABUF wsaBuf = {gsl::narrow_cast<ULONG>(target.size()), reinterpret_cast<CHAR*>(target.data())};
    DWORD bytesRead{};
    DWORD flags = 0;
    if (WSARecv(reinterpret_cast<SOCKET>(Socket.Get()), &wsaBuf, 1, &bytesRead, &flags, &Overlapped, nullptr) == 0)
    {
        ProcessRecvResult(bytesRead);
    }
    else
    {
        auto error = WSAGetLastError();
        if (error == WSAECONNABORTED || error == WSAECONNRESET)
        {
            ProcessRecvResult(0);
            return;
        }

        THROW_HR_IF_MSG(HRESULT_FROM_WIN32(error), error != WSA_IO_PENDING, "Socket: 0x%p", (void*)Socket.Get());

        State = IOHandleStatus::Pending;
    }
}

void ReadSocketMessageHandle::ProcessRecvResult(DWORD BytesRead)
{
    if (BytesRead == 0)
    {
        // If the socket was closed before any bytes of the next message were read, signal a clean end-of-stream.
        // If some bytes were already buffered, the peer closed mid-message which is a protocol error.
        THROW_HR_IF_MSG(
            E_UNEXPECTED,
            CurrentOffset > 0,
            "Socket closed before a complete message could be read. ReadingHeader: %d, CurrentOffset: %zu, BytesRemaining: %zu",
            ReadingHeader,
            CurrentOffset,
            BytesRemaining);

        OnMessage({});
        State = IOHandleStatus::Completed;
        return;
    }

    CurrentOffset += BytesRead;
    BytesRemaining -= BytesRead;

    if (BytesRemaining > 0)
    {
        return;
    }

    ProcessChunk();
}

bool ReadSocketMessageHandle::ProcessChunk()
{
    const auto messageSize = gslhelpers::get_struct<MESSAGE_HEADER>(gsl::make_span(Buffer.data(), sizeof(MESSAGE_HEADER)))->MessageSize;

    if (ReadingHeader)
    {
        THROW_HR_IF_MSG(E_UNEXPECTED, messageSize < sizeof(MESSAGE_HEADER), "Unexpected message size: %u", messageSize);
        THROW_HR_IF_MSG(E_UNEXPECTED, messageSize > 16 * 1024 * 1024, "Message size too large: %u", messageSize);

        if (Buffer.size() < messageSize)
        {
            Buffer.resize(messageSize);
        }

        ReadingHeader = false;
        if (CurrentOffset < messageSize)
        {
            BytesRemaining = messageSize - CurrentOffset;
        }

        if (BytesRemaining > 0)
        {
            return true;
        }
    }

    OnMessage(gsl::make_span(Buffer.data(), messageSize));
    State = IOHandleStatus::Completed;
    return false;
}

void ReadSocketMessageHandle::Schedule()
{
    WI_ASSERT(State == IOHandleStatus::Standby);

    // Process previously received bytes, if any.
    if (BytesRemaining == 0 && !ProcessChunk())
    {
        return; // Message has been fully received, no need to schedule a receive.
    }

    ScheduleRecv();
}

void ReadSocketMessageHandle::Collect()
{
    WI_ASSERT(State == IOHandleStatus::Pending);

    State = IOHandleStatus::Standby;

    DWORD bytesRead{};
    DWORD flags{};
    if (!WSAGetOverlappedResult(reinterpret_cast<SOCKET>(Socket.Get()), &Overlapped, &bytesRead, FALSE, &flags))
    {
        long error = WSAGetLastError();
        THROW_WIN32_IF(error, error != WSAECONNABORTED && error != WSAECONNRESET);

        WI_ASSERT(bytesRead == 0);
    }

    ProcessRecvResult(bytesRead);
}

HANDLE ReadSocketMessageHandle::GetHandle() const
{
    return Event.get();
}

// WriteHandle

WriteHandle::WriteHandle(HandleWrapper&& MovedHandle, const std::vector<char>& Source, bool CompleteOnDrained) :
    Handle(std::move(MovedHandle)), Buffer(Source.size()), Offset(InitializeFileOffset(Handle.Get())), CompleteOnDrained(CompleteOnDrained)
{
    if (!Source.empty())
    {
        std::memcpy(Buffer.Span().data(), Source.data(), Source.size());
    }

    Overlapped.hEvent = Event.get();

    if (!CompleteOnDrained && Buffer.Size() == 0)
    {
        State = IOHandleStatus::Idle;
    }
}

WriteHandle::WriteHandle(HandleWrapper&& MovedHandle, gsl::span<gsl::byte> Source) :
    Handle(std::move(MovedHandle)), Buffer(Source), Offset(InitializeFileOffset(Handle.Get()))
{
    Overlapped.hEvent = Event.get();
}

WriteHandle::~WriteHandle()
{
    if (State == IOHandleStatus::Pending)
    {
        CancelPendingIo(Handle.Get(), Overlapped);
    }
}

void WriteHandle::SetCompleteOnDrained(bool Value)
{
    CompleteOnDrained = Value;
}

IOHandleStatus WriteHandle::DrainedState() const
{
    if (CompleteOnDrained)
    {
        return IOHandleStatus::Completed;
    }

    return Pending.empty() ? IOHandleStatus::Idle : IOHandleStatus::Standby;
}

void WriteHandle::Schedule()
{
    WI_ASSERT(State == IOHandleStatus::Standby);

    if (!Pending.empty())
    {
        Buffer.Append(gsl::make_span(Pending));
        Pending.clear();
    }

    if (Buffer.Size() == 0)
    {
        State = DrainedState();
        return;
    }

    Event.ResetEvent();

    Overlapped.Offset = Offset.LowPart;
    Overlapped.OffsetHigh = Offset.HighPart;

    // Schedule the write.
    const auto buffer = Buffer.Span();
    DWORD bytesWritten{};
    if (WriteFile(Handle.Get(), buffer.data(), static_cast<DWORD>(buffer.size()), &bytesWritten, &Overlapped))
    {
        Offset.QuadPart += bytesWritten;

        Buffer.Consume(bytesWritten);
        if (Buffer.Size() == 0)
        {
            State = DrainedState();
        }
    }
    else
    {
        auto error = GetLastError();
        THROW_LAST_ERROR_IF_MSG(error != ERROR_IO_PENDING, "Handle: 0x%p", (void*)Handle.Get());

        // The write is pending, update to 'Pending'
        State = IOHandleStatus::Pending;
    }
}

void WriteHandle::Collect()
{
    WI_ASSERT(State == IOHandleStatus::Pending);

    // Transition back to standby
    State = IOHandleStatus::Standby;

    // Complete the write.
    DWORD bytesWritten{};
    THROW_IF_WIN32_BOOL_FALSE(GetOverlappedResult(Handle.Get(), &Overlapped, &bytesWritten, false));
    Offset.QuadPart += bytesWritten;

    Buffer.Consume(bytesWritten);
    if (Buffer.Size() == 0)
    {
        State = DrainedState();
    }
}

void WriteHandle::Push(const gsl::span<char>& Content)
{
    WI_ASSERT(!Content.empty());

    // Put any pending output to a different buffer, since the active buffer could be in the middle of a write.
    Pending.insert(Pending.end(), Content.begin(), Content.end());

    if (State == IOHandleStatus::Idle)
    {
        State = IOHandleStatus::Standby;
    }
}

size_t WriteHandle::PendingBytes() const
{
    return Pending.size() + Buffer.Size();
}

HANDLE WriteHandle::GetHandle() const
{
    return Event.get();
}

WriteNamedPipe::WriteNamedPipe(HandleWrapper&& MovedPipe, bool Reconnect, bool Connected) :
    Pipe(std::move(MovedPipe)), ReconnectOnFailure(Reconnect), NeedConnect(!Connected)
{
    ConnectOverlapped.hEvent = ConnectEvent.get();

    Write.emplace(HandleWrapper{Pipe.Get()}, std::vector<char>{}, false);

    State = IOHandleStatus::Idle;
}

WriteNamedPipe::~WriteNamedPipe()
{
    if (Connecting)
    {
        CancelPendingIo(Pipe.Get(), ConnectOverlapped);
    }
}

void WriteNamedPipe::Reconnect()
{
    // Drop the disconnected client so a new one can connect, and retry the buffered data once reconnected.
    LOG_IF_WIN32_BOOL_FALSE(DisconnectNamedPipe(Pipe.Get()));

    NeedConnect = true;
    State = IOHandleStatus::Standby;
}

void WriteNamedPipe::Schedule()
{
    WI_ASSERT(State == IOHandleStatus::Standby);

    if (NeedConnect)
    {
        ConnectEvent.ResetEvent();
        ConnectOverlapped.Offset = 0;
        ConnectOverlapped.OffsetHigh = 0;

        if (!ConnectNamedPipe(Pipe.Get(), &ConnectOverlapped))
        {
            const auto error = GetLastError();
            if (error == ERROR_IO_PENDING)
            {
                Connecting = true;
                State = IOHandleStatus::Pending;
                return;
            }

            THROW_HR_IF_MSG(HRESULT_FROM_WIN32(error), error != ERROR_PIPE_CONNECTED, "Handle: 0x%p", (void*)Pipe.Get());
        }

        NeedConnect = false;
    }

    try
    {
        Write->Schedule();
        State = Write->GetState();
    }
    catch (...)
    {
        if (!ReconnectOnFailure)
        {
            throw;
        }

        LOG_CAUGHT_EXCEPTION();
        Reconnect();
    }
}

void WriteNamedPipe::Collect()
{
    WI_ASSERT(State == IOHandleStatus::Pending);

    // Complete a pending connection, then let the loop schedule the first write.
    if (Connecting)
    {
        Connecting = false;

        DWORD bytes{};
        if (!GetOverlappedResult(Pipe.Get(), &ConnectOverlapped, &bytes, FALSE))
        {
            const auto error = GetLastError();
            THROW_HR_IF_MSG(HRESULT_FROM_WIN32(error), error != ERROR_PIPE_CONNECTED, "Handle: 0x%p", (void*)Pipe.Get());
        }

        NeedConnect = false;
        State = IOHandleStatus::Standby;
        return;
    }

    try
    {
        Write->Collect();
        State = Write->GetState();
    }
    catch (...)
    {
        if (!ReconnectOnFailure)
        {
            throw;
        }

        LOG_CAUGHT_EXCEPTION();
        Reconnect();
    }
}

HANDLE WriteNamedPipe::GetHandle() const
{
    return Connecting ? ConnectEvent.get() : Write->GetHandle();
}

void WriteNamedPipe::Push(const gsl::span<char>& Content)
{
    Write->Push(Content);

    if (State == IOHandleStatus::Idle)
    {
        State = IOHandleStatus::Standby;
    }
}

size_t WriteNamedPipe::PendingBytes() const
{
    return Write ? Write->PendingBytes() : 0;
}

// DockerIORelayHandle

DockerIORelayHandle::DockerIORelayHandle(HandleWrapper&& ReadHandle, HandleWrapper&& Stdout, HandleWrapper&& Stderr, Format ReadFormat) :
    WriteStdout(std::move(Stdout), {}, false), WriteStderr(std::move(Stderr), {}, false)
{
    if (ReadFormat == Format::HttpChunked)
    {
        Read = std::make_unique<HTTPChunkBasedReadHandle>(
            std::move(ReadHandle), [this](const gsl::span<char>& Line) { this->OnRead(Line); });
    }
    else
    {
        Read =
            std::make_unique<io::ReadHandle>(std::move(ReadHandle), [this](const gsl::span<char>& Buffer) { this->OnRead(Buffer); });
    }
}

void DockerIORelayHandle::Schedule()
{
    WI_ASSERT(State == IOHandleStatus::Standby);
    WI_ASSERT(Read->GetState() != IOHandleStatus::Pending);

    // If we have an active handle and a buffer, try to flush that first.
    if (ActiveHandle != nullptr && !PendingBuffer.empty())
    {
        // Push the data to the selected handle.
        DWORD bytesToWrite = std::min(static_cast<DWORD>(RemainingBytes), static_cast<DWORD>(PendingBuffer.size()));

        ActiveHandle->Push(gsl::make_span(PendingBuffer.data(), bytesToWrite));

        // Consume the written bytes.
        RemainingBytes -= bytesToWrite;
        PendingBuffer.erase(PendingBuffer.begin(), PendingBuffer.begin() + bytesToWrite);

        // Schedule the write.
        ActiveHandle->Schedule();

        // If the write is pending, update to 'Pending'
        if (ActiveHandle->GetState() == IOHandleStatus::Pending)
        {
            State = IOHandleStatus::Pending;
        }
        else if (ActiveHandle->GetState() == IOHandleStatus::Completed || ActiveHandle->GetState() == IOHandleStatus::Idle)
        {
            if (RemainingBytes == 0)
            {
                // Switch back to reading if we've written all bytes for this chunk.
                ActiveHandle = nullptr;

                ProcessNextHeader();
            }
        }
    }
    else
    {
        if (Read->GetState() == IOHandleStatus::Completed)
        {
            LOG_HR_IF(E_UNEXPECTED, ActiveHandle != nullptr);

            // No more data to read, we're done.
            State = IOHandleStatus::Completed;
            return;
        }

        // Schedule a read from the input.
        Read->Schedule();
        if (Read->GetState() == IOHandleStatus::Pending)
        {
            State = IOHandleStatus::Pending;
        }
    }
}

void DockerIORelayHandle::Collect()
{
    WI_ASSERT(State == IOHandleStatus::Pending);

    if (ActiveHandle != nullptr && ActiveHandle->GetState() == IOHandleStatus::Pending)
    {
        // Complete the write.
        ActiveHandle->Collect();

        // If the write is completed, switch back to reading.
        if (RemainingBytes == 0)
        {
            if (ActiveHandle->GetState() == IOHandleStatus::Completed || ActiveHandle->GetState() == IOHandleStatus::Idle)
            {
                ActiveHandle = nullptr;

                ProcessNextHeader();
            }
        }

        // Transition back to standby if there's still data to read.
        // Otherwise switch to Completed since everything is done.
        if (Read->GetState() == IOHandleStatus::Completed)
        {
            LOG_HR_IF(E_UNEXPECTED, RemainingBytes != 0);

            State = IOHandleStatus::Completed;
        }
        else
        {
            State = IOHandleStatus::Standby;
        }
    }
    else
    {
        WI_ASSERT(Read->GetState() == IOHandleStatus::Pending);

        // Complete the read.
        Read->Collect();

        // Transition back to standby.
        State = IOHandleStatus::Standby;
    }
}

HANDLE DockerIORelayHandle::GetHandle() const
{
    if (ActiveHandle != nullptr && ActiveHandle->GetState() == IOHandleStatus::Pending)
    {
        return ActiveHandle->GetHandle();
    }
    else
    {
        return Read->GetHandle();
    }
}

void DockerIORelayHandle::ProcessNextHeader()
{
    if (PendingBuffer.size() < sizeof(MultiplexedHeader))
    {
        // Not enough data for a header yet.
        return;
    }

    const auto* header = reinterpret_cast<const MultiplexedHeader*>(PendingBuffer.data());
    RemainingBytes = ntohl(header->Length);

    if (header->Fd == 1)
    {
        ActiveHandle = &WriteStdout;
    }
    else if (header->Fd == 2)
    {
        ActiveHandle = &WriteStderr;
    }
    else
    {
        THROW_HR_MSG(E_INVALIDARG, "Invalid Docker IO multiplexed header fd: %u", header->Fd);
    }

    // Consume the header.
    PendingBuffer.erase(PendingBuffer.begin(), PendingBuffer.begin() + sizeof(MultiplexedHeader));
}

void DockerIORelayHandle::OnRead(const gsl::span<char>& Buffer)
{
    PendingBuffer.insert(PendingBuffer.end(), Buffer.begin(), Buffer.end());

    if (ActiveHandle == nullptr)
    {
        // If no handle is active, expect a header.
        ProcessNextHeader();
    }
}

// MultiHandleWait

MultiHandleWait::MultiHandleWait(MultiHandleWait&& other) noexcept
{
    *this = std::move(other);
}

MultiHandleWait& MultiHandleWait::operator=(MultiHandleWait&& other) noexcept
{
    if (this != &other)
    {
        m_handles = std::move(other.m_handles);
        m_handleSignaledEvent = std::move(other.m_handleSignaledEvent);
        m_cancel = other.m_cancel;

        for (auto& entry : m_handles)
        {
            entry->self = this;
        }

        // N.B. moving a MultiHandleWait() while running is not supported
        WI_ASSERT(m_signaledHandles.empty());
    }

    return *this;
}

void MultiHandleWait::AddHandle(std::unique_ptr<OverlappedIOHandle>&& handle, Flags flags)
{
    auto entry = std::make_unique<Entry>();
    entry->HandleFlags = flags;
    entry->Handle = std::move(handle);
    entry->self = this;
    m_handles.emplace_back(std::move(entry));
}

void MultiHandleWait::Cancel()
{
    m_cancel = true;
}

void NTAPI MultiHandleWait::WaitCallback(PVOID Context, BOOLEAN /*TimerOrWaitFired*/)
{
    auto* entry = static_cast<Entry*>(Context);

    entry->self->m_signaledHandles.push(entry);
    entry->self->m_handleSignaledEvent.SetEvent();
}

bool MultiHandleWait::Run(std::optional<std::chrono::milliseconds> Timeout)
{
    m_cancel = false; // Run may be called multiple times.

    std::optional<std::chrono::steady_clock::time_point> deadline;
    if (Timeout.has_value())
    {
        deadline = std::chrono::steady_clock::now() + Timeout.value();
    }

    std::vector<unique_registered_wait> callbacks;

    while (!m_cancel)
    {
        // Cancel any pending callback.
        callbacks.clear();

        Entry* signaledEntry = nullptr;
        while (m_signaledHandles.try_pop(signaledEntry))
        {
            try
            {
                signaledEntry->Handle->Collect();
            }
            catch (...)
            {
                if (WI_IsFlagSet(signaledEntry->HandleFlags, Flags::IgnoreErrors))
                {
                    signaledEntry->Handle.reset();
                    continue;
                }

                throw;
            }
        }

        m_handleSignaledEvent.ResetEvent();

        bool hasHandleToWaitFor = false;
        for (auto it = m_handles.begin(); it != m_handles.end();)
        {
            auto& entry = **it;

            while (entry.Handle && entry.Handle->GetState() == IOHandleStatus::Standby && !m_cancel)
            {
                try
                {
                    entry.Handle->Schedule();
                }
                catch (...)
                {
                    if (WI_IsFlagSet(entry.HandleFlags, Flags::IgnoreErrors))
                    {
                        entry.Handle.reset();
                        break;
                    }

                    throw;
                }
            }

            if (!entry.Handle || entry.Handle->GetState() == IOHandleStatus::Completed)
            {
                if (entry.Handle && WI_IsFlagSet(entry.HandleFlags, Flags::CancelOnCompleted))
                {
                    m_cancel = true;
                }

                it = m_handles.erase(it);
                continue;
            }

            // N.B. An Idle handle cannot be waited for since it's not doing any IO.
            if (entry.Handle->GetState() == IOHandleStatus::Idle)
            {
                ++it;
                continue;
            }

            auto& callback = callbacks.emplace_back();

            THROW_IF_WIN32_BOOL_FALSE(RegisterWaitForSingleObject(
                &callback, entry.Handle->GetHandle(), &WaitCallback, &entry, INFINITE, WT_EXECUTEINWAITTHREAD | WT_EXECUTEONLYONCE));

            if (WI_IsFlagClear(entry.HandleFlags, Flags::NeedNotComplete))
            {
                hasHandleToWaitFor = true;
            }

            ++it;
        }

        if (m_handles.empty() || !hasHandleToWaitFor || m_cancel)
        {
            break;
        }

        DWORD waitTimeout = INFINITE;
        if (deadline.has_value())
        {
            auto milliseconds =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline.value() - std::chrono::steady_clock::now()).count();

            waitTimeout = static_cast<DWORD>(std::max<long long>(0, milliseconds));
        }

        THROW_HR_IF_MSG(
            HRESULT_FROM_WIN32(ERROR_TIMEOUT),
            !m_handleSignaledEvent.wait(waitTimeout),
            "Timed out waiting for %llu handles. Timeout: %lu",
            m_handles.size(),
            waitTimeout);
    }

    return !m_cancel;
}
