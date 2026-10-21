// Copyright (C) Microsoft Corporation. All rights reserved.

/*++

Module Name:

    ThreadPool.h

Abstract:

    Provides ThreadPool, a private Windows thread pool that runs submitted work
    concurrently, and WorkItem, a handle used to wait for, cancel, and retrieve
    the result of submitted work.

--*/
#pragma once

#include <deque>
#include <exception>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>
#include <windows.h>
#include <wil/resource.h>
#include <wil/result_macros.h>

namespace wsl::windows::common {

class ThreadPool;

namespace details {

    class WorkItemBase
    {
    public:
        NON_COPYABLE(WorkItemBase);
        NON_MOVABLE(WorkItemBase);

        virtual ~WorkItemBase() noexcept = default;

        // Waits until the work completes or is canceled. Returns false if the timeout elapses first.
        bool Wait(DWORD timeoutMs = INFINITE) const noexcept
        {
            return m_completed.wait(timeoutMs);
        }

        // Cancels the work if it has not started. Work that is already running is not interrupted.
        // Returns true if the work was canceled; its error is then HRESULT_FROM_WIN32(ERROR_CANCELLED).
        bool Cancel() noexcept
        {
            {
                const auto lock = m_lock.lock_exclusive();
                if (m_state != State::Pending)
                {
                    return false;
                }

                m_state = State::Canceled;
                m_error = std::make_exception_ptr(wil::ResultException(HRESULT_FROM_WIN32(ERROR_CANCELLED)));
            }

            m_completed.SetEvent();
            return true;
        }

        // Waits for completion, then returns the exception thrown by the work, or null if it succeeded.
        const std::exception_ptr& Error() const noexcept
        {
            Wait();
            return m_error;
        }

    protected:
        WorkItemBase() = default;

    private:
        friend class wsl::windows::common::ThreadPool;

        enum class State
        {
            Pending,
            Running,
            Completed,
            Canceled
        };

        virtual void Execute() = 0;

        // Called by the pool while it holds its own lock, so that the item leaves the pending
        // queue and becomes Running atomically with respect to pool shutdown.
        bool TryStart() noexcept
        {
            const auto lock = m_lock.lock_exclusive();
            if (m_state != State::Pending)
            {
                return false;
            }

            m_state = State::Running;
            return true;
        }

        // Runs an item that TryStart() has moved to Running.
        void Run() noexcept
        {
            std::exception_ptr error;
            try
            {
                Execute();
            }
            catch (...)
            {
                error = std::current_exception();
            }

            {
                const auto lock = m_lock.lock_exclusive();
                m_error = std::move(error);
                m_state = State::Completed;
            }

            m_completed.SetEvent();
        }

        wil::srwlock m_lock;
        _Guarded_by_(m_lock) State m_state { State::Pending };
        std::exception_ptr m_error;
        wil::unique_event m_completed{wil::EventOptions::ManualReset};
    };

} // namespace details

// Handle to work submitted to a ThreadPool. Error() and Result() wait for the work to complete.
template <typename TResult>
class WorkItem : public details::WorkItemBase
{
    static_assert(!std::is_reference_v<TResult>, "WorkItem results must be returned by value.");

public:
    // Waits for completion, then rethrows the exception thrown by the work or returns its result.
    decltype(auto) Result()
    {
        Wait();
        if (const auto& error = Error())
        {
            std::rethrow_exception(error);
        }

        if constexpr (!std::is_void_v<TResult>)
        {
            return (*m_result);
        }
    }

protected:
    WorkItem() = default;

private:
    virtual TResult Invoke() = 0;

    void Execute() final
    {
        if constexpr (std::is_void_v<TResult>)
        {
            Invoke();
        }
        else
        {
            m_result.emplace(Invoke());
        }
    }

    std::optional<std::conditional_t<std::is_void_v<TResult>, std::monostate, TResult>> m_result;
};

namespace details {

    template <typename TResult, typename TFunction>
    class WorkItemImpl final : public WorkItem<TResult>
    {
    public:
        template <typename TArg>
        explicit WorkItemImpl(TArg&& function) : m_function(std::forward<TArg>(function))
        {
        }

    private:
        TResult Invoke() override
        {
            return m_function();
        }

