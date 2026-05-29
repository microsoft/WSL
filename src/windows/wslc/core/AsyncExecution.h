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
        TItem item;
        std::optional<TResult> result;
        wil::ResultException error{S_OK};
        bool hasError{false};
    };

    // Holds all state for one thread pool worker. Owned via shared_ptr so the memory
    // remains valid if ForEachAsync unwinds while a work item is still running.
    template <typename TItem, typename TWork, typename TResult>
    struct SharedWorker
    {
        WorkerResult<TItem, TResult> workerResult;
        TWork* onWork{nullptr};
        HANDLE cancelHandle{nullptr};
        wil::unique_event done;
        wil::unique_threadpool_work work;
    };

    // Manages a fixed pool of SharedWorkers and a shared cancellation event.
    template <typename TItem, typename TWork, typename TSuccess, typename TError>
    struct WorkerPool
    {
        NON_COPYABLE(WorkerPool);
        NON_MOVABLE(WorkerPool);

        using TResult = decltype(std::declval<TWork>()(std::declval<TItem>(), std::declval<HANDLE>()));
        using TSharedWorker = SharedWorker<TItem, TWork, TResult>;

        std::vector<std::shared_ptr<TSharedWorker>> workers;
        std::vector<HANDLE> doneHandles;
        wil::unique_event cancelEvent;
        std::chrono::milliseconds timeout;
        DWORD cancelDrainMs{};

        WorkerPool(size_t poolSize, TWork& onWork, std::chrono::milliseconds timeout_, std::chrono::milliseconds cancelDrainTimeout) :
            timeout(timeout_), cancelDrainMs(static_cast<DWORD>(cancelDrainTimeout.count()))
        {
            cancelEvent.create(wil::EventOptions::ManualReset);

            workers.reserve(poolSize);
            doneHandles.reserve(poolSize);

            for (size_t i = 0; i < poolSize; ++i)
            {
                auto worker = std::make_shared<TSharedWorker>();
                worker->done.create(wil::EventOptions::ManualReset);
                worker->onWork = &onWork;
                worker->cancelHandle = cancelEvent.get();

                // Work item is created once per worker and reused for each dispatched item.
                worker->work.reset(::CreateThreadpoolWork(ThreadPoolCallback, worker.get(), nullptr));
                THROW_LAST_ERROR_IF(!worker->work);

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
        // Workers that do not exit within cancelDrainMs are abandoned - they retain shared_ptr ownership
        // of their state. onWork implementations must check the cancel event at natural checkpoints and
        // exit promptly.
        //
        // Note: TerminateThread() is not used - it skips C++ destructors, leaves user-mode locks
        // permanently held (causing deadlocks), and corrupts COM apartment state.
        [[noreturn]] void CancelAndThrow(size_t remainingItems)
        {
            cancelEvent.SetEvent();

            ::WaitForMultipleObjects(static_cast<DWORD>(doneHandles.size()), doneHandles.data(), TRUE, cancelDrainMs);

            THROW_HR_MSG(
                HRESULT_FROM_WIN32(ERROR_TIMEOUT),
                "ForEachAsync: worker exceeded timeout of %lld ms (%zu items remaining).",
                static_cast<long long>(timeout.count()),
                remainingItems);
        }

        // Thread pool callback - invoked on a pool thread for each submitted work item.
        static void CALLBACK ThreadPoolCallback(PTP_CALLBACK_INSTANCE, void* context, PTP_WORK) noexcept
        {
            auto& worker = *static_cast<TSharedWorker*>(context);

            try
            {
                worker.workerResult.result = (*worker.onWork)(worker.workerResult.item, worker.cancelHandle);
            }
            catch (const wil::ResultException& ex)
            {
                worker.workerResult.hasError = true;
                worker.workerResult.error = ex;
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

    const DWORD timeoutMs = (timeout == std::chrono::milliseconds::max()) ? INFINITE : static_cast<DWORD>(timeout.count());
    const size_t workerCount = std::min(poolSize, items.size());

    detail::WorkerPool<TItem, TWork, TSuccess, TError> pool{workerCount, onWork, timeout, cancelDrainTimeout};

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
        const DWORD waitResult = ::WaitForMultipleObjects(static_cast<DWORD>(workerCount), pool.doneHandles.data(), FALSE, timeoutMs);

        if (waitResult == WAIT_TIMEOUT)
        {
            pool.CancelAndThrow(items.size() - nextItem);
        }

        THROW_LAST_ERROR_IF(waitResult == WAIT_FAILED);
        const size_t workerIndex = waitResult - WAIT_OBJECT_0;
        pool.Drain(workerIndex, onSuccess, onError);
        pool.Launch(workerIndex, items[nextItem++]);
    }

    // Wait for all in-flight workers to finish and collect their final results.
    const DWORD finalWait = ::WaitForMultipleObjects(static_cast<DWORD>(workerCount), pool.doneHandles.data(), TRUE, timeoutMs);
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
