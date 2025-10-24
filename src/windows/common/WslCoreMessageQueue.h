/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WslCoreMessageQueue.h

Abstract:

    This file contains a queuing implementation, guaranteeing running function objects
    with guaranteed serialization in a threadpool thread

--*/

#pragma once
#include <deque>
#include <functional>
#include <memory>
#include <variant>
#include <windows.h>
#include <wil/resource.h>

namespace wsl::core {
// forward-declare classes that can instantiate a WslThreadPoolWaitableResult object
class WslCoreMessageQueue;

class WslBaseThreadPoolWaitableResult
{
public:
    virtual ~WslBaseThreadPoolWaitableResult() noexcept = default;

private:
    // limit who can run() and abort()
    friend class WslCoreMessageQueue;

    virtual void run() noexcept = 0;
    virtual void abort() noexcept = 0;
};

template <typename TReturn>
class WslThreadPoolWaitableResult : public WslBaseThreadPoolWaitableResult
{
public:
    // throws a wil exception on failure
    template <typename FunctorType>
    explicit WslThreadPoolWaitableResult(FunctorType&& functor) : m_function(std::forward<FunctorType>(functor))
    {
    }

    ~WslThreadPoolWaitableResult() noexcept override = default;

    // returns ERROR_SUCCESS if the callback ran to completion
    // returns ERROR_TIMEOUT if this wait timed out
    // - this can be called multiple times if needing to probe
    // any other error code resulted from attempting to run the callback
    // - meaning it did *not* run to completion
    DWORD wait(DWORD timeout) const noexcept
    {
        if (!m_completionSignal.wait(timeout))
        {
            // not setting m_internalError to timeout
            // since the caller is allowed to try to wait() again later
            return ERROR_TIMEOUT;
        }
        const auto lock = m_lock.lock_shared();
        return m_internalError;
    }

    // waitable event handle, signaled when the callback has run to completion (or failed)
    HANDLE notification_event() const noexcept
    {
        return m_completionSignal.get();
    }

    const TReturn& read_result() const noexcept
    {
        return result;
    }

    // move the result out of the object for move-only types
    TReturn move_result() noexcept
    {
        TReturn move_out(std::move(result));
        return move_out;
    }

    // non-copyable
    WslThreadPoolWaitableResult(const WslThreadPoolWaitableResult&) = delete;
    WslThreadPoolWaitableResult& operator=(const WslThreadPoolWaitableResult&) = delete;

private:
    void run() noexcept override
    {
        // we are now running in the TP callback
        {
            const auto lock = m_lock.lock_exclusive();
            if (m_runStatus != RunStatus::NotYetRun)
            {
                // return early - the caller has already canceled this
                return;
            }
            m_runStatus = RunStatus::Running;
        }

        DWORD error = NO_ERROR;
        try
        {
            result = std::move(m_function());
        }
        catch (...)
        {
            const HRESULT hr = wil::ResultFromCaughtException();
            // HRESULT_TO_WIN32
            error = (HRESULT_FACILITY(hr) == FACILITY_WIN32) ? HRESULT_CODE(hr) : hr;
        }

        const auto lock = m_lock.lock_exclusive();
        WI_ASSERT(m_runStatus == RunStatus::Running);
        m_runStatus = RunStatus::RanToCompletion;
        m_internalError = error;
        m_completionSignal.SetEvent();
    }

    void abort() noexcept override
    {
        const auto lock = m_lock.lock_exclusive();
        // only override the error if we know we haven't started running their functor
        if (m_runStatus == RunStatus::NotYetRun)
        {
            m_runStatus = RunStatus::Canceled;
            m_internalError = ERROR_CANCELLED;
            m_completionSignal.SetEvent();
        }
    }

    std::function<TReturn(void)> m_function;
    // a notification event
    wil::unique_event m_completionSignal{wil::EventOptions::ManualReset};
    mutable wil::srwlock m_lock;
    TReturn result{};
    DWORD m_internalError = NO_ERROR;

