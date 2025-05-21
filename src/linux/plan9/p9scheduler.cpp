// Copyright (C) Microsoft Corporation. All rights reserved.
#include "precomp.h"
#include "p9platform.h"
#include "p9scheduler.h"

namespace p9fs {

Scheduler g_Scheduler;
thread_local bool Scheduler::tls_Blocked{};
thread_local bool Scheduler::tls_SchedulerThread{};

Scheduler::Scheduler() : m_Work{CreateWorkItem(std::bind(&Scheduler::WorkerCallback, this))}
{
}

/// Schedules a coroutine to run. It will run sometime after this coroutine
/// yields or enters a blocking region.
void Scheduler::Schedule(Coroutine coroutine) noexcept
{
    bool kick = false;

    {
        std::unique_lock<std::shared_mutex> lock(m_Lock);

        // N.B. This could throw in very low memory situations, which would terminate the process.
        m_Queue.push(coroutine);
        if (!m_Running && !m_ThreadEnqueued)
        {
            m_ThreadEnqueued = true;
            kick = true;
        }
    }

    if (kick)
    {
        m_Work->Submit();
    }
}

/// Donates the current thread to run coroutines and schedules the specified
/// coroutine to run.
void Scheduler::DonateThreadAndResume(Coroutine coroutine) noexcept
{
    const bool run = Claim(false);
    Schedule(coroutine);
    if (run)
    {
        RunAndRelease();
    }
}

/// Runs coroutines until there are no more in the queue or until this thread
/// gave up the queue in order to run blocking code.
///
/// Must be called on the thread that called Claim().
void Scheduler::RunAndRelease() noexcept
{
    WI_ASSERT(!tls_Blocked);

    tls_SchedulerThread = true;
    std::unique_lock<std::shared_mutex> lock(m_Lock);
    while (!m_Queue.empty())
    {
        auto coroutine = m_Queue.front();
        m_Queue.pop();
        lock.unlock();
        coroutine.resume();
        if (tls_Blocked)
        {
            tls_Blocked = false;
            tls_SchedulerThread = false;
            return;
        }

        lock.lock();
    }

    WI_ASSERT(m_Queue.empty() || m_Running || m_ThreadEnqueued);

    m_Running = false;
    tls_SchedulerThread = false;
}

/// Called when the current thread may block for some time. Gives up queue
/// ownership, potentially scheduling another thread to resume running
/// non-blocking code.
bool Scheduler::Block() noexcept
{
    if (!tls_SchedulerThread)
    {
        return false;
    }

    WI_ASSERT(!tls_Blocked);

    tls_Blocked = true;

    bool kick = false;

    {
        std::unique_lock<std::shared_mutex> lock(m_Lock);

        WI_ASSERT(m_Running);

        m_Running = false;
        if (!m_Queue.empty() && !m_ThreadEnqueued)
        {
            m_ThreadEnqueued = true;
            kick = true;
        }
    }

    if (kick)
    {
        m_Work->Submit();
    }

    return true;
}

/// Awaitable function called when the current thread is done running blocking
/// code. Tries to reclaim ownership of the queue and resumes the current
/// coroutine.
Scheduler::Unblocker Scheduler::Unblock() noexcept
{
    WI_ASSERT(tls_Blocked);

    // Try to reuse this thread to run async tasks.
    const bool run = Claim(false);
    if (run)
    {
        tls_Blocked = false;
    }

    // Unblocker will either resume the current coroutine or schedule it to run
    // on the new queue owner.
    return Unblocker{*this, run};
}

/// Try to claim queue ownership for the current thread. If this function
/// returns true, then the caller must call RunAndRelease to process the queue.
///
/// If fromKick, then the caller is the thread that was explicitly kicked to
/// process the queue. Otherwise, this is an IO completion or other
/// opportunistic thread.
bool Scheduler::Claim(bool fromKick) noexcept
{
    std::unique_lock<std::shared_mutex> lock(m_Lock);

    WI_ASSERT(!fromKick || m_ThreadEnqueued);

    if (fromKick)
    {
        m_ThreadEnqueued = false;
    }

    if (m_Running)
    {
        return false;
    }

    m_Running = true;
    return true;
}

/// Threadpool callback called to process the queue.
void Scheduler::WorkerCallback() noexcept
{
    if (Claim(true))
    {
        RunAndRelease();
    }
}

} // namespace p9fs
