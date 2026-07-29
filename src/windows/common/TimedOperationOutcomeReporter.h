/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    TimedOperationOutcomeReporter.h

Abstract:

    Reports the outcome of an operation once, including when it exceeds a time limit.

--*/

#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <wil/resource.h>
#include "wslutil.h"

enum class TimedOperationOutcome
{
    Success,
    Failure,
    Timeout
};

class TimedOperationOutcomeReporter
{
public:
    using Callback = std::function<void(TimedOperationOutcome, std::chrono::milliseconds)>;

    TimedOperationOutcomeReporter(std::chrono::milliseconds Timeout, Callback Callback);
    ~TimedOperationOutcomeReporter() noexcept = default;

    void Complete(bool Success) noexcept;

    TimedOperationOutcomeReporter(const TimedOperationOutcomeReporter&) = delete;
    TimedOperationOutcomeReporter& operator=(const TimedOperationOutcomeReporter&) = delete;
    TimedOperationOutcomeReporter(TimedOperationOutcomeReporter&&) = delete;
    TimedOperationOutcomeReporter& operator=(TimedOperationOutcomeReporter&&) = delete;

private:
    static void CALLBACK OnTimerFired(PTP_CALLBACK_INSTANCE, PVOID Context, PTP_TIMER) noexcept;
    void Report(TimedOperationOutcome Outcome, std::chrono::milliseconds Elapsed) noexcept;
    std::chrono::milliseconds Elapsed() const noexcept;

    const std::chrono::milliseconds m_timeout;
    const Callback m_callback;
    wsl::windows::common::wslutil::StopWatch m_stopWatch;
    std::atomic_bool m_reported{false};

    // Keep the timer last so it is cancelled and drained before callback state is destroyed.
    wil::unique_threadpool_timer m_timer;
};
