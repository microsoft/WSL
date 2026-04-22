/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WslSlowOperation.cpp

Abstract:

    See header for contract. A single threadpool timer is armed in the constructor for
    SlowThreshold; if it fires, the callback records that the operation went slow and
    emits SlowOperationStarted telemetry + log. The destructor cancels and drains the
    timer (so the callback cannot touch *this after destruction) and, if the callback
    already fired, emits a terminating SlowOperationEnded event with the final elapsed
    time.

--*/

#include "precomp.h"
#include "WslSlowOperation.h"

namespace {
FILETIME RelativeFileTime(std::chrono::milliseconds Relative) noexcept
{
    // Negative FILETIME means "relative to now", in 100ns units.
    ULARGE_INTEGER due{};
    due.QuadPart = static_cast<ULONGLONG>(-(Relative.count() * 10'000LL));
    FILETIME ft{};
    ft.dwLowDateTime = due.LowPart;
    ft.dwHighDateTime = due.HighPart;
    return ft;
}
} // namespace

WslSlowOperation::WslSlowOperation(_In_z_ const char* Name, std::chrono::milliseconds SlowThreshold) :
    m_name(Name), m_slowThreshold(SlowThreshold), m_start(std::chrono::steady_clock::now()), m_uncaughtOnEntry(std::uncaught_exceptions())
{
    m_timer.reset(CreateThreadpoolTimer(OnTimerFired, this, nullptr));
    THROW_LAST_ERROR_IF_NULL(m_timer);

    FILETIME due = RelativeFileTime(m_slowThreshold);
    SetThreadpoolTimer(m_timer.get(), &due, 0, 0);
}

WslSlowOperation::~WslSlowOperation() noexcept
{
    // Cancel any pending fire, then block until an in-flight callback drains. This is
    // the guarantee that OnTimerFired will not dereference `this` after destruction.
    if (m_timer)
    {
        SetThreadpoolTimer(m_timer.get(), nullptr, 0, 0);
        WaitForThreadpoolTimerCallbacks(m_timer.get(), TRUE);
    }

    if (!m_fired.load(std::memory_order_acquire))
    {
        return;
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - m_start);

    // Mirror the idiom used elsewhere in the codebase: avoid calling
    // wil::ResultFromCaughtException from a destructor (UB outside a catch handler);
    // use uncaught_exceptions() delta to surface "did this scope unwind?" only.
    const HRESULT hr = (std::uncaught_exceptions() > m_uncaughtOnEntry) ? E_FAIL : S_OK;

    try
    {
        WSL_LOG_TELEMETRY(
            "SlowOperationEnded",
            PDT_ProductAndServicePerformance,
            TraceLoggingString(m_name, "name"),
            TraceLoggingInt64(elapsed.count(), "elapsedMs"),
            TraceLoggingHResult(hr, "hr"));
    }
    CATCH_LOG()
}

void CALLBACK WslSlowOperation::OnTimerFired(PTP_CALLBACK_INSTANCE, PVOID Context, PTP_TIMER) noexcept
try
{
    auto* self = static_cast<WslSlowOperation*>(Context);
    if (!self->m_fired.exchange(true, std::memory_order_acq_rel))
    {
        self->EmitSlowThresholdCrossed();
    }
}
CATCH_LOG()

void WslSlowOperation::EmitSlowThresholdCrossed() noexcept
try
{
    WSL_LOG_TELEMETRY(
        "SlowOperationStarted",
        PDT_ProductAndServicePerformance,
        TraceLoggingString(m_name, "name"),
        TraceLoggingInt64(m_slowThreshold.count(), "thresholdMs"));

    WSL_LOG("SlowOperation", TraceLoggingString(m_name, "name"), TraceLoggingInt64(m_slowThreshold.count(), "thresholdMs"));
}
CATCH_LOG()
