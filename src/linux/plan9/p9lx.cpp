// Copyright (C) Microsoft Corporation. All rights reserved.
#include "precomp.h"
#include "p9lx.h"
using namespace std::chrono_literals;

namespace p9fs {

constexpr auto ThreadPoolTimeout = 10s;

ThreadPool g_ThreadPool;

// Create a socket class with a socket fd.
Socket::Socket(int socket) : m_Io{g_Watcher}
{
    Reset(socket);
}

// Asynchronously wait for new connections.
Task<std::unique_ptr<ISocket>> Socket::AcceptAsync(CancelToken& token)
{
    int socket = co_await p9fs::AcceptAsync(m_Io, token);
    co_return std::make_unique<Socket>(socket);
}

// Asynchronously receive data.
Task<size_t> Socket::RecvAsync(gsl::span<gsl::byte> buffer, CancelToken& token)
{
    return p9fs::RecvAsync(m_Io, buffer, token);
}

// Asynchronously send data.
Task<size_t> Socket::SendAsync(gsl::span<const gsl::byte> buffer, CancelToken& token)
{
    size_t totalSent{};
    do
    {
        totalSent += co_await p9fs::SendAsync(m_Io, buffer.subspan(totalSent), token);
    } while (totalSent < buffer.size());

    co_return totalSent;
}

void Socket::Reset(int socket)
{
    m_Io.Reset(socket);
    m_Socket.reset(socket);
}

// Create a new work item for a specific callback.
WorkItem::WorkItem(std::function<void()> callback) : m_Callback{callback}
{
}

// Submit the work item to the thread pool.
void WorkItem::Submit()
{
    g_ThreadPool.SubmitWork(m_Callback);
}

// Create a new work item for a specific callback.
std::unique_ptr<IWorkItem> CreateWorkItem(std::function<void()> callback)
{
    return std::make_unique<WorkItem>(callback);
}

// Create a new thread pool.
ThreadPool::ThreadPool() : m_MaxThreads{std::thread::hardware_concurrency()}
{
}

// Submit work to the thread pool.
void ThreadPool::SubmitWork(std::function<void()> callback)
{
    std::lock_guard<std::mutex> lock{m_Lock};
    m_WorkQueue.push(callback);
    // If there are no threads to run the work right now, and it's not at the
    // max, start a new thread.
    if (m_AvailableThreads == 0 && m_RunningThreads < m_MaxThreads)
    {
        ++m_RunningThreads;
        std::thread(&ThreadPool::WorkerCallback, this).detach();
    }
    else
    {
        m_Condition.notify_one();
    }
}

// Runs a worker thread that executes queued work items.
void ThreadPool::WorkerCallback()
{
    std::unique_lock<std::mutex> lock{m_Lock, std::defer_lock};
    for (;;)
    {
        lock.lock();
        ++m_AvailableThreads;
        // Wait for work.
        if (m_WorkQueue.empty() && !m_Condition.wait_for(lock, ThreadPoolTimeout, [this]() { return !m_WorkQueue.empty(); }))
        {
            // The wait timed out, so shut down this thread.
            --m_AvailableThreads;
            --m_RunningThreads;
            return;
        }

        // Take ownership of the work item.
        --m_AvailableThreads;
        auto work = m_WorkQueue.front();
        m_WorkQueue.pop();
        lock.unlock();

        // Run the work outside the lock.
        work();
    }
}

} // namespace p9fs
