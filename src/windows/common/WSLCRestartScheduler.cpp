/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCRestartScheduler.cpp

Abstract:

    Contains the implementation for WSLCRestartScheduler.

--*/

#include "precomp.h"
#include "WSLCRestartScheduler.h"

using wsl::windows::service::wslc::WSLCRestartScheduler;

WSLCRestartScheduler::~WSLCRestartScheduler()
{
    Stop();
}

void WSLCRestartScheduler::Schedule(std::chrono::milliseconds Delay, std::function<void()>&& Callback)
{
    std::lock_guard lock{m_lock};
    if (m_stopped)
    {
        return;
    }

    m_queue.emplace(std::chrono::steady_clock::now() + Delay, std::move(Callback));

    if (!m_thread.joinable())
    {
        m_thread = std::thread{&WSLCRestartScheduler::Run, this};
    }

    m_wake.notify_one();
}

void WSLCRestartScheduler::Stop() noexcept
{
    std::thread thread;
    decltype(m_queue) discarded;

    {
        std::lock_guard lock{m_lock};

        // Callback-initiated shutdown or owner destruction cannot join this worker; continuing would permit unsafe teardown.
        FAIL_FAST_IF(m_thread.get_id() == std::this_thread::get_id());
        m_stopped = true;
        discarded = std::exchange(m_queue, {});

        thread = std::move(m_thread);
    }

    m_wake.notify_all();

    if (thread.joinable())
    {
        thread.join();
    }
}

void WSLCRestartScheduler::Run() noexcept
{
    wsl::windows::common::wslutil::SetThreadDescription(L"WSLCRestartScheduler");
    const auto coInit = wil::CoInitializeEx(COINIT_MULTITHREADED);

    std::unique_lock lock{m_lock};
    while (!m_stopped)
    {
        if (m_queue.empty())
        {
            m_wake.wait(lock);
            continue;
        }

        const auto due = m_queue.begin()->first;
        if (std::chrono::steady_clock::now() < due)
        {
            m_wake.wait_until(lock, due);
            continue;
        }

        auto callback = std::move(m_queue.begin()->second);
        m_queue.erase(m_queue.begin());

        lock.unlock();

        try
        {
            callback();
        }
        CATCH_LOG();

        // Destroy the callback's captures before re-taking the lock.
        callback = nullptr;
        lock.lock();
    }
}
