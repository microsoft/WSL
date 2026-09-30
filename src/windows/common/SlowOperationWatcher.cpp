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
    // Negative FILETIME means "relative to now", in 100ns units. Matches the pattern used
    // elsewhere in the service (see Lifetime.cpp).
    return wil::filetime::from_int64(-wil::filetime_duration::one_millisecond * Relative.count());
}

// std::source_location::file_name() returns the path as the compiler saw it, which on
// MSVC is an absolute build-agent path. Strip to the basename so telemetry groups the
// same file across different build environments without leaking machine-specific paths.
// The substring is taken from the same null-terminated char array, so the returned view's
// data() is safe to pass to C APIs that expect a null-terminated string.
constexpr std::string_view Basename(std::string_view Path) noexcept
{
    const auto pos = Path.find_last_of("\\/");
    return pos == std::string_view::npos ? Path : Path.substr(pos + 1);
}

static_assert(Basename("/foo/bar/test.cpp") == "test.cpp");
static_assert(Basename("C:\\src\\test.cpp") == "test.cpp");
static_assert(Basename("no_separator.cpp") == "no_separator.cpp");
} // namespace

SlowOperationWatcher::SlowOperationWatcher(_In_z_ const char* Name, std::chrono::milliseconds SlowThreshold, std::source_location Location) :
    m_name(Name), m_slowThreshold(SlowThreshold), m_location(Location)
{
    m_timer.reset(CreateThreadpoolTimer(OnTimerFired, this, nullptr));
    THROW_IF_NULL_ALLOC(m_timer.get());

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
        TraceLoggingValue(Basename(self->m_location.file_name()).data(), "file"),
        TraceLoggingValue(self->m_location.function_name(), "function"),
        TraceLoggingUInt32(self->m_location.line(), "line"));
}
CATCH_LOG()
