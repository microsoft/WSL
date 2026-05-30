// Copyright (C) Microsoft Corporation. All rights reserved.

/*++

Module Name:

    AsyncExecution.h

Abstract:

    Provides ForEachAsync, a generic helper for executing a work callback
    over a collection concurrently using the Windows thread pool with bounded
    concurrency and cooperative cancellation.

--*/
#pragma once

#include <algorithm>
#include <chrono>
#include <memory>
#include <optional>
#include <utility>
#include <vector>
#include <wil/resource.h>
#include <wil/result_macros.h>

namespace wsl::windows::wslc {

namespace detail {

    template <typename TItem, typename TResult>
    struct WorkerResult
    {
        WorkerResult() = default;

        explicit WorkerResult(const TItem& item_) : item(item_)
        {
        }

        TItem item;
        std::optional<TResult> result;
        wil::ResultException error{S_OK};
        bool hasError{false};
    };

    // SharedContext holds state that must remain valid for the full lifetime of any running
    // callback, including after WorkerPool is destroyed on the timeout path. Owned via
    // shared_ptr and referenced by every SharedWorker.
    template <typename TWork>
    struct SharedContext
    {
        NON_COPYABLE(SharedContext);
        NON_MOVABLE(SharedContext);

        TWork onWork;
        wil::unique_event cancelEvent;

        explicit SharedContext(TWork onWork_) : onWork(std::move(onWork_))
        {
            cancelEvent.create(wil::EventOptions::ManualReset);
        }
    };

    // Holds per-worker state. Each Launch heap-allocates a shared_ptr<SharedWorker> as the
    // thread pool callback context, giving the callback shared ownership and ensuring this
    // memory is not freed while a callback is still running.
    template <typename TWork, typename TItem, typename TResult>
    struct SharedWorker
    {
        WorkerResult<TItem, TResult> workerResult;
        std::shared_ptr<SharedContext<TWork>> context;
        wil::unique_event done;
        wil::unique_threadpool_work work;
    };

    // Manages a fixed pool of SharedWorkers.
    template <typename TItem, typename TWork, typename TSuccess, typename TError>
    struct WorkerPool
    {
        NON_COPYABLE(WorkerPool);
        NON_MOVABLE(WorkerPool);

        using TResult = decltype(std::declval<TWork>()(std::declval<TItem>(), std::declval<HANDLE>()));
        using TSharedWorker = SharedWorker<TWork, TItem, TResult>;
        using TSharedContext = SharedContext<TWork>;

        std::vector<std::shared_ptr<TSharedWorker>> workers;
        std::vector<HANDLE> doneHandles;
        std::shared_ptr<TSharedContext> context;
        std::chrono::milliseconds timeout;
        DWORD timeoutMs{};
        DWORD cancelDrainMs{};

        WorkerPool(size_t workerCount, TWork onWork, std::chrono::milliseconds timeout_, std::chrono::milliseconds cancelDrainTimeout) :
            context(std::make_shared<TSharedContext>(std::move(onWork))),
            timeout(timeout_),
            timeoutMs(timeout_ == std::chrono::milliseconds::max() ? INFINITE : static_cast<DWORD>(timeout_.count())),
            cancelDrainMs(static_cast<DWORD>(cancelDrainTimeout.count()))
        {
            workers.reserve(workerCount);
            doneHandles.reserve(workerCount);

            for (size_t i = 0; i < workerCount; ++i)
            {
                auto worker = std::make_shared<TSharedWorker>();
                worker->done.create(wil::EventOptions::ManualReset);
                worker->context = context;

                doneHandles.push_back(worker->done.get());
                workers.push_back(std::move(worker));
            }
        }

        void Launch(size_t workerIndex, const TItem& item)
        {
            auto& worker = workers[workerIndex];
            worker->workerResult = WorkerResult<TItem, TResult>{};
            worker->workerResult.item = item;
            worker->done.ResetEvent();

            // Heap-allocate a shared_ptr as the callback context. The callback takes ownership,
            // keeping the worker and its SharedContext alive for the full duration of the callback
            // regardless of WorkerPool lifetime. Re-create the work item each launch so the
            // context pointer is fresh.
            auto* ctx = new std::shared_ptr<TSharedWorker>(worker);
            worker->work.reset(::CreateThreadpoolWork(ThreadPoolCallback, ctx, nullptr));
            if (!worker->work)
            {
                delete ctx;
                THROW_LAST_ERROR();
            }

            ::SubmitThreadpoolWork(worker->work.get());
        }

        void Drain(size_t workerIndex, TSuccess& onSuccess, TError& onError)
        {
            auto& worker = workers[workerIndex];

            // Ensure the callback has fully returned before reading results.
            ::WaitForThreadpoolWorkCallbacks(worker->work.get(), FALSE);

            if (worker->workerResult.hasError)
            {
                onError(worker->workerResult.item, worker->workerResult.error);
            }
            else if (worker->workerResult.result.has_value())
            {
                onSuccess(*worker->workerResult.result);
            }
        }

