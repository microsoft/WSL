/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    TimedOperationOutcomeReporter.cpp

--*/

#include "precomp.h"
#include "TimedOperationOutcomeReporter.h"

namespace {
FILETIME RelativeFileTime(std::chrono::milliseconds Relative) noexcept
{
    return wil::filetime::from_int64(-wil::filetime_duration::one_millisecond * Relative.count());
}
} // namespace

TimedOperationOutcomeReporter::TimedOperationOutcomeReporter(std::chrono::milliseconds Timeout, Callback Callback) :
    m_timeout(Timeout), m_callback(std::move(Callback))
{
    THROW_HR_IF(E_INVALIDARG, m_timeout.count() <= 0);
    THROW_HR_IF(E_INVALIDARG, !m_callback);

    m_timer.reset(CreateThreadpoolTimer(OnTimerFired, this, nullptr));
    THROW_IF_NULL_ALLOC(m_timer.get());

    FILETIME due = RelativeFileTime(m_timeout);
    SetThreadpoolTimer(m_timer.get(), &due, 0, 0);
}

void TimedOperationOutcomeReporter::Complete(bool Success) noexcept
{
    const auto elapsed = Elapsed();
    const auto outcome = elapsed >= m_timeout ? TimedOperationOutcome::Timeout
                                              : (Success ? TimedOperationOutcome::Success : TimedOperationOutcome::Failure);
    Report(outcome, elapsed);
}

void CALLBACK TimedOperationOutcomeReporter::OnTimerFired(PTP_CALLBACK_INSTANCE, PVOID Context, PTP_TIMER) noexcept
{
    auto* self = static_cast<TimedOperationOutcomeReporter*>(Context);
    self->Report(TimedOperationOutcome::Timeout, self->Elapsed());
}

void TimedOperationOutcomeReporter::Report(TimedOperationOutcome Outcome, std::chrono::milliseconds Elapsed) noexcept
try
{
    if (!m_reported.exchange(true))
    {
        m_callback(Outcome, Elapsed);
    }
}
CATCH_LOG()

std::chrono::milliseconds TimedOperationOutcomeReporter::Elapsed() const noexcept
{
    return std::chrono::milliseconds{m_stopWatch.ElapsedMilliseconds()};
}
