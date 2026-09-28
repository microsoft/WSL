// Copyright (C) Microsoft Corporation. All rights reserved.

/*++

Module Name:

    AsyncExecution.h

Abstract:

    Provides ForEachAsync, a generic helper for executing a work callback
    over a collection concurrently in bounded batches using std::async.

--*/
#pragma once

#include <algorithm>
#include <future>
#include <optional>
#include <utility>
#include <vector>
#include <wil/result_macros.h>

namespace wsl::windows::wslc {

// Invokes onWork for each element in items concurrently, in batches of batchSize.
// Results are delivered serially to onSuccess. Errors are delivered serially to onError.
//
// This keeps wall time proportional to ceil(N / batchSize) rather than N for operations
// that have inherent per-item latency (e.g. network or IPC calls).
//
// Note: worker threads have no guaranteed per-thread initialization (e.g. COM). Callers
// whose onWork requires per-thread setup (such as CoInitializeEx) are responsible for
// performing it at the start of the onWork lambda.
//
// TWork   : TItem -> TResult                              (called concurrently)
// TSuccess: TResult -> void                               (called serially)
// TError  : (TItem, wil::ResultException) -> void         (called serially)
template <typename TItem, typename TWork, typename TSuccess, typename TError>
void ForEachAsync(const std::vector<TItem>& items, TWork onWork, TSuccess onSuccess, TError onError, size_t batchSize = 10)
{
    WI_ASSERT(batchSize > 0);
    THROW_HR_IF(E_INVALIDARG, batchSize == 0);

    using TResult = decltype(onWork(std::declval<TItem>()));

    struct BatchResult
    {
        explicit BatchResult(TItem capturedItem) : item(std::move(capturedItem))
        {
        }

        TItem item;
        std::optional<TResult> result;
        wil::ResultException error{S_OK};
        bool hasError{false};
    };

    for (size_t batchStart = 0; batchStart < items.size(); batchStart += batchSize)
    {
        const size_t batchEnd = std::min(batchStart + batchSize, items.size());

        std::vector<std::future<BatchResult>> futures;
        futures.reserve(batchEnd - batchStart);

        for (size_t i = batchStart; i < batchEnd; ++i)
        {
            const auto& item = items[i];
            futures.push_back(std::async(std::launch::async, [&onWork, item]() -> BatchResult {
                BatchResult result{item};
                try
                {
                    result.result = onWork(item);
                }
                catch (const wil::ResultException& ex)
                {
                    result.hasError = true;
                    result.error = ex;
                }
                return result;
            }));
        }

        for (auto& future : futures)
        {
            auto batchResult = future.get();

            if (batchResult.hasError)
            {
                onError(batchResult.item, batchResult.error);
            }
            else if (batchResult.result.has_value())
            {
                onSuccess(*batchResult.result);
            }
        }
    }
}

} // namespace wsl::windows::wslc