        // Signals cancellation, waits up to cancelDrainMs for workers to exit, then throws ERROR_TIMEOUT.
        // Workers that do not exit within cancelDrainMs are abandoned. Each running callback holds a
        // shared_ptr to its SharedWorker and SharedContext, so neither is freed while the callback runs.
        // onWork implementations must check the cancel event at natural checkpoints and exit promptly.
        //
        // Note: TerminateThread() is not used - it skips C++ destructors, leaves user-mode locks
        // permanently held (causing deadlocks), and corrupts COM apartment state.
        [[noreturn]] void CancelAndThrow(size_t remainingItems)
        {
            context->cancelEvent.SetEvent();

            ::WaitForMultipleObjects(static_cast<DWORD>(doneHandles.size()), doneHandles.data(), TRUE, cancelDrainMs);

            THROW_HR_MSG(
                HRESULT_FROM_WIN32(ERROR_TIMEOUT),
                "ForEachAsync: worker exceeded timeout of %lld ms (%zu items remaining).",
                static_cast<long long>(timeout.count()),
                remainingItems);
        }

        // Thread pool callback - invoked on a pool thread for each submitted work item.
        // Takes ownership of the heap-allocated shared_ptr<TSharedWorker> passed as context,
        // ensuring the worker and its SharedContext remain alive for the duration of this call.
        static void CALLBACK ThreadPoolCallback(PTP_CALLBACK_INSTANCE, void* context, PTP_WORK) noexcept
        {
            const std::unique_ptr<std::shared_ptr<TSharedWorker>> owner(static_cast<std::shared_ptr<TSharedWorker>*>(context));
            auto& worker = **owner;

            try
            {
                worker.workerResult.result = worker.context->onWork(worker.workerResult.item, worker.context->cancelEvent.get());
            }
            catch (const wil::ResultException& ex)
            {
                worker.workerResult.hasError = true;
                worker.workerResult.error = ex;
            }
            catch (...)
            {
                worker.workerResult.hasError = true;
                worker.workerResult.error = wil::ResultException{wil::details::ResultFromCaughtException()};
            }

            worker.done.SetEvent();
        }
    };

} // namespace detail

// Invokes onWork for each element in items concurrently using the Windows thread pool,
// with concurrency bounded to poolSize. Results are delivered serially to onSuccess.
// Errors are delivered serially to onError.
//
// onWork receives a HANDLE to a cancellation event and should check it at natural
// checkpoints using WaitForSingleObject(cancel, 0), returning early if it is set.
// On timeout, the event is signalled and ForEachAsync waits up to cancelDrainTimeout
// for workers to exit before throwing HRESULT_FROM_WIN32(ERROR_TIMEOUT).
//
// poolSize must not exceed MAXIMUM_WAIT_OBJECTS (64).
//
// The timeout is a safety net against indefinite hangs, not a strict per-worker limit.
// A worker that hangs while other workers are still completing will be caught in the
// final wait at most one full timeout after all other work has finished.
//
// Note: thread pool threads have no guaranteed per-thread initialization. Callers
// whose onWork requires per-thread setup (e.g. CoInitializeEx) must perform it at
// the start of the onWork lambda.
//
// TWork   : (TItem, HANDLE cancelEvent) -> TResult        (called concurrently)
// TSuccess: TResult -> void                               (called serially)
// TError  : (TItem, wil::ResultException) -> void         (called serially)
template <typename TItem, typename TWork, typename TSuccess, typename TError>
void ForEachAsync(
    const std::vector<TItem>& items,
    TWork onWork,
    TSuccess onSuccess,
    TError onError,
    size_t poolSize = 10,
    std::chrono::milliseconds timeout = std::chrono::milliseconds::max(),
    std::chrono::milliseconds cancelDrainTimeout = std::chrono::seconds(5))
{
    THROW_HR_IF(E_INVALIDARG, poolSize == 0);
    THROW_HR_IF(E_INVALIDARG, poolSize > MAXIMUM_WAIT_OBJECTS);

    if (items.empty())
    {
        return;
    }

    const size_t workerCount = std::min(poolSize, items.size());

    detail::WorkerPool<TItem, TWork, TSuccess, TError> pool{workerCount, std::move(onWork), timeout, cancelDrainTimeout};

    // Fill the pool - submit one item per worker to saturate all workers immediately.
    size_t nextItem = 0;
    for (; nextItem < workerCount; ++nextItem)
    {
        pool.Launch(nextItem, items[nextItem]);
    }

    // Keep the pool full - as each worker completes, drain its result and immediately
    // assign it the next pending item. WaitForMultipleObjects(FALSE) wakes on the first
    // completion, so no worker idles while work remains.
    while (nextItem < items.size())
    {
        const DWORD waitResult = ::WaitForMultipleObjects(static_cast<DWORD>(workerCount), pool.doneHandles.data(), FALSE, pool.timeoutMs);

        if (waitResult == WAIT_TIMEOUT)
        {
            pool.CancelAndThrow(items.size() - nextItem);
        }

        THROW_LAST_ERROR_IF(waitResult == WAIT_FAILED);

        const size_t workerIndex = waitResult - WAIT_OBJECT_0;
        pool.Drain(workerIndex, onSuccess, onError);
        pool.Launch(workerIndex, items[nextItem++]);
    }

    const DWORD finalWait = ::WaitForMultipleObjects(static_cast<DWORD>(workerCount), pool.doneHandles.data(), TRUE, pool.timeoutMs);

    if (finalWait == WAIT_TIMEOUT)
    {
        pool.CancelAndThrow(0);
    }

    THROW_LAST_ERROR_IF(finalWait == WAIT_FAILED);

    for (size_t i = 0; i < workerCount; ++i)
    {
        pool.Drain(i, onSuccess, onError);
    }
}

} // namespace wsl::windows::wslc
