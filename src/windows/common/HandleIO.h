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

    EventHandle(HandleWrapper&& Handle, std::function<void()>&& OnSignalled = []() {});
    void Schedule() override;
    void Collect() override;
    HANDLE GetHandle() const override;

private:
    HandleWrapper Handle;
    std::function<void()> OnSignalled;
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
    HANDLE GetHandle() const override;

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
    HANDLE GetHandle() const override;

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
    HANDLE GetHandle() const override;

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
    HANDLE GetHandle() const override;
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

    HANDLE GetHandle() const override
    {
        if (Read.GetState() == IOHandleStatus::Pending)
        {
            return Read.GetHandle();
        }
        else
        {
            WI_ASSERT(Write.GetState() == IOHandleStatus::Pending);
            return Write.GetHandle();
        }
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
    HANDLE GetHandle() const override;

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

namespace details {
    inline void UnregisterRegisteredWait(HANDLE waitHandle) noexcept
    {
        // INVALID_HANDLE_VALUE makes UnregisterWaitEx block until any in-flight wait callback
        // returns, so resources captured by the callback (Entry, SharedState) can be safely
        // freed once the unique_any goes out of scope.
        LOG_LAST_ERROR_IF(!UnregisterWaitEx(waitHandle, INVALID_HANDLE_VALUE));
    }
} // namespace details

using unique_registered_wait = wil::unique_any_handle_null<decltype(&details::UnregisterRegisteredWait), &details::UnregisterRegisteredWait>;

// MultiHandleWait runs a set of OverlappedIOHandle to completion using the system thread
// pool's wait infrastructure (RegisterWaitForSingleObject) so it can wait for more than
// MAXIMUM_WAIT_OBJECTS (64) handles at once.
//
// Threading model: the wait callback runs on a thread pool thread and only enqueues the
// signaled Entry into a queue, then sets a notification event. Run() owns the actual work:
// it drains the queue and calls Collect() on each signaled handle from the Run() thread,
// preserving the original guarantee that Schedule() and Collect() execute on the caller's
// thread. To simplify lifetime management, Run() unregisters every wait at the top of each
// iteration before processing state, then re-registers a fresh wait for every Pending
// handle. The first handle to signal wakes Run(), which then processes whatever the
// callbacks queued before re-arming.
class MultiHandleWait
{
public:
    NON_COPYABLE(MultiHandleWait);

    enum Flags
    {
        None = 0,
        CancelOnCompleted = 1,
        IgnoreErrors = 2,
        NeedNotComplete = 4,
    };

    MultiHandleWait();
    ~MultiHandleWait();
    MultiHandleWait(MultiHandleWait&&) noexcept = default;
    MultiHandleWait& operator=(MultiHandleWait&&) noexcept;

    void AddHandle(std::unique_ptr<OverlappedIOHandle>&& handle, Flags flags = Flags::None);
    bool Run(std::optional<std::chrono::milliseconds> Timeout);
    void Cancel();

private:
    struct Entry;

    // SharedState is heap-allocated so the address captured by wait callbacks remains
    // stable when the owning MultiHandleWait is moved.
    struct SharedState
    {
        wil::srwlock Lock;
        wil::unique_event Notification{wil::EventOptions::None};
        _Guarded_by_(Lock) std::vector<Entry*> Signaled;
    };

    // Entry is the callback context passed to RegisterWaitForSingleObject. Each Entry is
    // heap-allocated (unique_ptr) so its address - and the OverlappedIOHandle pointer
    // embedded in it - remains stable across m_handles reallocations.
    struct Entry
    {
        Flags HandleFlags{};
        std::unique_ptr<OverlappedIOHandle> Handle;
        unique_registered_wait WaitRegistration;
        SharedState* State{};
    };

    static void NTAPI WaitCallback(PVOID Context, BOOLEAN TimerOrWaitFired);
    void RegisterWait(Entry& entry);
    void UnregisterAllWaits() noexcept;

    std::vector<std::unique_ptr<Entry>> m_handles;
    std::unique_ptr<SharedState> m_state;
    bool m_cancel = false;
};

DEFINE_ENUM_FLAG_OPERATORS(MultiHandleWait::Flags);

} // namespace wsl::windows::common::io
