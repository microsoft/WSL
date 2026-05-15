// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#define LX_RELAY_BUFFER_SIZE 0x1000

namespace wsl::windows::common::io {

enum class IOHandleStatus
{
    Standby,
    Pending,
    Completed
};

struct HandleWrapper
{
    DEFAULT_MOVABLE(HandleWrapper);
    NON_COPYABLE(HandleWrapper)

    HandleWrapper(wil::unique_handle&& handle, std::function<void()>&& OnClose = []() {});
    HandleWrapper(wil::unique_socket&& handle, std::function<void()>&& OnClose = []() {});
    HandleWrapper(wil::unique_event&& handle, std::function<void()>&& OnClose = []() {});
    HandleWrapper(SOCKET handle, std::function<void()>&& OnClose = []() {});
    HandleWrapper(HANDLE handle, std::function<void()>&& OnClose = []() {});
    HandleWrapper(wil::unique_hfile&& handle, std::function<void()>&& OnClose = []() {});
    ~HandleWrapper();

    HANDLE Get() const;
    void Reset();

private:
    HANDLE Handle{};
    std::variant<wil::unique_handle, wil::unique_socket> OwnedHandle;
    std::function<void()> OnClose;
};

// A buffer that may either own its underlying storage (constructed from a size, allocating an
// internal std::vector<char>) or borrow it from a caller-provided gsl::span<gsl::byte>.
class BufferWrapper
{
public:
    DEFAULT_MOVABLE(BufferWrapper);
    NON_COPYABLE(BufferWrapper);

    explicit BufferWrapper(size_t size);
    explicit BufferWrapper(gsl::span<gsl::byte> span);

    bool Owned() const noexcept;
    void Resize(size_t size);
    void Append(gsl::span<char> Span);
    void Consume(size_t bytes) noexcept;
    gsl::span<gsl::byte> Span() noexcept;
    size_t Size() const noexcept;

private:
    std::optional<std::vector<char>> m_owned;
    gsl::span<gsl::byte> m_unowned;
};

// RAII helper for binding a file/socket to an I/O completion port. Bind detaches
// any prior IOCP association on the handle before installing the new one, so the
// same underlying file/socket can be re-bound to a different IOCP from a later
// MultiHandleWait instance even after the previous instance's IOCP has been
// closed. The destructor likewise detaches the binding so external consumers
// (e.g. boost::asio::stream::assign which calls CreateIoCompletionPort with its
// own IOCP) can take ownership of the kernel handle without hitting
// ERROR_INVALID_PARAMETER.
//
// Detach uses the undocumented NtSetInformationFile +
// FileReplaceCompletionInformation NTAPI (introduced in Windows 8.1).
//
// CreateIoCompletionPort returns ERROR_INVALID_PARAMETER for handles that don't
// support overlapped I/O - anonymous pipes from CreatePipe, console handles from
// GetStdHandle, and a few other device types. Bind silently treats those as a
// no-op; such handles are expected to complete their I/O synchronously inside
// Schedule() so no IOCP packet is required.
//
// IOCPHandle is owned exclusively by MultiHandleWait (via a per-MHW
// std::map<HANDLE, IOCPHandle>) so each kernel handle has at most one binding
// per wait. Two leaves wrapping the same kernel handle therefore share a single
// IOCPHandle and the destructor unconditionally detaches when the wait ends.
class IOCPHandle
{
public:
    IOCPHandle() = default;
    ~IOCPHandle();

    NON_COPYABLE(IOCPHandle)
    NON_MOVABLE(IOCPHandle)

    void Bind(HANDLE Handle, HANDLE Iocp, ULONG_PTR Key);

private:
    HANDLE m_handle = nullptr;
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

