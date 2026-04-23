/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    SlowOperationWatcher.h

Abstract:

    RAII guard that watches a scoped operation. A threadpool timer is armed in the
    constructor for `SlowThreshold` (10 s default). On the fast path (scope exits
    before the threshold) the watcher's destructor cancels and drains the timer and
    nothing is emitted. If the threshold is reached first -- including while the
    scope is still running or hung indefinitely -- the timer callback fires once,
    emitting a single `SlowOperation` telemetry event carrying the phase name and
    the call site captured via std::source_location, so the backend can attribute
    where time is spent.

    Usage:

        SlowOperationWatcher slow{"WaitForMiniInitConnect"};
        m_miniInitChannel = wsl::shared::SocketChannel{AcceptConnection(timeout), ...};

    If the scope needs to outlive the watched operation (for example to keep a
    pointer into an internal receive buffer alive without a nested block), call
    Reset() to disarm the watcher early:

        SlowOperationWatcher slow{"WaitForCreateInstanceResult"};
        const auto& result = channel.ReceiveMessage<...>(...);
        slow.Reset();
        // result remains valid and usable here

--*/

#pragma once

#include <windows.h>
#include <wil/resource.h>
#include <chrono>
#include <source_location>

class SlowOperationWatcher
{
public:
    // Name must have static storage duration (string literal). It is emitted verbatim into
    // telemetry, so keep it a short CamelCase phase identifier that the backend query can
    // switch on (e.g. "WaitForMiniInitConnect").
    explicit SlowOperationWatcher(
        _In_z_ const char* Name,
        std::chrono::milliseconds SlowThreshold = std::chrono::seconds{10},
        std::source_location Location = std::source_location::current());

    ~SlowOperationWatcher() noexcept = default;

    // Disarm the watcher early. After Reset() returns, the threshold callback is
    // guaranteed not to fire. Relies on wil::unique_threadpool_timer's destroyer to
    // cancel pending callbacks and drain any in-flight one.
    void Reset() noexcept;

    SlowOperationWatcher(const SlowOperationWatcher&) = delete;
    SlowOperationWatcher& operator=(const SlowOperationWatcher&) = delete;
    SlowOperationWatcher(SlowOperationWatcher&&) = delete;
    SlowOperationWatcher& operator=(SlowOperationWatcher&&) = delete;

private:
    static void CALLBACK OnTimerFired(PTP_CALLBACK_INSTANCE, PVOID Context, PTP_TIMER) noexcept;

    const char* const m_name;
    const std::chrono::milliseconds m_slowThreshold;
    const std::source_location m_location;
    wil::unique_threadpool_timer m_timer;
};
