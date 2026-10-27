// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include <cstddef>
#include <cstdint>

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
    }

    std::uint64_t BeginRequest() noexcept
    {
        ++m_pending;
        return m_iteration;
    }

    bool CompleteRequest(std::uint64_t Iteration, bool Succeeded) noexcept
    {
        if (!OwnsRun(Iteration))
        {
            return false;
        }

        --m_pending;
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

private:
    std::uint64_t m_iteration = 0;
    size_t m_pending = 0;
    bool m_requested = false;
};

} // namespace wsl::windows::service::wslc
