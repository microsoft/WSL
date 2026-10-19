// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include <cstddef>
#include <cstdint>

namespace wsl::windows::service::wslc {

// Access is serialized by the container lock. Epochs belong to staged runs, not committed state transitions.
class WSLCExitRequestState
{
public:
    void BeginRun() noexcept
    {
        ++m_epoch;
        m_pending = 0;
        m_requested = false;
    }

    std::uint64_t BeginRequest() noexcept
    {
        ++m_pending;
        return m_epoch;
    }

    bool CompleteRequest(std::uint64_t Epoch, bool Succeeded) noexcept
    {
        if (Epoch != m_epoch)
        {
            return false;
        }

        --m_pending;
        m_requested = m_requested || Succeeded;
        return true;
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
    std::uint64_t m_epoch = 0;
    size_t m_pending = 0;
    bool m_requested = false;
};

} // namespace wsl::windows::service::wslc
