// Copyright (C) Microsoft Corporation. All rights reserved.
#include "precomp.h"
#include "p9io.h"

#ifndef TEMP_FAILURE_RETRY
#define TEMP_FAILURE_RETRY(expression) \
    (__extension__({ \
        long int __result; \
        do \
            __result = (long int)(expression); \
        while (__result == -1L && errno == EINTR); \
        __result; \
    }))
#endif

namespace p9fs {

EpollWatcher g_Watcher;

CoroutineIoIssuer::CoroutineIoIssuer(int fd) : m_FileDescriptor(fd)
{
}

void CoroutineIoIssuer::Callback(sigval value)
{
    const auto operation = static_cast<CoroutineIoOperation*>(value.sival_ptr);
    auto bytesTransferred = aio_return(&operation->ControlBlock);
    int error = 0;
    if (bytesTransferred < 0)
    {
        error = aio_error(&operation->ControlBlock);
    }

    operation->Result = {error, static_cast<size_t>(bytesTransferred)};
    if (!operation->DoneOrCoroutine.exchange(true))
    {
        return;
    }

    // TODO: Can we use this thread to resume the coroutine?
    g_Scheduler.Schedule(operation->Coroutine);
}

bool CoroutineIoIssuer::PreIssue(CoroutineIoOperation& operation, CancelToken& token)
{
    // Register the IO for cancellation.
    operation.ControlBlock = {};
    operation.ControlBlock.aio_fildes = m_FileDescriptor;
    operation.ControlBlock.aio_sigevent.sigev_notify = SIGEV_THREAD;
    operation.ControlBlock.aio_sigevent.sigev_notify_function = Callback;
    operation.ControlBlock.aio_sigevent.sigev_value.sival_ptr = &operation;
    if (token.Register(operation))
    {
        return true;
    }

    // The operation has already been cancelled. Don't even issue the IO.
    operation.Result = {ECANCELED, 0};
    operation.DoneOrCoroutine = true;
    return false;
}

void CoroutineIoIssuer::IssueFailed(CancelToken& token)
{
    // Unwind the work done in PreIssue.
    token.Unregister();
}

void CoroutineIoIssuer::PostIssue(CoroutineIoOperation& operation, CancelToken& token, IoResult result)
{
    if (result.Error != 0)
    {
        // The IO completed synchronously.
        operation.Result = result;
        operation.DoneOrCoroutine = true;

        WI_ASSERT(operation.Coroutine == nullptr);
    }
    else if (token.Cancelled())
    {
        // The IO did not complete synchronously, but the operation has been
        // cancelled. Depending on when the cancel occurred, the IO may not have
        // been cancelled, so cancel it now.
        aio_cancel(operation.ControlBlock.aio_fildes, &operation.ControlBlock);
    }
}

// Register an epoll operation for either an in or an out event.
// Returns true if the operation must suspend to wait for the event, or false if the operation can
// resume immediately.
// N.B. There can be only one operation registered at a time for each event.
bool EpollDispatcher::Register(int event, CoroutineEpollOperation& operation)
{
    std::scoped_lock<std::mutex> lock{m_lock};
    if (event == EPOLLIN)
    {
        FAIL_FAST_IF(m_inOperation != nullptr);
        if (WI_IsFlagSet(m_currentEvents, EPOLLIN))
        {
            WI_ClearFlag(m_currentEvents, EPOLLIN);

            // Since Register is called before the operation is registered for cancellation,
            // it's not possible for this to return false.
            operation.SetResult(0);
            return false;
        }

        m_inOperation = &operation;
    }
    else
    {
        FAIL_FAST_IF(event != EPOLLOUT);
        FAIL_FAST_IF(m_outOperation != nullptr);
        if (WI_IsFlagSet(m_currentEvents, EPOLLOUT))
        {
            WI_ClearFlag(m_currentEvents, EPOLLOUT);
            operation.SetResult(0);
            return false;
        }

        m_outOperation = &operation;
    }

    return true;
}

// Removes the handler for the specified event (EPOLLIN or EPOLLOUT).
void EpollDispatcher::Remove(int event)
{
    std::scoped_lock<std::mutex> lock{m_lock};
    if (event == EPOLLIN)
    {
        m_inOperation = nullptr;
    }
    else
    {
        FAIL_FAST_IF(event != EPOLLOUT);
        m_outOperation = nullptr;
    }
}

// Notifies the dispatcher an event has occurred.
void EpollDispatcher::Notify(int events)
{
    std::scoped_lock<std::mutex> lock{m_lock};

    // Resume the out operation first, since that is responding to an existing message rather than
    // reading the request for a new one.
    if (WI_IsFlagSet(events, EPOLLOUT))
    {
        if (m_outOperation != nullptr)
        {
            m_outOperation->Resume(0);
            m_outOperation = nullptr;
        }
        else
        {
            // If no operation is registered, remember the event occurred for the next time one
            // is registered.
            WI_SetFlag(m_currentEvents, EPOLLOUT);
        }
    }

    if (WI_IsFlagSet(events, EPOLLIN))
    {
        if (m_inOperation != nullptr)
        {
            m_inOperation->Resume(0);
            m_inOperation = nullptr;
        }
        else
        {
            // If no operation is registered, remember the event occurred for the next time one
            // is registered.
            WI_SetFlag(m_currentEvents, EPOLLIN);
        }
    }
}

void EpollWatcher::Run()
{
    FAIL_FAST_IF(m_EpollFileDescriptor >= 0);

    m_EpollFileDescriptor = epoll_create1(EPOLL_CLOEXEC);
    THROW_LAST_ERROR_IF(m_EpollFileDescriptor < 0);

    std::thread(WatchThread, this).detach();
}

void EpollWatcher::Add(int fd, int events, EpollDispatcher& dispatcher)
{
    epoll_event event{};
    event.events = events;
    event.data.ptr = &dispatcher;
    THROW_LAST_ERROR_IF(epoll_ctl(m_EpollFileDescriptor, EPOLL_CTL_ADD, fd, &event) < 0);
}

void EpollWatcher::Remove(int fd)
{
    THROW_LAST_ERROR_IF(epoll_ctl(m_EpollFileDescriptor, EPOLL_CTL_DEL, fd, nullptr) < 0);
}

void EpollWatcher::WatchThread(EpollWatcher* watcher)
{
    for (;;)
    {
        epoll_event events[10];
        int result = TEMP_FAILURE_RETRY(epoll_wait(watcher->m_EpollFileDescriptor, events, 10, -1));
        THROW_LAST_ERROR_IF(result < 0);

        for (int i = 0; i < result; ++i)
        {
            if (events[i].data.ptr != nullptr)
            {
                const auto dispatcher = static_cast<EpollDispatcher*>(events[i].data.ptr);
                dispatcher->Notify(events[i].events);
            }
        }
    }
}

Task<size_t> RecvAsync(CoroutineEpollIssuer& socket, gsl::span<gsl::byte> buffer, CancelToken& token)
{
    CoroutineEpollOperation operation;
    auto result =
        co_await socket.Issue<ssize_t>(operation, token, EPOLLIN, [&](int fd) { return recv(fd, buffer.data(), buffer.size(), 0); });

    if (result < 0)
    {
        THROW_ERRNO(-result);
    }

    co_return static_cast<size_t>(result);
}

Task<size_t> SendAsync(CoroutineEpollIssuer& socket, gsl::span<const gsl::byte> buffer, CancelToken& token)
{
    CoroutineEpollOperation operation;
    auto result = co_await socket.Issue<ssize_t>(
        operation, token, EPOLLOUT, [&](int fd) { return send(fd, buffer.data(), buffer.size(), 0); });

    if (result < 0)
    {
        THROW_ERRNO(-result);
    }

    co_return static_cast<size_t>(result);
}

Task<int> AcceptAsync(CoroutineEpollIssuer& listen, CancelToken& token)
{
    CoroutineEpollOperation operation;
    auto result = co_await listen.Issue<int>(
        operation, token, EPOLLIN, [&](int fd) { return accept4(fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC); });

    if (result < 0)
    {
        THROW_ERRNO(-result);
    }

    co_return result;
}

Task<IoResult> ReadAsync(CoroutineIoIssuer& file, std::uint64_t offset, gsl::span<gsl::byte> buffer, CancelToken& token)
{
    CoroutineIoOperation operation;
    co_return co_await file.Issue(operation, token, [&](aiocb& cb) -> IoResult {
        cb.aio_buf = buffer.data();
        cb.aio_nbytes = buffer.size();
        cb.aio_offset = offset;
        if (aio_read(&cb) < 0)
        {
            return {-errno, 0};
        }

        return {};
    });
}

Task<IoResult> WriteAsync(CoroutineIoIssuer& file, std::uint64_t offset, gsl::span<const gsl::byte> buffer, CancelToken& token)
{
    CoroutineIoOperation operation;
    co_return co_await file.Issue(operation, token, [&](aiocb& cb) -> IoResult {
        cb.aio_buf = (volatile void*)buffer.data();
        cb.aio_nbytes = buffer.size();
        cb.aio_offset = offset;
        if (aio_write(&cb) < 0)
        {
            return {errno, 0};
        }

        return {};
    });
}

} // namespace p9fs
