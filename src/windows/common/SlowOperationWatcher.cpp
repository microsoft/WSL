/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    SlowOperationWatcher.cpp

Abstract:

    See header for contract. A single-shot threadpool timer is armed for SlowThreshold
    in the constructor. If it fires, the callback emits one `SlowOperation` telemetry
    event with the phase name and captured std::source_location. The timer is owned by
    wil::unique_threadpool_timer, whose destroyer cancels pending callbacks and blocks
    for any in-flight callback before closing, so OnTimerFired cannot dereference
    `*this` after destruction.

--*/

#include "precomp.h"
#include "SlowOperationWatcher.h"

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

SlowOperationWatcher::SlowOperationWatcher(
    _In_z_ const char* Name, std::chrono::milliseconds SlowThreshold, std::source_location Location) :
    m_name(Name), m_slowThreshold(SlowThreshold), m_location(Location)
{
    m_timer.reset(CreateThreadpoolTimer(OnTimerFired, this, nullptr));
    THROW_LAST_ERROR_IF_NULL(m_timer.get());

    FILETIME due = RelativeFileTime(m_slowThreshold);
    SetThreadpoolTimer(m_timer.get(), &due, 0, 0);
}

void SlowOperationWatcher::Reset() noexcept
{
    m_timer.reset();
}

void CALLBACK SlowOperationWatcher::OnTimerFired(PTP_CALLBACK_INSTANCE, PVOID Context, PTP_TIMER) noexcept
try
{
    auto* self = static_cast<SlowOperationWatcher*>(Context);

    WSL_LOG_TELEMETRY(
        "SlowOperation",
        PDT_ProductAndServicePerformance,
        TraceLoggingValue(self->m_name, "name"),
        TraceLoggingInt64(self->m_slowThreshold.count(), "thresholdMs"),
        TraceLoggingValue(self->m_location.file_name(), "file"),
        TraceLoggingValue(self->m_location.function_name(), "function"),
        TraceLoggingUInt32(self->m_location.line(), "line"));
}
CATCH_LOG()
