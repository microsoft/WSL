// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#define LX_RELAY_BUFFER_SIZE 0x1000

namespace wsl::windows::common::io {

enum class IOHandleStatus
{
    Created,
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
// When two leaves of the same MultiHandleWait wrap the same kernel handle
// (e.g. RelayHandle::Write and a separate response ReadHandle both targeting one
// docker socket), the second leaf's Bind detaches the first leaf's binding and
// installs its own. To avoid the first leaf's destructor tearing the active
// (second leaf's) binding back out, IOCPHandle records the latest binder for
// each kernel handle in a process-wide registry; the destructor only detaches
// when it finds itself still recorded as the current binder.
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

    // Bind this handle's underlying file/socket to an I/O completion port. Each call
    // returns the list of completion keys that were associated with the IOCP - one per
    // file/socket. Completion keys must be unique across the IOCP so packets can be
    // routed unambiguously back to the originating handle, so each leaf implementation
    // picks its own key (typically the address of the leaf object). Composite handles
    // forward to every sub-handle and concatenate the returned key lists.
    //
    // Subclasses that wrap an event (which cannot be associated with an IOCP) instead
    // arrange for a thread pool wait to PostQueuedCompletionStatus when the event signals,
    // using the chosen key.
    //
    // Bind must only be called when the handle is in the Created state. Implementations
    // transition the state to Standby on success.
    virtual std::vector<ULONG_PTR> Bind(HANDLE Iocp) = 0;

    // Returns true if this handle (or any of its sub-handles) issued the I/O described by
    // the given OVERLAPPED pointer. MultiHandleWait::Run uses this to route completion
    // packets back to the originating leaf because the kernel-level completion key cannot
    // distinguish between two leaves that share the same kernel handle (e.g. a request
    // socket used by both a RelayHandle::Write and a separate response ReadHandle): only
    // one Bind survives in the kernel, so all completions arrive with the latest key.
    // Each leaf's I/O carries its own OVERLAPPED, so the pointer is always unambiguous.
    virtual bool OwnsOverlapped(OVERLAPPED* Overlapped) const = 0;

    IOHandleStatus GetState() const;

protected:
    IOHandleStatus State = IOHandleStatus::Created;
};

class EventHandle : public OverlappedIOHandle
{
public:
    NON_COPYABLE(EventHandle)
    NON_MOVABLE(EventHandle)

    EventHandle(HandleWrapper&& Handle, std::function<void()>&& OnSignalled = []() {});
    void Schedule() override;
    void Collect() override;
    std::vector<ULONG_PTR> Bind(HANDLE Iocp) override;
    bool OwnsOverlapped(OVERLAPPED* Overlapped) const override;

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
    std::vector<ULONG_PTR> Bind(HANDLE Iocp) override;
    bool OwnsOverlapped(OVERLAPPED* OverlappedPtr) const override;

private:
    HandleWrapper Handle;
    std::function<void(const gsl::span<char>& Buffer)> OnRead;
    wil::unique_event Event{wil::EventOptions::ManualReset};
    OVERLAPPED Overlapped{};
    BufferWrapper Buffer{LX_RELAY_BUFFER_SIZE};
    LARGE_INTEGER Offset{};
    IOCPHandle m_iocpBinding;
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
    std::vector<ULONG_PTR> Bind(HANDLE Iocp) override;
    bool OwnsOverlapped(OVERLAPPED* OverlappedPtr) const override;

private:
    HandleWrapper ListenSocket;
    HandleWrapper AcceptedSocket;
    wil::unique_event Event{wil::EventOptions::ManualReset};
    OVERLAPPED Overlapped{};
    std::function<void()> OnAccepted;
    char AcceptBuffer[2 * sizeof(SOCKADDR_STORAGE)];
    IOCPHandle m_iocpBinding;
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
    std::vector<ULONG_PTR> Bind(HANDLE Iocp) override;
    bool OwnsOverlapped(OVERLAPPED* OverlappedPtr) const override;

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
    IOCPHandle m_iocpBinding;
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
    std::vector<ULONG_PTR> Bind(HANDLE Iocp) override;
    bool OwnsOverlapped(OVERLAPPED* OverlappedPtr) const override;
    void Push(const gsl::span<char>& Buffer);

private:
    HandleWrapper Handle;
    wil::unique_event Event{wil::EventOptions::ManualReset};
    OVERLAPPED Overlapped{};
    BufferWrapper Buffer;
    LARGE_INTEGER Offset{};
    IOCPHandle m_iocpBinding;
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

    std::vector<ULONG_PTR> Bind(HANDLE Iocp) override
    {
        WI_ASSERT(State == IOHandleStatus::Created);

        // Each sub-handle picks its own unique completion key. Concatenate the lists so
        // the parent MultiHandleWait can route either completion back to this RelayHandle.
        auto keys = Read.Bind(Iocp);
        auto writeKeys = Write.Bind(Iocp);
        keys.insert(keys.end(), writeKeys.begin(), writeKeys.end());
        State = IOHandleStatus::Standby;
        return keys;
    }

    bool OwnsOverlapped(OVERLAPPED* OverlappedPtr) const override
    {
        return Read.OwnsOverlapped(OverlappedPtr) || Write.OwnsOverlapped(OverlappedPtr);
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
    std::vector<ULONG_PTR> Bind(HANDLE Iocp) override;
    bool OwnsOverlapped(OVERLAPPED* OverlappedPtr) const override;

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

    MultiHandleWait() = default;

    void AddHandle(std::unique_ptr<OverlappedIOHandle>&& handle, Flags flags = Flags::None);
    bool Run(std::optional<std::chrono::milliseconds> Timeout);
    void Cancel();

private:
    struct HandleEntry
    {
        Flags Options;
        std::unique_ptr<OverlappedIOHandle> Handle;
        std::vector<ULONG_PTR> Keys;
    };

    // m_iocp is declared first so it is destroyed last - after every handle has had a
    // chance to cancel its pending I/O. Closing a handle while its file/socket still has
    // overlapped I/O outstanding could otherwise leave packets queued to a destroyed
    // completion port.
    wil::unique_handle m_iocp;
    // std::list (rather than std::vector) so iterators held by the schedule loop survive
    // erasures performed by the cleanup pass and processPacket.
    std::list<HandleEntry> m_handles;
    bool m_cancel = false;
};

DEFINE_ENUM_FLAG_OPERATORS(MultiHandleWait::Flags);

} // namespace wsl::windows::common::io
