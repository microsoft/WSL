// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include <cstddef>
#include <cstdint>
#include <wil/resource.h>

namespace wsl::windows::service::wslc {

// Access is serialized by the container lock. Iterations identify accepted run attempts, not committed state transitions.
class WSLCExitRequestState
{
public:
    void BeginRun() noexcept
    {
        ++m_iteration;
        m_pending = 0;
        m_requested = false;
        m_idle.SetEvent();
    }

    std::uint64_t BeginRequest() noexcept
    {
        if (m_pending++ == 0)
        {
            m_idle.ResetEvent();
        }

        return m_iteration;
    }

    bool CompleteRequest(std::uint64_t Iteration, bool Succeeded) noexcept
    {
        if (!OwnsRun(Iteration))
        {
            return false;
        }

        if (--m_pending == 0)
        {
            m_idle.SetEvent();
        }

        m_requested = m_requested || Succeeded;
        return true;
    }

    bool OwnsRun(std::uint64_t Iteration) const noexcept
    {
        return Iteration == m_iteration;
    }

    void RequestExit() noexcept
    {
        m_requested = true;
    }

    bool SuppressesRestart() const noexcept
    {
        return m_requested || m_pending != 0;
    }

    bool HasPendingRequests() const noexcept
    {
        return m_pending != 0;
    }

    // Signaled while no request is pending. Can be waited on without holding the container lock.
    HANDLE IdleEvent() const noexcept
    {
        return m_idle.get();
    }

private:
    std::uint64_t m_iteration = 0;
    size_t m_pending = 0;
    bool m_requested = false;
    wil::unique_event m_idle{wil::EventOptions::ManualReset | wil::EventOptions::Signaled};
};

} // namespace wsl::windows::service::wslc
