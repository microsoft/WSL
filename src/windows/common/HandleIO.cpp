// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "HandleIO.h"
#pragma hdrstop

using wsl::windows::common::io::AcceptHandle;
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

#define TTY_ALT_NUMPAD_VK_MENU (0x12)
#define TTY_ESCAPE_CHARACTER (L'\x1b')
#define TTY_INPUT_EVENT_BUFFER_SIZE (16)
#define TTY_UTF8_TRANSLATION_BUFFER_SIZE (4 * TTY_INPUT_EVENT_BUFFER_SIZE)

BOOL IsActionableKey(_In_ PKEY_EVENT_RECORD KeyEvent)
{
    //
    // This is a bit complicated to discern.
    //
    // 1. Our first check is that we only want structures that
    //    represent at least one key press. If we have 0, then we don't
    //    need to bother. If we have >1, we'll send the key through
    //    that many times into the pipe.
    // 2. Our second check is where it gets confusing.
    //    a. Characters that are non-null get an automatic pass. Copy
    //       them through to the pipe.
    //    b. Null characters need further scrutiny. We generally do not
    //       pass nulls through EXCEPT if they're sourced from the
    //       virtual terminal engine (or another application living
    //       above our layer). If they're sourced by a non-keyboard
    //       source, they'll have no scan code (since they didn't come
    //       from a keyboard). But that rule has an exception too:
    //       "Enhanced keys" from above the standard range of scan
    //       codes will return 0 also with a special flag set that says
    //       they're an enhanced key. That means the desired behavior
    //       is:
    //           Scan Code = 0, ENHANCED_KEY = 0
    //               -> This came from the VT engine or another app
    //                  above our layer.
    //           Scan Code = 0, ENHANCED_KEY = 1
    //               -> This came from the keyboard, but is a special
    //                  key like 'Volume Up' that wasn't generally a
    //                  part of historic (pre-1990s) keyboards.
    //           Scan Code = <anything else>
    //               -> This came from a keyboard directly.
    //

    if ((KeyEvent->wRepeatCount == 0) || ((KeyEvent->uChar.UnicodeChar == UNICODE_NULL) &&
                                          ((KeyEvent->wVirtualScanCode != 0) || (WI_IsFlagSet(KeyEvent->dwControlKeyState, ENHANCED_KEY)))))
    {
        return FALSE;
    }

    return TRUE;
}

