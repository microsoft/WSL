/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    SlowOperationWatcher.cpp

Abstract:

    See header for contract. A single-shot threadpool timer is armed for MaxDuration in
    the constructor. Emission is at most one event, guarded by m_reported (atomic):

      - If the operation finishes first (destructor or Reset()), Finish() cancels+drains
        the timer and, if the elapsed time is at least SlowThreshold, emits one
        timedOut=false event with the real elapsed time. Under-threshold => silent.
      - If the operation is still running at MaxDuration, the timer fires once and emits
        one timedOut=true event (elapsed ~= MaxDuration) as the hang backstop, then does
        nothing more; a later scope exit sees m_reported set and stays silent.

    timedOut is a finished-vs-hung flag, NOT success-vs-failure: the watcher is a pure
    duration timer with no visibility into the operation's result, so a scope that exits
    by throwing is still reported timedOut=false.

    The timer is owned by wil::unique_threadpool_timer, whose destroyer cancels the pending
    callback and blocks for any in-flight callback before closing, so OnTimerFired cannot
    dereference `*this` after destruction and m_reported is stable once the timer is drained.

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

// clang-format off
SlowOperationWatcher::SlowOperationWatcher(
    _In_z_ const char* Name,
    Duration SlowThreshold,
    Duration MaxDuration,
    Sink OnSlow,
    std::source_location Location) noexcept :
    // clang-format on
    m_name(Name),
    m_slowThreshold(SlowThreshold),
    m_maxDuration(MaxDuration),
    m_location(Location),
    m_start(std::chrono::steady_clock::now()),
    m_sink(OnSlow),
    m_reported(false)
{
    // Internal invariants (all call sites pass compile-time constants, so a violation is a
    // programming error): a real sink, a positive threshold, and MaxDuration >= SlowThreshold.
    // The last one guarantees the hang backstop can never fire before the slow threshold, so a
    // timedOut=true record is never emitted for an operation that isn't yet "slow".
    WI_ASSERT(m_sink != nullptr);
    WI_ASSERT(m_slowThreshold.count() > 0);
    WI_ASSERT(m_maxDuration >= m_slowThreshold);

    // Best-effort: this is a passive telemetry observer, so timer allocation failure must not
    // propagate into (and abort) the watched operation. If the timer can't be created we simply
    // run without the hang backstop -- the completion path (Finish() on scope exit / Reset())
    // still emits a timedOut=false record for a slow operation, since it does not depend on the timer.
    m_timer.reset(CreateThreadpoolTimer(OnTimerFired, this, nullptr));
    if (m_timer)
    {
        // Single-shot: fire once at MaxDuration as the hang backstop. If the operation finishes
        // first, Finish() cancels this before it fires. The due time is a 64-bit FILETIME, so it
        // represents any realistic MaxDuration without truncation; period 0 = one-shot.
        FILETIME due = RelativeFileTime(m_maxDuration);
        SetThreadpoolTimer(m_timer.get(), &due, 0, 0);
    }
}

SlowOperationWatcher::~SlowOperationWatcher() noexcept
{
    Finish();
}

void SlowOperationWatcher::Reset() noexcept
{
    Finish();
}

void SlowOperationWatcher::Finish() noexcept
{
    // Cancel the backstop timer and drain any in-flight callback. After this returns no
    // OnTimerFired can run, so m_reported is stable.
    m_timer.reset();

    // The FIRST Finish() -- whether Reset() or the destructor -- permanently claims the
    // single report and decides emit-or-not from the elapsed time AT THIS MOMENT. This is
    // what makes Reset() an authoritative "operation ended here" marker: if the operation
    // was fast (< threshold) at Reset(), we still claim m_reported so the later destructor
    // cannot re-evaluate a larger elapsed (including post-Reset work) and emit a bogus
    // slow record. If it was slow, we emit exactly one timedOut=false record with the real
    // duration here, and the destructor is then a no-op. exchange also races safely against
    // the timer callback (which claims m_reported the same way).
    if (!m_reported.exchange(true))
    {
        const auto elapsed = Elapsed();
        if (elapsed >= m_slowThreshold)
        {
            Emit(false, elapsed);
        }
    }
}

SlowOperationWatcher::Duration SlowOperationWatcher::Elapsed() const noexcept
{
    return std::chrono::duration_cast<Duration>(std::chrono::steady_clock::now() - m_start);
}

void SlowOperationWatcher::Emit(bool TimedOut, Duration Elapsed) noexcept
{
    m_sink(Event{m_name, m_slowThreshold, Elapsed, TimedOut, m_location});
}

void CALLBACK SlowOperationWatcher::OnTimerFired(PTP_CALLBACK_INSTANCE, PVOID Context, PTP_TIMER) noexcept
try
{
    // The single-shot timer reached MaxDuration while the operation is still running: emit the
    // hang backstop record once (timedOut=true, elapsed ~= MaxDuration) and stop. If Finish()
    // already reported (a race with a near-simultaneous scope exit), exchange keeps this a no-op.
    auto* self = static_cast<SlowOperationWatcher*>(Context);
    if (!self->m_reported.exchange(true))
    {
        self->Emit(true, self->Elapsed());
    }
}
CATCH_LOG()

void SlowOperationWatcher::EmitTelemetry(const Event& Record) noexcept
try
{
    WSL_LOG_TELEMETRY(
        "SlowOperation",
        PDT_ProductAndServicePerformance,
        TraceLoggingValue(Record.Name, "name"),
        TraceLoggingInt64(Record.Threshold.count(), "thresholdMs"),
        TraceLoggingInt64(Record.Elapsed.count(), "elapsedMs"),
        TraceLoggingBool(Record.TimedOut, "timedOut"),
        TraceLoggingValue(Basename(Record.Location.file_name()).data(), "file"),
        TraceLoggingValue(Record.Location.function_name(), "function"),
        TraceLoggingUInt32(Record.Location.line(), "line"));
}
CATCH_LOG()
