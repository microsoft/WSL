// Copyright (C) Microsoft Corporation. All rights reserved.
#pragma once

#include "p9await.h"
#include "p9tracelogging.h"

namespace p9fs {

struct IoResult
{
    int Error;
    size_t BytesTransferred;
};

struct CoroutineIoOperation final : public ICancellable
{
    aiocb ControlBlock;
    IoResult Result;
    std::coroutine_handle<> Coroutine{};
    std::atomic<bool> DoneOrCoroutine{false};

    void Cancel() override
    {
        aio_cancel(ControlBlock.aio_fildes, &ControlBlock);
    }
};

struct CoroutineEpollOperation final : public ICancellable
{
    std::atomic<int> Result{EWOULDBLOCK};
    std::coroutine_handle<> Coroutine;

    void Resume(int result)
    {
        if (SetResult(result))
        {
            g_Scheduler.Schedule(Coroutine);
        }
    }

    bool SetResult(int result)
    {
        int expected = EWOULDBLOCK;
        return Result.compare_exchange_strong(expected, result);
    }

    void Cancel() override
    {
        Resume(ECANCELED);
    }
};

struct CoroutineIoIssuer
{
private:
    struct Awaiter
    {
        CoroutineIoOperation& m_Operation;
        CancelToken& m_Token;

        bool await_ready() const
        {
            return m_Operation.DoneOrCoroutine;
        }

        bool await_suspend(std::coroutine_handle<> handle)
        {
            WI_ASSERT(m_Operation.Coroutine == nullptr);

            m_Operation.Coroutine = handle;
            if (m_Operation.DoneOrCoroutine.exchange(true))
            {
                return false;
            }

            return true;
        }

        IoResult await_resume() const
        {
            m_Token.Unregister();
            return m_Operation.Result;
        }
    };

public:
    CoroutineIoIssuer() = default;
    CoroutineIoIssuer(int fd);

    explicit operator bool() const
    {
        return m_FileDescriptor >= 0;
    }

    template <class T>
    Awaiter Issue(CoroutineIoOperation& operation, CancelToken& token, T&& func)
    {
        if (PreIssue(operation, token))
        {
            IoResult result;
            try
            {
                result = func(operation.ControlBlock);
            }
            catch (...)
            {
                IssueFailed(token);
                throw;
            }

            PostIssue(operation, token, result);
        }

        return Awaiter{operation, token};
    }

private:
    static void Callback(sigval value);

    bool PreIssue(CoroutineIoOperation& operation, CancelToken& token);
    static void IssueFailed(CancelToken& token);
    void PostIssue(CoroutineIoOperation& operation, CancelToken& token, IoResult result);

    int m_FileDescriptor{-1};
};

// Class that handles suspending and resuming operations based on EPOLLIN and EPOLLOUT events.
class EpollDispatcher
{
public:
    bool Register(int event, CoroutineEpollOperation& operation);
    void Remove(int event);
    void Notify(int events);

private:
    std::mutex m_lock;
    int m_currentEvents{};
    CoroutineEpollOperation* m_outOperation{};
    CoroutineEpollOperation* m_inOperation{};
};

class EpollWatcher
{
public:
    void Run();
    void Add(int fd, int events, EpollDispatcher& dispatcher);
    void Remove(int fd);

    explicit operator bool() const noexcept
    {
        return m_EpollFileDescriptor >= 0;
    }

private:
    static void WatchThread(EpollWatcher* watcher);

    int m_EpollFileDescriptor{-1};
};

extern EpollWatcher g_Watcher;

class CoroutineEpollIssuer
{
private:
    struct Awaiter
    {
        EpollDispatcher& Dispatcher;
        int Fd;
        CoroutineEpollOperation& Operation;
        int Events;
        CancelToken& Token;

        static constexpr bool await_ready() noexcept
        {
            return false;
        }

        bool await_suspend(std::coroutine_handle<> handle)
        {
            Operation.Result = EWOULDBLOCK;
            Operation.Coroutine = handle;

            // Check if the operation needs to be suspended.
            if (!Dispatcher.Register(Events, Operation))
            {
                return false;
            }

            // Register the operation for cancellation only if it actually needs to suspend.
            if (!Token.Register(Operation))
            {
                // If the operation is already cancelled, attempt to mark it cancelled. The function
                // must return true if that fails, because it means the dispatcher already scheduled
                // it for resumption.
                return !Operation.SetResult(ECANCELED);
            }

            return true;
        }

        int await_resume() const
        {
            // If the function was resumed by cancellation, make sure the dispatcher can't access
            // it after it goes out of scope.
            // N.B. If the operation was not registered with the dispatcher, calling remove is a
            //      no-op.
            if (Operation.Result == ECANCELED)
            {
                Dispatcher.Remove(Events);
            }

            // Unregister from the cancel token.
            // N.B. If the operation was not registered with the token, calling remove is a no-op.
            Token.Unregister();
            return Operation.Result;
        }
    };

public:
    CoroutineEpollIssuer(EpollWatcher& watcher) : m_Watcher{watcher}
    {
    }

    ~CoroutineEpollIssuer()
    {
        Reset();
    }

    explicit operator bool() const
    {
        return m_FileDescriptor >= 0;
    }

    template <typename TResult, typename T>
    Task<TResult> Issue(CoroutineEpollOperation& operation, CancelToken& token, int events, T&& func)
    {
        for (;;)
        {
            TResult result = func(m_FileDescriptor);
            if (result >= 0)
            {
                co_return result;
            }

            if (errno != EWOULDBLOCK)
            {
                co_return -errno;
            }

            // Wait for the epoll notification before retrying.
            int waitResult = co_await Awaiter{m_Dispatcher, m_FileDescriptor, operation, events, token};
            if (waitResult != 0)
            {
                co_return -waitResult;
            }
        }
    }

    void Reset(int fd = -1)
    {
        // If there is an existing file descriptor, remove it from the epoll.
        if (m_FileDescriptor >= 0)
        {
            m_Watcher.Remove(m_FileDescriptor);
        }

        m_FileDescriptor = fd;

        // Add the new file descriptor to epoll.
        if (m_FileDescriptor >= 0)
        {
            m_Watcher.Add(m_FileDescriptor, EPOLLIN | EPOLLOUT | EPOLLET, m_Dispatcher);
        }
    }

private:
    EpollWatcher& m_Watcher;
    EpollDispatcher m_Dispatcher;
    int m_FileDescriptor{-1};
};

Task<int> AcceptAsync(CoroutineEpollIssuer& listen, CancelToken& token);
Task<size_t> RecvAsync(CoroutineEpollIssuer& socket, gsl::span<gsl::byte> buffer, CancelToken& token);
Task<size_t> SendAsync(CoroutineEpollIssuer& socket, gsl::span<const gsl::byte> buffer, CancelToken& token);
Task<IoResult> ReadAsync(CoroutineIoIssuer& file, std::uint64_t offset, gsl::span<gsl::byte> buffer, CancelToken& token);
Task<IoResult> WriteAsync(CoroutineIoIssuer& file, std::uint64_t offset, gsl::span<const gsl::byte> buffer, CancelToken& token);

} // namespace p9fs
