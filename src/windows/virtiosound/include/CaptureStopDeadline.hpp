// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include <chrono>

namespace wsl::virtio_sound::native::windows::detail {

// Shared controller state for a bounded capture Stop. A guest Stop/Reset can
// cancel a Start before the controller has put any command on the worker pipe;
// in that idle case there is nothing to acknowledge and therefore no deadline
// to arm. If the Start write races that cancellation, AfterStartSent observes
// the changed desired epoch and starts the deadline after the write completes.
class CaptureStopDeadline final
{
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    enum class Phase
    {
        Idle,
        StartSent,
        Capturing,
        StopSent,
        ShutdownSent,
        Closed,
    };

    void OnStopOrResetRequest(Phase phase, TimePoint now, Clock::duration timeout) noexcept
    {
        if (phase == Phase::Idle || phase == Phase::Closed)
        {
            return;
        }
        ArmEarlier(now + timeout);
    }

    void AfterStartSent(bool desiredEpochStillMatches, TimePoint now, Clock::duration timeout) noexcept
    {
        if (!desiredEpochStillMatches)
        {
            ArmIfMissing(now, timeout);
        }
    }

    void AfterStopSent(TimePoint now, Clock::duration timeout) noexcept
    {
        ArmIfMissing(now, timeout);
    }

    void ArmIfMissing(TimePoint now, Clock::duration timeout) noexcept
    {
        if (!m_armed)
        {
            m_deadline = now + timeout;
            m_armed = true;
        }
    }

    void OnWirePhaseChanged(Phase phase) noexcept
    {
        if (phase == Phase::Idle || phase == Phase::Closed)
        {
            m_armed = false;
            m_deadline = TimePoint{};
        }
    }

    bool Expired(Phase phase, TimePoint now) const noexcept
    {
        const bool acknowledgementPending = phase == Phase::StartSent || phase == Phase::Capturing || phase == Phase::StopSent;
        return acknowledgementPending && m_armed && now >= m_deadline;
    }

    bool IsArmed() const noexcept
    {
        return m_armed;
    }

private:
    void ArmEarlier(TimePoint deadline) noexcept
    {
        if (!m_armed || deadline < m_deadline)
        {
            m_deadline = deadline;
            m_armed = true;
        }
    }

    bool m_armed{};
    TimePoint m_deadline{};
};

} // namespace wsl::virtio_sound::native::windows::detail