        TFunction m_function;
    };

} // namespace details

// Runs submitted work on a private Windows thread pool bounded to maxThreads concurrent threads.
//
// Destroying the ThreadPool cancels work that has not started and waits for running work to
// return, so no submitted work runs after the destructor completes. Running work is never
// abandoned or interrupted; work that needs to stop early must observe its own cancellation
// signal. The ThreadPool must not be destroyed from one of its own work items.
//
// Thread pool threads have no per-thread initialization. Work that requires per-thread setup,
// such as CoInitializeEx, must perform it itself.
class ThreadPool
{
public:
    NON_COPYABLE(ThreadPool);
    NON_MOVABLE(ThreadPool);

    ThreadPool(DWORD minThreads, DWORD maxThreads)
    {
        THROW_HR_IF(E_INVALIDARG, maxThreads == 0 || minThreads > maxThreads);

        InitializeThreadpoolEnvironment(&m_environment);

        m_pool.reset(CreateThreadpool(nullptr));
        THROW_LAST_ERROR_IF_NULL(m_pool.get());

        SetThreadpoolThreadMaximum(m_pool.get(), maxThreads);
        THROW_IF_WIN32_BOOL_FALSE(SetThreadpoolThreadMinimum(m_pool.get(), minThreads));
        SetThreadpoolCallbackPool(&m_environment, m_pool.get());

        m_work.reset(CreateThreadpoolWork(WorkCallback, this, &m_environment));
        THROW_LAST_ERROR_IF_NULL(m_work.get());
    }

    ~ThreadPool() noexcept
    {
        std::deque<std::shared_ptr<details::WorkItemBase>> pending;
        {
            const auto lock = m_lock.lock_exclusive();
            m_shuttingDown = true;
            pending.swap(m_pending);
        }

        for (const auto& item : pending)
        {
            item->Cancel();
        }

        // Cancels callbacks that have not started, and waits for running callbacks to return.
        m_work.reset();
    }

    // Queues work to run on the pool. Throws HRESULT_FROM_WIN32(ERROR_CANCELLED) if the pool is being destroyed.
    template <typename TFunction>
    auto SubmitWork(TFunction&& work)
    {
        using TCallable = std::decay_t<TFunction>;
        using TResult = std::invoke_result_t<TCallable&>;

        auto item = std::make_shared<details::WorkItemImpl<TResult, TCallable>>(std::forward<TFunction>(work));
        {
            const auto lock = m_lock.lock_exclusive();
            THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_CANCELLED), m_shuttingDown);
            m_pending.emplace_back(item);

            // Each submission queues exactly one callback, and each callback runs exactly one pending item.
            SubmitThreadpoolWork(m_work.get());
        }

        return std::shared_ptr<WorkItem<TResult>>(std::move(item));
    }

private:
    using unique_tp_env = wil::unique_struct<TP_CALLBACK_ENVIRON, decltype(&DestroyThreadpoolEnvironment), DestroyThreadpoolEnvironment>;
    using unique_tp_pool = wil::unique_any<PTP_POOL, decltype(&CloseThreadpool), CloseThreadpool>;

    static void CALLBACK WorkCallback(PTP_CALLBACK_INSTANCE, void* context, PTP_WORK) noexcept
    {
        auto* self = static_cast<ThreadPool*>(context);

        std::shared_ptr<details::WorkItemBase> item;
        {
            const auto lock = self->m_lock.lock_exclusive();
            if (self->m_pending.empty())
            {
                return;
            }

            item = std::move(self->m_pending.front());
            self->m_pending.pop_front();

            // An item canceled through its WorkItem handle still consumes the callback queued for it.
            if (!item->TryStart())
            {
                return;
            }
        }

        item->Run();
    }

    // Members are destroyed in reverse order: the work object is closed first, after which no
    // callback can access the lock or the pending queue.
    wil::srwlock m_lock;
    unique_tp_env m_environment;
    unique_tp_pool m_pool;
    _Guarded_by_(m_lock) std::deque<std::shared_ptr<details::WorkItemBase>> m_pending;
    _Guarded_by_(m_lock) bool m_shuttingDown { false };
    wil::unique_threadpool_work m_work;
};

} // namespace wsl::windows::common