    enum class RunStatus
    {
        NotYetRun,
        Running,
        RanToCompletion,
        Canceled
    } m_runStatus{RunStatus::NotYetRun};
};

class WslCoreMessageQueue
{
public:
    WslCoreMessageQueue() : m_tpEnvironment(0, 1)
    {
        // create a single-threaded threadpool
        m_tpHandle = m_tpEnvironment.create_tp(WorkCallback, this);
    }

    template <typename TReturn, typename FunctorType>
    std::shared_ptr<WslThreadPoolWaitableResult<TReturn>> submit_with_results(FunctorType&& functor) noexcept
    try
    {
        FAIL_FAST_IF(m_tpHandle.get() == nullptr);

        const auto new_result = std::make_shared<WslThreadPoolWaitableResult<TReturn>>(std::forward<FunctorType>(functor));
        // scope to the queue lock
        {
            const auto queueLock = m_lock.lock_exclusive();
            THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_CANCELLED), m_isCanceled);
            m_workItems.emplace_back(new_result);
        }

        // always maintain a 1:1 ratio for calls to SubmitWorkWithResults() and ::SubmitThreadpoolWork
        SubmitThreadpoolWork(m_tpHandle.get());
        return new_result;
    }
    catch (...)
    {
        LOG_CAUGHT_EXCEPTION();
        return nullptr;
    }

    template <typename FunctorType>
    bool submit(FunctorType&& functor) noexcept
    try
    {
        FAIL_FAST_IF(m_tpHandle.get() == nullptr);

        // scope to the queue lock
        {
            const auto queueLock = m_lock.lock_exclusive();
            THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_CANCELLED), m_isCanceled);
            m_workItems.emplace_back(std::forward<SimpleFunction_t>(functor));
        }

        // always maintain a 1:1 ratio for calls to SubmitWork() and ::SubmitThreadpoolWork
        SubmitThreadpoolWork(m_tpHandle.get());
        return true;
    }
    catch (...)
    {
        LOG_CAUGHT_EXCEPTION();
        return false;
    }

    // functors must return type HRESULT
    template <typename FunctorType>
    HRESULT submit_and_wait(FunctorType&& functor) noexcept
    try
    {
        HRESULT hr = HRESULT_FROM_WIN32(ERROR_OUTOFMEMORY);
        if (const auto waitableResult = submit_with_results<HRESULT>(std::forward<FunctorType>(functor)))
        {
            hr = HRESULT_FROM_WIN32(waitableResult->wait(INFINITE));
            if (SUCCEEDED(hr))
            {
                hr = waitableResult->read_result();
            }
        }
        return hr;
    }
    CATCH_RETURN()

    // cancels anything queued to the TP - this WslCoreMessageQueue instance can no longer be used
    void cancel() noexcept
    try
    {
        if (m_tpHandle)
        {
            // immediately release anyone waiting for these workitems not yet run
            {
                const auto queueLock = m_lock.lock_exclusive();
                m_isCanceled = true;

                for (const auto& work : m_workItems)
                {
                    // signal that these are canceled before we shutdown the TP which they could be scheduled
                    if (const auto* pWaitableWorkitem = std::get_if<WaitableFunction_t>(&work))
                    {
                        (*pWaitableWorkitem)->abort();
                    }
                }

                m_workItems.clear();
            }

            // force the m_tpHandle to wait and close the TP
            m_tpHandle.reset();
            m_tpEnvironment.reset();
        }
    }
    CATCH_LOG()

    bool isRunningInQueue() const noexcept
    {
        const auto currentThreadId = GetThreadId(GetCurrentThread());
        return currentThreadId == static_cast<DWORD>(InterlockedCompareExchange64(&m_threadpoolThreadId, 0ll, 0ll));
    }

    ~WslCoreMessageQueue() noexcept
    {
        cancel();
    }

    WslCoreMessageQueue(const WslCoreMessageQueue&) = delete;
    WslCoreMessageQueue& operator=(const WslCoreMessageQueue&) = delete;
    WslCoreMessageQueue(WslCoreMessageQueue&&) = delete;
    WslCoreMessageQueue& operator=(WslCoreMessageQueue&&) = delete;

