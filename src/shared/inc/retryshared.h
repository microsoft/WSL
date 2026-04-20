/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    retryshared.h

Abstract:

    This file contains shared retry helper functions.

--*/

#pragma once

#include <chrono>
#include <functional>
#include <future>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>

namespace wsl::shared::retry {

constexpr auto AlwaysRetry = []() { return true; };

// NOTE: despite its name, `timeout` here is a RETRY BUDGET, not a per-call
// deadline. If `routine` hangs instead of throwing, `timeout` is never
// checked. For true per-call deadline semantics, use CallWithDeadline below.
template <typename T, typename TPeriod, typename TTimeout>
T RetryWithTimeout(const std::function<T()>& routine, TPeriod retryPeriod, TTimeout timeout, const std::function<bool()>& retryPred = AlwaysRetry)
{
    auto stop = std::chrono::steady_clock::now() + timeout;
    for (;;)
    {
        try
        {
            return routine();
        }
        catch (...)
        {
            if (!retryPred() || std::chrono::steady_clock::now() > stop)
            {
                throw;
            }

            std::this_thread::sleep_for(retryPeriod);
        }
    }
}

class DeadlineExceededError : public std::runtime_error
{
public:
    DeadlineExceededError() : std::runtime_error("call deadline exceeded")
    {
    }
};

// Enforces a true per-call deadline. If `routine` does not complete within
// `deadline`, the calling thread throws DeadlineExceededError. The routine
// continues running on a detached worker and its result (if it eventually
// arrives) is discarded via the shared state holding it.
//
// Requirements on the caller:
// - `routine` must be self-contained: it must not hold references to caller-
//   stack state that could die before the detached worker finishes. Prefer
//   capturing inputs by value and returning outputs via the result type.
// - `routine` must eventually return. This helper gives up waiting; it does
//   not and cannot cancel a native call that is truly deadlocked.
template <typename TRep, typename TPeriod, typename F>
auto CallWithDeadline(std::chrono::duration<TRep, TPeriod> deadline, F routine) -> std::invoke_result_t<F>
{
    using R = std::invoke_result_t<F>;
    std::packaged_task<R()> task(std::move(routine));
    auto future = task.get_future();
    std::thread worker(std::move(task));

    if (future.wait_for(deadline) == std::future_status::timeout)
    {
        worker.detach();
        throw DeadlineExceededError{};
    }

    worker.join();
    return future.get();
}

} // namespace wsl::shared::retry
