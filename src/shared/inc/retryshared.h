/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    retryshared.h

Abstract:

    This file contains shared retry helper functions.

--*/

#pragma once

#include <algorithm>

namespace wsl::shared::retry {

constexpr auto AlwaysRetry = []() { return true; };

template <typename T, typename TPeriod, typename TTimeout>
T RetryWithTimeout(
    const std::function<T()>& routine,
    TPeriod retryPeriod,
    TTimeout timeout,
    const std::function<bool()>& retryPred = AlwaysRetry,
    TPeriod initialRetryPeriod = TPeriod::zero())
{
    auto stop = std::chrono::steady_clock::now() + timeout;
    auto currentRetryPeriod = initialRetryPeriod == TPeriod::zero() ? retryPeriod : std::min(initialRetryPeriod, retryPeriod);
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

            std::this_thread::sleep_for(currentRetryPeriod);
            if (currentRetryPeriod < retryPeriod)
            {
                currentRetryPeriod += std::min(currentRetryPeriod, retryPeriod - currentRetryPeriod);
            }
        }
    }
}

#ifdef WIN32

template <typename T, typename TPeriod, typename TTimeout>
T RetryWithTimeout(const std::function<T()>& routine, TPeriod retryPeriod, TTimeout timeout, std::vector<HRESULT>&& retryErrors, TPeriod initialRetryPeriod = TPeriod::zero())
{
    auto pred = [retryErrors = std::move(retryErrors)]() {
        auto hr = HRESULT_FROM_WIN32(GetLastError());
        return std::find(retryErrors.begin(), retryErrors.end(), hr) != retryErrors.end();
    };

    return RetryWithTimeout(routine, retryPeriod, timeout, pred, initialRetryPeriod);
}

#endif

} // namespace wsl::shared::retry
