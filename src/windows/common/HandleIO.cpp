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
using wsl::windows::common::io::ReadSocketMessageHandle;
using wsl::windows::common::io::SingleAcceptHandle;
using wsl::windows::common::io::WriteHandle;

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

void CancelPendingIo(auto Handle, OVERLAPPED& Overlapped)
{
    DWORD bytesTransferred{};
    if (CancelIoEx((HANDLE)Handle, &Overlapped))
    {
        if constexpr (std::is_same_v<decltype(Handle), SOCKET>)
        {
            if (!WSAGetOverlappedResult(Handle, &Overlapped, &bytesTransferred, true, nullptr))
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
        // ERROR_NOT_FOUND is returned if there was no IO to cancel.
        LOG_LAST_ERROR_IF(GetLastError() != ERROR_NOT_FOUND);
    }
}

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
    HandleWrapper&& MovedSocket, std::vector<gsl::byte>& Buffer, std::function<void(const gsl::span<gsl::byte>& Message)>&& OnMessage) :
    Socket(std::move(MovedSocket)), Buffer(Buffer), OnMessage(std::move(OnMessage))
{
    Overlapped.hEvent = Event.get();

    if (Buffer.size() < sizeof(MESSAGE_HEADER))
    {
        Buffer.resize(sizeof(MESSAGE_HEADER));
    }
}

ReadSocketMessageHandle::~ReadSocketMessageHandle()
{
    if (State == IOHandleStatus::Pending)
    {
        CancelPendingIo((SOCKET)Socket.Get(), Overlapped);
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

    if (ReadingHeader)
    {
        auto messageSize = gslhelpers::get_struct<MESSAGE_HEADER>(gsl::make_span(Buffer.data(), sizeof(MESSAGE_HEADER)))->MessageSize;

        THROW_HR_IF_MSG(E_UNEXPECTED, messageSize < sizeof(MESSAGE_HEADER), "Unexpected message size: %u", messageSize);
        THROW_HR_IF_MSG(E_UNEXPECTED, messageSize > 4 * 1024 * 1024, "Message size too large: %u", messageSize);

        if (messageSize == sizeof(MESSAGE_HEADER))
        {
            OnMessage(gsl::make_span(Buffer.data(), messageSize));
            State = IOHandleStatus::Completed;
            return;
        }

        if (Buffer.size() < messageSize)
        {
            Buffer.resize(messageSize);
        }

        ReadingHeader = false;
        CurrentOffset = sizeof(MESSAGE_HEADER);
        BytesRemaining = messageSize - sizeof(MESSAGE_HEADER);
    }
    else
    {
        auto messageSize = gslhelpers::get_struct<MESSAGE_HEADER>(gsl::make_span(Buffer.data(), sizeof(MESSAGE_HEADER)))->MessageSize;
        OnMessage(gsl::make_span(Buffer.data(), messageSize));
        State = IOHandleStatus::Completed;
    }
}

void ReadSocketMessageHandle::Schedule()
{
    WI_ASSERT(State == IOHandleStatus::Standby);
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

WriteHandle::WriteHandle(HandleWrapper&& MovedHandle, const std::vector<char>& Source) :
    Handle(std::move(MovedHandle)), Buffer(Source.size()), Offset(InitializeFileOffset(Handle.Get()))
{
    std::memcpy(Buffer.Span().data(), Source.data(), Source.size());
    Overlapped.hEvent = Event.get();
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

void WriteHandle::Schedule()
{
    WI_ASSERT(State == IOHandleStatus::Standby);

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
            State = IOHandleStatus::Completed;
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
        State = IOHandleStatus::Completed;
    }
}

void WriteHandle::Push(const gsl::span<char>& Content)
{
    // Don't write if a WriteFile() is pending, since that could cause the buffer to reallocate.
    WI_ASSERT(State == IOHandleStatus::Standby || State == IOHandleStatus::Completed);
    WI_ASSERT(!Content.empty());

    // Resize() throws E_UNEXPECTED if Buffer does not own its storage.
    Buffer.Append(Content);

    State = IOHandleStatus::Standby;
}

HANDLE WriteHandle::GetHandle() const
{
    return Event.get();
}

// DockerIORelayHandle

DockerIORelayHandle::DockerIORelayHandle(HandleWrapper&& ReadHandle, HandleWrapper&& Stdout, HandleWrapper&& Stderr, Format ReadFormat) :
    WriteStdout(std::move(Stdout)), WriteStderr(std::move(Stderr))
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
        else if (ActiveHandle->GetState() == IOHandleStatus::Completed)
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
            if (ActiveHandle->GetState() == IOHandleStatus::Completed)
            {
                ActiveHandle = nullptr;
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
        for (size_t i = 0; i < m_handles.size() && !m_cancel; i++)
        {
            while (m_handles[i].second->GetState() == IOHandleStatus::Standby && !m_cancel)
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
                        break;
                    }
                    else
                    {
                        throw;
                    }
                }
            }
        }

        // Remove completed handles from m_handles.
        bool hasHandleToWaitFor = false;
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
                // If only NeedNotComplete handles are left, we want to exit Run.
                if (WI_IsFlagClear(it->first, Flags::NeedNotComplete))
                {
                    hasHandleToWaitFor = true;
                }
                ++it;
            }
        }

        if (!hasHandleToWaitFor || m_cancel)
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
