// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include <algorithm>
#include <chrono>

namespace wsl::windows::service::wslc {

// Access is serialized by the container lock. Exit observations freeze the duration used by deferred decisions.
class WSLCRestartBackoff
{
public:
    using Clock = std::chrono::steady_clock;

    void Reset() noexcept
    {
        m_delay = {};
    }

    void OnStarted(Clock::time_point Time) noexcept
    {
        m_startedAt = Time;
    }

    void OnExited(Clock::time_point Time) noexcept
    {
        m_runDuration = Time - m_startedAt;
    }

    std::chrono::milliseconds NextDelay() noexcept
    {
        constexpr auto c_initialDelay = std::chrono::milliseconds{100};
        constexpr auto c_maximumDelay = std::chrono::milliseconds{60000};
        constexpr auto c_resetDuration = std::chrono::seconds{10};
        if (m_runDuration >= c_resetDuration)
        {
            Reset();
        }

        m_delay = m_delay.count() == 0 ? c_initialDelay : std::min(m_delay * 2, c_maximumDelay);
        return m_delay;
    }

private:
    Clock::time_point m_startedAt{};
    Clock::duration m_runDuration{};
    std::chrono::milliseconds m_delay{};
};

} // namespace wsl::windows::service::wslc
