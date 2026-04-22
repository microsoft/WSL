/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WslSlowOperation.cpp

Abstract:

    See header for contract. A single-shot threadpool timer is armed for SlowThreshold
    in the constructor. If it fires, the callback emits one `SlowOperation` telemetry
    event plus a matching debug log. The destructor cancels and drains the timer
    (WaitForThreadpoolTimerCallbacks with fCancelPendingCallbacks=TRUE), so the
    callback is guaranteed not to dereference `*this` after destruction.

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
    m_name(Name), m_slowThreshold(SlowThreshold)
{
    m_timer.reset(CreateThreadpoolTimer(OnTimerFired, this, nullptr));
    THROW_LAST_ERROR_IF_NULL(m_timer);

    FILETIME due = RelativeFileTime(m_slowThreshold);
    SetThreadpoolTimer(m_timer.get(), &due, 0, 0);
}

WslSlowOperation::~WslSlowOperation() noexcept
{
    // Cancel any pending fire and block until an in-flight callback drains. This is the
    // guarantee that OnTimerFired will not dereference `this` after destruction.
    if (m_timer)
    {
        SetThreadpoolTimer(m_timer.get(), nullptr, 0, 0);
        WaitForThreadpoolTimerCallbacks(m_timer.get(), TRUE);
    }
}

void CALLBACK WslSlowOperation::OnTimerFired(PTP_CALLBACK_INSTANCE, PVOID Context, PTP_TIMER) noexcept
try
{
    auto* self = static_cast<WslSlowOperation*>(Context);

    WSL_LOG_TELEMETRY(
        "SlowOperation",
        PDT_ProductAndServicePerformance,
        TraceLoggingValue(self->m_name, "name"),
        TraceLoggingInt64(self->m_slowThreshold.count(), "thresholdMs"));

    WSL_LOG(
        "SlowOperation", TraceLoggingValue(self->m_name, "name"), TraceLoggingInt64(self->m_slowThreshold.count(), "thresholdMs"));
}
CATCH_LOG()
