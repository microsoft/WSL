// Copyright (C) Microsoft Corporation. All rights reserved.
#pragma once

namespace p9fs {

class IWorkItem;

class Scheduler
{
public:
    using Coroutine = std::coroutine_handle<>;

    struct Unblocker
    {
        Scheduler& m_Scheduler;
        bool m_Run{};

        bool await_ready() const
        {
            return m_Run;
        }

        void await_suspend(Coroutine handle)
        {
            m_Scheduler.Schedule(handle);
        }

        static void await_resume()
        {
        }
    };

    Scheduler();
    void Schedule(Coroutine coroutine) noexcept;
    void DonateThreadAndResume(Coroutine coroutine) noexcept;
    bool Block() noexcept;
    struct Unblocker Unblock() noexcept;

private:
    void RunAndRelease() noexcept;
    bool Claim(bool owner) noexcept;
    void WorkerCallback() noexcept;

    std::shared_mutex m_Lock;
    std::queue<Coroutine> m_Queue;
    std::unique_ptr<IWorkItem> m_Work;
    bool m_Running;
    bool m_ThreadEnqueued;
    static thread_local bool tls_Blocked;
    static thread_local bool tls_SchedulerThread;
};

extern Scheduler g_Scheduler;

} // namespace p9fs