BOOL GetNextCharacter(_In_ INPUT_RECORD* InputRecord, _Out_ PWCHAR NextCharacter)
{
    BOOL IsNextCharacterValid = FALSE;
    if (InputRecord->EventType == KEY_EVENT)
    {
        const auto KeyEvent = &InputRecord->Event.KeyEvent;
        if ((IsActionableKey(KeyEvent) != FALSE) && ((KeyEvent->bKeyDown != FALSE) || (KeyEvent->wVirtualKeyCode == TTY_ALT_NUMPAD_VK_MENU)))
        {
            *NextCharacter = KeyEvent->uChar.UnicodeChar;
            IsNextCharacterValid = TRUE;
        }
    }

    return IsNextCharacterValid;
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

// AcceptHandle

AcceptHandle::AcceptHandle(HandleWrapper&& ListenSocket, bool AcceptOnce, std::function<void(wil::unique_socket&&)>&& OnAccepted) :
    ListenSocket(std::move(ListenSocket)), AcceptOnce(AcceptOnce), OnAccepted(std::move(OnAccepted))
{
    Overlapped.hEvent = Event.get();

    // Query the listen socket so accepted sockets can be created with a matching address family, type, and protocol.
    WSAPROTOCOL_INFOW protocolInfo{};
    int length = sizeof(protocolInfo);
    THROW_LAST_ERROR_IF(
        getsockopt(reinterpret_cast<SOCKET>(this->ListenSocket.Get()), SOL_SOCKET, SO_PROTOCOL_INFOW, reinterpret_cast<char*>(&protocolInfo), &length) ==
        SOCKET_ERROR);

    AddressFamily = protocolInfo.iAddressFamily;
    SocketType = protocolInfo.iSocketType;
    Protocol = protocolInfo.iProtocol;
}

AcceptHandle::~AcceptHandle()
{
    if (State == IOHandleStatus::Pending)
    {
        CancelPendingIo(reinterpret_cast<SOCKET>(ListenSocket.Get()), Overlapped);
    }
}

void AcceptHandle::CreateAcceptSocket()
{
    AcceptedSocket.reset(WSASocketW(AddressFamily, SocketType, Protocol, nullptr, 0, WSA_FLAG_OVERLAPPED));
    THROW_LAST_ERROR_IF(!AcceptedSocket);

    if (AddressFamily == AF_HYPERV)
    {
        ULONG enable = 1;
        THROW_LAST_ERROR_IF(
            setsockopt(AcceptedSocket.get(), HV_PROTOCOL_RAW, HVSOCKET_CONNECTED_SUSPEND, reinterpret_cast<char*>(&enable), sizeof(enable)) ==
            SOCKET_ERROR);
    }
}

void AcceptHandle::OnComplete()
{
    wsl::windows::common::socket::SetAcceptContext(AcceptedSocket.get(), reinterpret_cast<SOCKET>(ListenSocket.Get()));

    OnAccepted(std::move(AcceptedSocket));

    if (AcceptOnce)
    {
        State = IOHandleStatus::Completed;
    }
    else
    {
        State = IOHandleStatus::Standby;
    }
}

void AcceptHandle::Schedule()
{
    WI_ASSERT(State == IOHandleStatus::Standby);

    CreateAcceptSocket();

    Event.ResetEvent();

    // Schedule the accept.
    DWORD bytesReturned{};
    if (AcceptEx((SOCKET)ListenSocket.Get(), AcceptedSocket.get(), &AcceptBuffer, 0, sizeof(SOCKADDR_STORAGE), sizeof(SOCKADDR_STORAGE), &bytesReturned, &Overlapped))
    {
        // Accept completed immediately.
        OnComplete();
    }
    else
    {
        auto error = WSAGetLastError();
        THROW_HR_IF_MSG(HRESULT_FROM_WIN32(error), error != ERROR_IO_PENDING, "Handle: 0x%p", (void*)ListenSocket.Get());

        State = IOHandleStatus::Pending;
    }
}

void AcceptHandle::Collect()
{
    WI_ASSERT(State == IOHandleStatus::Pending);

    DWORD bytesReceived{};
    DWORD flagsReturned{};

    THROW_IF_WIN32_BOOL_FALSE(WSAGetOverlappedResult((SOCKET)ListenSocket.Get(), &Overlapped, &bytesReceived, false, &flagsReturned));

    OnComplete();
}

HANDLE AcceptHandle::GetHandle() const
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

// ReadConsoleHandle

wsl::windows::common::io::ReadConsoleHandle::ReadConsoleHandle(
    HandleWrapper&& Console,
    std::function<void(const gsl::span<char>& Buffer)>&& OnRead,
    std::function<void()>&& UpdateTerminalSize,
    std::vector<char> DetachSequence,
    std::function<void()>&& OnDetach) :
    Console(std::move(Console)),
    OnRead(std::move(OnRead)),
    UpdateTerminalSize(std::move(UpdateTerminalSize)),
    DetachSequence(std::move(DetachSequence)),
    OnDetach(std::move(OnDetach))
{
}

void wsl::windows::common::io::ReadConsoleHandle::Schedule()
{
    WI_ASSERT(State == IOHandleStatus::Standby);

    //
    // Use the console handle as the signal event.
    // N.B. This behavior is documented here: https://learn.microsoft.com/en-us/windows/console/readconsoleinput
    //

    State = IOHandleStatus::Pending;
}

HANDLE wsl::windows::common::io::ReadConsoleHandle::GetHandle() const
{
    return Console.Get();
}

void wsl::windows::common::io::ReadConsoleHandle::Collect()
{
    WI_ASSERT(State == IOHandleStatus::Pending);

    //
    // Re-arm by default; a detected detach sequence overrides this to Completed below.
    //

    State = IOHandleStatus::Standby;

    //
    // N.B. ReadConsoleInputEx has no associated import library.
    //

    static LxssDynamicFunction<decltype(ReadConsoleInputExW)> readConsoleInput(L"Kernel32.dll", "ReadConsoleInputExW");

    INPUT_RECORD InputRecordBuffer[TTY_INPUT_EVENT_BUFFER_SIZE];
    INPUT_RECORD* InputRecordPeek = &(InputRecordBuffer[1]);
    KEY_EVENT_RECORD* KeyEvent;
    DWORD RecordsRead;

    //
    // The console handle stays signaled while input is available, so drain all currently available
    // input here and return to Standby once none remains (the handle is waited on again by the IO loop).
    //

    for (;;)
    {
        // Detach if the escape sequence was detected.
        // N.B. This needs to be done at the beginning of the loop so the escape sequence is also delivered.
        if (!CurrentSequence.empty() && std::ranges::equal(CurrentSequence, DetachSequence))
        {
            OnDetach();
            State = IOHandleStatus::Completed;
            return;
        }

        //
        // Because some input events generated by the console are encoded with
        // more than one input event, we have to be smart about reading the
        // events.
        //
        // First, we peek at the next input event.
        // If it's an escape (wch == L'\x1b') event, then the characters that
        //      follow are part of an input sequence. We can't know for sure
        //      how long that sequence is, but we can assume it's all sent to
        //      the input queue at once, and it's less that 16 events.
        //      Furthermore, we can assume that if there's an Escape in those
        //      16 events, that the escape marks the start of a new sequence.
        //      So, we'll peek at another 15 events looking for escapes.
        //      If we see an escape, then we'll read one less than that,
        //      such that the escape remains the next event in the input.
        //      From those read events, we'll aggregate chars into a single
        //      string to send to the subsystem.
        // If it's not an escape, send the event through one at a time.
        //

        //
        // Read one input event without blocking. If none is available, all input has been drained.
        //

        THROW_IF_WIN32_BOOL_FALSE(readConsoleInput(Console.Get(), InputRecordBuffer, 1, &RecordsRead, CONSOLE_READ_NOWAIT));
        if (RecordsRead == 0)
        {
            return;
        }

        //
        // Don't read additional records if the first entry is a window size
        // event, or a repeated character. Handle those events on their own.
        //

        DWORD RecordsPeeked = 0;
        if ((InputRecordBuffer[0].EventType != WINDOW_BUFFER_SIZE_EVENT) &&
            ((InputRecordBuffer[0].EventType != KEY_EVENT) || (InputRecordBuffer[0].Event.KeyEvent.wRepeatCount < 2)))
        {
            //
            // Read additional input records into the buffer if available.
            //

            THROW_IF_WIN32_BOOL_FALSE(PeekConsoleInputW(Console.Get(), InputRecordPeek, (RTL_NUMBER_OF(InputRecordBuffer) - 1), &RecordsPeeked));
        }

        //
        // Iterate over peeked records [1, RecordsPeeked].
        //

        DWORD AdditionalRecordsToRead = 0;
        WCHAR NextCharacter;
        for (DWORD RecordIndex = 1; RecordIndex <= RecordsPeeked; RecordIndex++)
        {
            if (GetNextCharacter(&InputRecordBuffer[RecordIndex], &NextCharacter) != FALSE)
            {
                KeyEvent = &InputRecordBuffer[RecordIndex].Event.KeyEvent;
                if (NextCharacter == TTY_ESCAPE_CHARACTER)
                {
                    //
                    // CurrentRecord is an escape event. We will start here
                    // on the next input loop.
                    //

                    break;
                }
                else if (KeyEvent->wRepeatCount > 1)
                {
                    //
                    // Repeated keys are handled on their own. Start with this
                    // key on the next input loop.
                    //

                    break;
                }
                else if (IS_HIGH_SURROGATE(NextCharacter) && (RecordIndex >= (RecordsPeeked - 1)))
                {
                    //
                    // If there is not enough room for the second character of
                    // a surrogate pair, start with this character on the next
                    // input loop.
                    //
                    // N.B. The test is for at least two remaining records
                    //      because typically a surrogate pair will be entered
                    //      via copy/paste, which will appear as an input
                    //      record with alt-down, alt-up and character. So to
                    //      include the next character of the surrogate pair it
                    //      is likely that the alt-up record will need to be
                    //      read first.
                    //

                    break;
                }
            }
            else if (InputRecordBuffer[RecordIndex].EventType == WINDOW_BUFFER_SIZE_EVENT)
            {
                //
                // A window size event is handled on its own.
                //

                break;
            }

            //
            // Process the additional input record.
            //

            AdditionalRecordsToRead += 1;
        }

        if (AdditionalRecordsToRead > 0)
        {
            THROW_IF_WIN32_BOOL_FALSE(readConsoleInput(Console.Get(), InputRecordPeek, AdditionalRecordsToRead, &RecordsRead, CONSOLE_READ_NOWAIT));

            if (RecordsRead == 0)
            {
                //
                // This would be an unexpected case. We've already peeked to see
                // that there are AdditionalRecordsToRead # of records in the
                // input that need reading, yet we didn't get them when we read.
                // In this case, stop draining and wait to be signaled again.
                //

                return;
            }

            //
            // We already had one input record in the buffer before reading
            // additional, So account for that one too
            //

            RecordsRead += 1;
        }

        //
        // Process each input event. Keydowns will get aggregated into
        // Utf8String before getting injected into the subsystem.
        //

        WCHAR Utf16String[TTY_INPUT_EVENT_BUFFER_SIZE];
        ULONG Utf16StringSize = 0;
        for (DWORD RecordIndex = 0; RecordIndex < RecordsRead; RecordIndex++)
        {
            INPUT_RECORD* CurrentInputRecord = &(InputRecordBuffer[RecordIndex]);
            switch (CurrentInputRecord->EventType)
            {
            case KEY_EVENT:

                KeyEvent = &CurrentInputRecord->Event.KeyEvent;

                if (KeyEvent->bKeyDown && IsActionableKey(KeyEvent) && !DetachSequence.empty())
                {
                    if (CurrentSequence.size() >= DetachSequence.size())
                    {
                        CurrentSequence.pop_front();
                    }

                    CurrentSequence.push_back(CurrentInputRecord->Event.KeyEvent.uChar.AsciiChar);
                }

                //
                // Filter out key up events unless they are from an <Alt> key.
                // Key up with an <Alt> key could contain a Unicode character
                // pasted from the clipboard and converted to an <Alt>+<Numpad> sequence.
                //

                if ((KeyEvent->bKeyDown == FALSE) && (KeyEvent->wVirtualKeyCode != TTY_ALT_NUMPAD_VK_MENU))
                {
                    break;
                }

                //
                // Filter out key presses that are not actionable, such as just
                // pressing <Ctrl>, <Alt>, <Shift> etc. These key presses return
                // the character of null but will have a valid scan code off the
                // keyboard. Certain other key sequences such as Ctrl+A,
                // Ctrl+<space>, and Ctrl+@ will also return the character null
                // but have no scan code.
                // <Alt> + <NumPad> sequences will show an <Alt> but will have
                // a scancode and character specified, so they should be actionable.
                //

                if (IsActionableKey(KeyEvent) == FALSE)
                {
                    break;
                }

                Utf16String[Utf16StringSize] = KeyEvent->uChar.UnicodeChar;
                Utf16StringSize += 1;
                break;

            case WINDOW_BUFFER_SIZE_EVENT:

                //
                // Query the window size and send an update message via the
                // control channel.
                //

                UpdateTerminalSize();
                break;
            }
        }

        CHAR Utf8String[TTY_UTF8_TRANSLATION_BUFFER_SIZE];
        DWORD Utf8StringSize = 0;
        if (Utf16StringSize > 0)
        {
            //
            // Windows uses UTF-16LE encoding, Linux uses UTF-8 by default.
            // Convert each UTF-16LE character into the proper UTF-8 byte
            // sequence equivalent.
            //

            THROW_LAST_ERROR_IF(
                (Utf8StringSize = WideCharToMultiByte(
                     CP_UTF8, 0, Utf16String, Utf16StringSize, Utf8String, sizeof(Utf8String), nullptr, nullptr)) == 0);
        }

        //
        // Deliver the translated input bytes.
        //

        const auto Utf8Span = gsl::make_span(Utf8String, static_cast<size_t>(Utf8StringSize));
        if ((RecordsRead == 1) && (InputRecordBuffer[0].EventType == KEY_EVENT) && (InputRecordBuffer[0].Event.KeyEvent.wRepeatCount > 1))
        {
            WI_ASSERT(Utf16StringSize == 1);

            //
            // Handle repeated characters. They aren't part of an input
            // sequence, so there's only one event that's generating characters.
            //

            for (WORD RepeatIndex = 0; RepeatIndex < InputRecordBuffer[0].Event.KeyEvent.wRepeatCount; RepeatIndex += 1)
            {
                OnRead(Utf8Span);
            }
        }
        else if (Utf8StringSize > 0)
        {
            OnRead(Utf8Span);
        }
    }
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
        THROW_LAST_ERROR_IF_MSG(error != ERROR_IO_PENDING, "Handle: 0x%p, size: %zu", (void*)Handle.Get(), buffer.size());

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
            entry->Self = this;
        }

        // N.B. moving a MultiHandleWait() while running is not supported
        WI_ASSERT(m_signaledHandles.empty());
    }

    return *this;
}

void MultiHandleWait::AddHandle(std::unique_ptr<OverlappedIOHandle>&& handle, Flags flags, OnError&& onError)
{
    auto entry = std::make_unique<Entry>();
    entry->HandleFlags = flags;
    entry->Handle = std::move(handle);
    entry->Self = this;

    if (WI_IsFlagSet(flags, Flags::IgnoreErrors))
    {
        entry->ErrorCallback = []() {};
    }
    else
    {
        entry->ErrorCallback = std::move(onError);
    }
    m_handles.emplace_back(std::move(entry));
}

void MultiHandleWait::Cancel()
{
    m_cancel = true;
}

void NTAPI MultiHandleWait::WaitCallback(PVOID Context, BOOLEAN /*TimerOrWaitFired*/)
{
    auto* entry = static_cast<Entry*>(Context);

    entry->Self->m_signaledHandles.push(entry);
    entry->Self->m_handleSignaledEvent.SetEvent();
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
                signaledEntry->ErrorCallback(); // Might throw and cancel the IO.
                signaledEntry->Handle.reset();
                continue;
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
                    entry.ErrorCallback(); // Might throw and cancel the IO.
                    entry.Handle.reset();
                    break;
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