    // Hand the wait's IOCP and the leaf's per-entry completion key to this leaf and
    // return every (kernel handle, OVERLAPPED*) pair that should be associated with
    // the IOCP. MultiHandleWait calls Bind exactly once per leaf, when the leaf is
    // added via AddHandle.
    //
    // Most leaves ignore Iocp/Key and simply return their own (handle, &overlapped)
    // pair. EventHandle wraps an event - which cannot be associated with an IOCP -
    // and instead uses Iocp/Key to arm a thread pool wait that posts a completion
    // packet via PostQueuedCompletionStatus, so it returns no pairs.
    //
    // Composite handles forward Bind to every sub-handle and concatenate the
    // returned pairs.
    virtual std::vector<std::pair<HANDLE, OVERLAPPED*>> Bind(HANDLE Iocp, ULONG_PTR Key) = 0;

    IOHandleStatus GetState() const;

protected:
    IOHandleStatus State = IOHandleStatus::Standby;
};

class EventHandle : public OverlappedIOHandle
{
public:
    NON_COPYABLE(EventHandle)
    NON_MOVABLE(EventHandle)

    EventHandle(HandleWrapper&& Handle, std::function<void()>&& OnSignalled = []() {});
    void Schedule() override;
    void Collect() override;
    std::vector<std::pair<HANDLE, OVERLAPPED*>> Bind(HANDLE Iocp, ULONG_PTR Key) override;

private:
    static void NTAPI WaitCallback(PTP_CALLBACK_INSTANCE Instance, PVOID Context, PTP_WAIT Wait, TP_WAIT_RESULT WaitResult);

    HandleWrapper Handle;
    std::function<void()> OnSignalled;
    HANDLE m_iocp{};
    ULONG_PTR m_completionKey{};
    wil::unique_threadpool_wait m_threadpoolWait;
};

class ReadHandle : public OverlappedIOHandle
{
public:
    NON_COPYABLE(ReadHandle);
    NON_MOVABLE(ReadHandle);

    ReadHandle(HandleWrapper&& MovedHandle, std::function<void(const gsl::span<char>& Buffer)>&& OnRead);
    virtual ~ReadHandle();

    void Schedule() override;
    void Collect() override;
    std::vector<std::pair<HANDLE, OVERLAPPED*>> Bind(HANDLE Iocp, ULONG_PTR Key) override;

private:
    HandleWrapper Handle;
    std::function<void(const gsl::span<char>& Buffer)> OnRead;
    wil::unique_event Event{wil::EventOptions::ManualReset};
    OVERLAPPED Overlapped{};
    BufferWrapper Buffer{LX_RELAY_BUFFER_SIZE};
    LARGE_INTEGER Offset{};
};

class SingleAcceptHandle : public OverlappedIOHandle
{
public:
    NON_COPYABLE(SingleAcceptHandle)
    NON_MOVABLE(SingleAcceptHandle)

    SingleAcceptHandle(HandleWrapper&& ListenSocket, HandleWrapper&& AcceptedSocket, std::function<void()>&& OnAccepted);
    ~SingleAcceptHandle();

    void Schedule() override;
    void Collect() override;
    std::vector<std::pair<HANDLE, OVERLAPPED*>> Bind(HANDLE Iocp, ULONG_PTR Key) override;

private:
    HandleWrapper ListenSocket;
    HandleWrapper AcceptedSocket;
    wil::unique_event Event{wil::EventOptions::ManualReset};
    OVERLAPPED Overlapped{};
    std::function<void()> OnAccepted;
    char AcceptBuffer[2 * sizeof(SOCKADDR_STORAGE)];
};

class LineBasedReadHandle : public ReadHandle
{
public:
    NON_COPYABLE(LineBasedReadHandle);
    NON_MOVABLE(LineBasedReadHandle);

    LineBasedReadHandle(HandleWrapper&& Handle, std::function<void(const gsl::span<char>& Buffer)>&& OnLine, bool Crlf);
    ~LineBasedReadHandle();

private:
    void OnRead(const gsl::span<char>& Buffer);

    std::function<void(const gsl::span<char>& Buffer)> OnLine;
    std::string PendingBuffer;
    bool Crlf{};
};

class HTTPChunkBasedReadHandle : public ReadHandle
{
public:
    NON_COPYABLE(HTTPChunkBasedReadHandle);
    NON_MOVABLE(HTTPChunkBasedReadHandle);

