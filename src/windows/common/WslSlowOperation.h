/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WslSlowOperation.h

Abstract:

    RAII guard that watches a scoped operation. On the fast path (operation completes under
    the slow threshold) nothing is emitted. When the slow threshold is crossed the guard
    emits a `SlowOperationStarted` telemetry event plus a `SlowOperation` debug log so the
    backend can attribute where time is spent; on scope exit it emits a terminating
    `SlowOperationEnded` event carrying the final elapsed time and hr (E_FAIL if the
    scope exits via exception, S_OK otherwise).

    Usage:

        WslSlowOperation slow{"WaitForMiniInitConnect"};
        m_miniInitChannel = wsl::shared::SocketChannel{AcceptConnection(timeout), ...};

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
    explicit WslSlowOperation(_In_z_ const char* Name, std::chrono::milliseconds SlowThreshold = std::chrono::seconds{10});

    ~WslSlowOperation() noexcept;

    WslSlowOperation(const WslSlowOperation&) = delete;
    WslSlowOperation& operator=(const WslSlowOperation&) = delete;
    WslSlowOperation(WslSlowOperation&&) = delete;
    WslSlowOperation& operator=(WslSlowOperation&&) = delete;

private:
    static void CALLBACK OnTimerFired(PTP_CALLBACK_INSTANCE, PVOID Context, PTP_TIMER) noexcept;

    void EmitSlowThresholdCrossed() noexcept;

    const char* const m_name;
    const std::chrono::milliseconds m_slowThreshold;
    const std::chrono::steady_clock::time_point m_start;
    const int m_uncaughtOnEntry;

    // Set to true by the timer callback when the slow threshold is crossed; read by the
    // destructor to decide whether to emit SlowOperationEnded. exchange() in the callback
    // is defensive (the timer is a single-shot so the callback only runs once anyway).
    std::atomic<bool> m_fired{false};
    wil::unique_threadpool_timer m_timer;
};
