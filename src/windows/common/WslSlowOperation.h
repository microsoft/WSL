/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WslSlowOperation.h

Abstract:

    RAII guard that watches a scoped operation. On the fast path (operation completes under
    the slow threshold) nothing is emitted. When the slow threshold is crossed the guard
    emits telemetry + log so the backend can attribute where time is spent; when the
    user-visible threshold is crossed the guard also emits a user-visible warning via
    EMIT_USER_WARNING so the user knows the long wait is intentional work, not a freeze.

    Usage:

        WslSlowOperation slow{"WaitForMiniInitConnect"};
        m_miniInitChannel = wsl::shared::SocketChannel{AcceptConnection(timeout), ...};
        // slow destructor cancels the timer; if slow threshold was crossed it emits a
        // terminating "SlowOperationEnded" telemetry event carrying the final elapsed
        // time and hr (E_FAIL if the scope exits via exception, S_OK otherwise).

--*/

#pragma once

#include <windows.h>
#include <wil/resource.h>
#include <atomic>
#include <chrono>

class WslSlowOperation
{
public:
    // Name must have static storage duration (string literal). It is emitted verbatim into
    // telemetry and log, so keep it a short CamelCase phase identifier that the backend
    // query can switch on (e.g. "WaitForMiniInitConnect").
    explicit WslSlowOperation(
        _In_z_ const char* Name,
        std::chrono::milliseconds SlowThreshold = std::chrono::seconds{10},
        std::chrono::milliseconds UserVisibleThreshold = std::chrono::seconds{60});

    ~WslSlowOperation() noexcept;

    WslSlowOperation(const WslSlowOperation&) = delete;
    WslSlowOperation& operator=(const WslSlowOperation&) = delete;
    WslSlowOperation(WslSlowOperation&&) = delete;
    WslSlowOperation& operator=(WslSlowOperation&&) = delete;

private:
    // The stage is the only shared state touched from both the timer callback and the
    // destructor; CAS on it serializes "fired exactly once" transitions.
    enum class Stage : int
    {
        Fast = 0,         // still within SlowThreshold — fast path, emit nothing on exit
        Slow = 1,         // crossed SlowThreshold (telemetry + log fired)
        UserVisible = 2,  // crossed UserVisibleThreshold (user warning also fired)
    };

    static void CALLBACK OnTimerFired(PTP_CALLBACK_INSTANCE, PVOID Context, PTP_TIMER) noexcept;

    void EmitSlowThresholdCrossed() noexcept;
    void EmitUserVisibleThresholdCrossed() noexcept;
    void ArmTimer(std::chrono::milliseconds Relative) noexcept;

    const char* const m_name;
    const std::chrono::milliseconds m_slowThreshold;
    const std::chrono::milliseconds m_userVisibleThreshold;
    const std::chrono::steady_clock::time_point m_start;
    const int m_uncaughtOnEntry;

    std::atomic<Stage> m_stage{Stage::Fast};
    wil::unique_threadpool_timer m_timer;
};