    HTTPChunkBasedReadHandle(HandleWrapper&& Handler, std::function<void(const gsl::span<char>& Buffer)>&& OnChunk);
    ~HTTPChunkBasedReadHandle();

    void OnRead(const gsl::span<char>& Line);

private:
    std::function<void(const gsl::span<char>& Buffer)> OnChunk;
    std::string PendingBuffer;
    uint64_t PendingChunkSize = 0;
    bool ExpectHeader = true;
};

class ReadSocketMessageHandle : public OverlappedIOHandle
{
public:
    NON_COPYABLE(ReadSocketMessageHandle);
    NON_MOVABLE(ReadSocketMessageHandle);

    ReadSocketMessageHandle(HandleWrapper&& Socket, std::vector<gsl::byte>& Buffer, std::function<void(const gsl::span<gsl::byte>& Message)>&& OnMessage);
    ~ReadSocketMessageHandle();

    void Schedule() override;
    void Collect() override;
    std::vector<std::pair<HANDLE, OVERLAPPED*>> Bind(HANDLE Iocp, ULONG_PTR Key) override;

private:
    void ScheduleRecv();
    void ProcessRecvResult(DWORD BytesRead);

    HandleWrapper Socket;
    std::vector<gsl::byte>& Buffer;
    std::function<void(const gsl::span<gsl::byte>& Message)> OnMessage;
    wil::unique_event Event{wil::EventOptions::ManualReset};
    OVERLAPPED Overlapped{};
    bool ReadingHeader = true;
    size_t BytesRemaining = sizeof(MESSAGE_HEADER);
    size_t CurrentOffset = 0;
};

class WriteHandle : public OverlappedIOHandle
{
public:
    NON_COPYABLE(WriteHandle);
    NON_MOVABLE(WriteHandle);

    WriteHandle(HandleWrapper&& Handle, const std::vector<char>& Buffer = {});
    WriteHandle(HandleWrapper&& Handle, gsl::span<gsl::byte> Span);
    ~WriteHandle();
    void Schedule() override;
    void Collect() override;
    std::vector<std::pair<HANDLE, OVERLAPPED*>> Bind(HANDLE Iocp, ULONG_PTR Key) override;
    void Push(const gsl::span<char>& Buffer);

private:
    HandleWrapper Handle;
    wil::unique_event Event{wil::EventOptions::ManualReset};
    OVERLAPPED Overlapped{};
    BufferWrapper Buffer;
    LARGE_INTEGER Offset{};
};

template <typename TRead = ReadHandle>
class RelayHandle : public OverlappedIOHandle
{
public:
    NON_COPYABLE(RelayHandle);
    NON_MOVABLE(RelayHandle);

    RelayHandle(HandleWrapper&& Input, HandleWrapper&& Output) :
        Read(std::move(Input), [this](const gsl::span<char>& Buffer) { return OnRead(Buffer); }), Write(std::move(Output))
    {
    }

    void Schedule() override
    {
        WI_ASSERT(State == IOHandleStatus::Standby);

        // If the Buffer is empty, then we're reading.
        if (PendingBuffer.empty())
        {
            // If the output buffer is empty and the reading end is completed, then we're done.
            if (Read.GetState() == IOHandleStatus::Completed)
            {
                State = IOHandleStatus::Completed;
                return;
            }

            Read.Schedule();

            // If the read is pending, update to 'Pending'
            if (Read.GetState() == IOHandleStatus::Pending)
            {
                State = IOHandleStatus::Pending;
            }
        }
        else
        {
            Write.Push(PendingBuffer);
            PendingBuffer.clear();

            Write.Schedule();

            if (Write.GetState() == IOHandleStatus::Pending)
            {
                // The write is pending, update to 'Pending'
                State = IOHandleStatus::Pending;
            }
        }
    }

