/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    retryshared.h

Abstract:

    This file contains shared retry helper functions.

--*/

#pragma once

namespace wsl::shared::retry {

constexpr auto AlwaysRetry = []() { return true; };

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

} // namespace wsl::shared::retry
