/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WslSlowOperation.h

Abstract:

    RAII guard that watches a scoped operation. A threadpool timer is armed in the
    constructor for `SlowThreshold` (10 s default). On the fast path (scope exits
    before the threshold) the destructor cancels the timer and nothing is emitted.
    If the threshold is reached first -- including while the scope is still running
    or hung indefinitely -- the timer callback fires once, emitting a single
    `SlowOperation` telemetry event plus a matching debug log so the backend can
    attribute where time is spent.

    Usage:

        WslSlowOperation slow{"WaitForMiniInitConnect"};
        m_miniInitChannel = wsl::shared::SocketChannel{AcceptConnection(timeout), ...};

--*/

#pragma once

#include <windows.h>
#include <wil/resource.h>
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

    const char* const m_name;
    const std::chrono::milliseconds m_slowThreshold;
    wil::unique_threadpool_timer m_timer;
};