    void Collect() override
    {
        WI_ASSERT(State == IOHandleStatus::Pending);

        // Transition back to standby
        State = IOHandleStatus::Standby;

        if (Read.GetState() == IOHandleStatus::Pending)
        {
            Read.Collect();
        }
        else
        {
            WI_ASSERT(Write.GetState() == IOHandleStatus::Pending);
            Write.Collect();
        }
    }

    std::vector<std::pair<HANDLE, OVERLAPPED*>> Bind(HANDLE Iocp, ULONG_PTR Key) override
    {
        // Concatenate the children's (handle, overlapped) pairs so the parent
        // MultiHandleWait associates both kernel handles with its IOCP and routes
        // each completion to this entry by OVERLAPPED pointer.
        auto handles = Read.Bind(Iocp, Key);
        auto writeHandles = Write.Bind(Iocp, Key);
        handles.insert(handles.end(), writeHandles.begin(), writeHandles.end());
        return handles;
    }

private:
    void OnRead(const gsl::span<char>& Content)
    {
        PendingBuffer.insert(PendingBuffer.end(), Content.begin(), Content.end());
    }

    TRead Read;
    WriteHandle Write;
    std::vector<char> PendingBuffer;
};

class DockerIORelayHandle : public OverlappedIOHandle
{
public:
    NON_COPYABLE(DockerIORelayHandle);
    NON_MOVABLE(DockerIORelayHandle);

    enum class Format
    {
        Raw,
        HttpChunked
    };

    DockerIORelayHandle(HandleWrapper&& Input, HandleWrapper&& Stdout, HandleWrapper&& Stderr, Format ReadFormat);
    void Schedule() override;
    void Collect() override;
    std::vector<std::pair<HANDLE, OVERLAPPED*>> Bind(HANDLE Iocp, ULONG_PTR Key) override;

#pragma pack(push, 1)
    struct MultiplexedHeader
    {
        uint8_t Fd;
        char Zeroes[3];
        uint32_t Length;
    };
#pragma pack(pop)

    static_assert(sizeof(MultiplexedHeader) == 8);

private:
    void OnRead(const gsl::span<char>& Buffer);
    void ProcessNextHeader();

    std::unique_ptr<OverlappedIOHandle> Read;
    WriteHandle WriteStdout;
    WriteHandle WriteStderr;
    std::vector<char> PendingBuffer;
    WriteHandle* ActiveHandle = nullptr;
    size_t RemainingBytes = 0;
};

class MultiHandleWait
{
public:
    NON_COPYABLE(MultiHandleWait);
    DEFAULT_MOVABLE(MultiHandleWait);

    enum Flags
    {
        None = 0,
        CancelOnCompleted = 1,
        IgnoreErrors = 2,
        NeedNotComplete = 4,
    };

    MultiHandleWait()
    {
        m_iocp.reset(CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0));
        THROW_LAST_ERROR_IF(!m_iocp);
    }

    void AddHandle(std::unique_ptr<OverlappedIOHandle>&& handle, Flags flags = Flags::None);
    bool Run(std::optional<std::chrono::milliseconds> Timeout);
    void Cancel();

private:
    struct HandleEntry
    {
        Flags Options;
        std::unique_ptr<OverlappedIOHandle> Handle;
        std::vector<OVERLAPPED*> Overlappeds;
    };


    wil::unique_handle m_iocp;
    // std::list (rather than std::vector) so iterators held by the schedule loop
    // survive erasures performed by the cleanup pass and processPacket.
    std::list<HandleEntry> m_handles;
    // One IOCPHandle per unique kernel handle this MultiHandleWait operates on.
    // Owns the IOCP association lifetime: when m_iocpBindings is destroyed (before
    // m_handles), each IOCPHandle's destructor detaches its kernel handle so
    // external consumers (e.g. boost::asio::stream::assign, or a later
    // MultiHandleWait reusing the same socket) can rebind freely.
    std::map<HANDLE, IOCPHandle> m_iocpBindings;
    bool m_cancel = false;
};

DEFINE_ENUM_FLAG_OPERATORS(MultiHandleWait::Flags);

} // namespace wsl::windows::common::io