private:
    struct TPEnvironment
    {
        using unique_tp_env = wil::unique_struct<TP_CALLBACK_ENVIRON, decltype(&DestroyThreadpoolEnvironment), DestroyThreadpoolEnvironment>;
        unique_tp_env m_tpEnvironment;

        using unique_tp_pool = wil::unique_any<PTP_POOL, decltype(&CloseThreadpool), CloseThreadpool>;
        unique_tp_pool m_threadPool;

        TPEnvironment(DWORD countMinThread, DWORD countMaxThread)
        {
            InitializeThreadpoolEnvironment(&m_tpEnvironment);

            m_threadPool.reset(CreateThreadpool(nullptr));
            THROW_LAST_ERROR_IF_NULL(m_threadPool.get());

            // Set min and max thread counts for custom thread pool
            THROW_LAST_ERROR_IF(!::SetThreadpoolThreadMinimum(m_threadPool.get(), countMinThread));
            SetThreadpoolThreadMaximum(m_threadPool.get(), countMaxThread);
            SetThreadpoolCallbackPool(&m_tpEnvironment, m_threadPool.get());
        }

        wil::unique_threadpool_work create_tp(PTP_WORK_CALLBACK callback, void* pv)
        {
            wil::unique_threadpool_work newThreadpool(CreateThreadpoolWork(callback, pv, (m_threadPool) ? &m_tpEnvironment : nullptr));
            THROW_LAST_ERROR_IF_NULL(newThreadpool.get());
            return newThreadpool;
        }

        void reset()
        {
            m_threadPool.reset();
            m_tpEnvironment.reset();
        }
    };

    using SimpleFunction_t = std::function<void()>;
    using WaitableFunction_t = std::shared_ptr<WslBaseThreadPoolWaitableResult>;
    using FunctionVariant_t = std::variant<SimpleFunction_t, WaitableFunction_t>;

    // the lock must be destroyed *after* the TP object (thus must be declared first)
    // since the lock is used in the TP callback
    // the lock is mutable to allow us to acquire the lock in const methods
    mutable wil::srwlock m_lock;
    TPEnvironment m_tpEnvironment;
    wil::unique_threadpool_work m_tpHandle;
    std::deque<FunctionVariant_t> m_workItems;
    mutable LONG64 m_threadpoolThreadId{0}; // useful for callers to assert they are running within the queue
    bool m_isCanceled{false};

    static void CALLBACK WorkCallback(PTP_CALLBACK_INSTANCE, void* Context, PTP_WORK) noexcept
    try
    {
        auto* pThis = static_cast<WslCoreMessageQueue*>(Context);

        FunctionVariant_t work;
        {
            const auto queueLock = pThis->m_lock.lock_exclusive();

            if (pThis->m_workItems.empty())
            {
                // pThis object is being destroyed and the queue was cleared
                return;
            }

            std::swap(work, pThis->m_workItems.front());
            pThis->m_workItems.pop_front();

            InterlockedExchange64(&pThis->m_threadpoolThreadId, GetThreadId(GetCurrentThread()));
        }

        // run the tasks outside the WslCoreMessageQueue lock
        const auto resetThreadIdOnExit = wil::scope_exit([pThis] { InterlockedExchange64(&pThis->m_threadpoolThreadId, 0ll); });
        if (work.index() == 0)
        {
            const auto& workItem = std::get<SimpleFunction_t>(work);
            workItem();
        }
        else
        {
            const auto& waitableWorkItem = std::get<WaitableFunction_t>(work);
            waitableWorkItem->run();
        }
    }
    CATCH_LOG()
};
} // namespace wsl::core
