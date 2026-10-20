// Copyright (C) Microsoft Corporation. All rights reserved.

/*++

Module Name:

    AsyncExecution.h

Abstract:

    Provides ForEachAsync, a generic helper for executing a work callback
    over a collection concurrently on a bounded thread pool.

--*/
#pragma once

#include <algorithm>
#include <exception>
#include <memory>
#include <type_traits>
#include <vector>
#include <wil/result_macros.h>
#include "ThreadPool.h"

namespace wsl::windows::common {

// Invokes onWork for each element in items on a private thread pool that runs at most
// maxConcurrency items at once. As soon as one item completes, the next pending item starts.
// Results are delivered serially, in input order, to onSuccess. Errors are delivered serially,
// in input order, to onError.
//
// If onSuccess or onError throws, items that have not started are canceled, and ForEachAsync
// waits for running items to return before the exception propagates. No call to onWork runs
// after ForEachAsync returns or throws. Running work is never interrupted; work that needs to
// stop early must observe its own cancellation signal.
//
// onWork is invoked concurrently and must be safe to call from multiple threads. Thread pool
// threads have no per-thread initialization, so onWork must perform any setup it needs, such
// as CoInitializeEx.
//
// TWork   : const TItem& -> TResult                              (called concurrently)
// TSuccess: TResult -> void                                      (called serially)
// TError  : (const TItem&, const std::exception_ptr&) -> void    (called serially)
template <typename TItem, typename TWork, typename TSuccess, typename TError>
void ForEachAsync(const std::vector<TItem>& items, TWork onWork, TSuccess onSuccess, TError onError, DWORD maxConcurrency = 10)
{
    using TResult = std::invoke_result_t<TWork&, const TItem&>;
    static_assert(!std::is_void_v<TResult>, "onWork must return a result for onSuccess.");

    THROW_HR_IF(E_INVALIDARG, maxConcurrency == 0);

    if (items.empty())
    {
        return;
    }

    const auto threadCount = static_cast<DWORD>(std::min<size_t>(maxConcurrency, items.size()));

    std::vector<std::shared_ptr<WorkItem<TResult>>> workItems;
    workItems.reserve(items.size());

    // Declared after workItems so that the pool is destroyed first; its destructor cancels work
    // that has not started and waits for running work on every exit path.
    ThreadPool pool{threadCount, threadCount};
    for (const auto& item : items)
    {
        workItems.push_back(pool.SubmitWork([&onWork, &item]() { return onWork(item); }));
    }

    for (size_t i = 0; i < items.size(); ++i)
    {
        auto& workItem = *workItems[i];
        if (const auto& error = workItem.Error())
        {
            onError(items[i], error);
        }
        else
        {
            onSuccess(std::move(workItem.Result()));
        }
    }
}

} // namespace wsl::windows::common