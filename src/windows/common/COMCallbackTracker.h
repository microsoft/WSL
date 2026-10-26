// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>

namespace wsl::windows::common {

class COMCallbackTracker
{
public:
    void Register()
    {
        std::lock_guard lock(m_lock);
        THROW_HR_IF(E_ABORT, m_stopping);
        THROW_IF_FAILED(CoEnableCallCancellation(nullptr));
        auto disableCancellation = wil::scope_exit([]() { LOG_IF_FAILED(CoDisableCallCancellation(nullptr)); });
        ++m_threads[GetCurrentThreadId()];
        disableCancellation.release();
    }

    void Unregister(DWORD threadId) noexcept
    {
        {
            std::lock_guard lock(m_lock);
            const auto it = m_threads.find(threadId);
            WI_VERIFY(it != m_threads.end() && it->second > 0);
            if (--it->second == 0)
            {
                m_threads.erase(it);
            }

            m_cv.notify_all();
        }

        LOG_IF_FAILED(CoDisableCallCancellation(nullptr));
    }

    void BeginShutdown() noexcept
    {
        std::lock_guard lock(m_lock);
        m_stopping = true;
    }

    bool CancelPending() noexcept
    {
        std::lock_guard lock(m_lock);
        return CancelPendingLockHeld();
    }

    void CancelAndWait() noexcept
    {
        std::unique_lock lock(m_lock);
        m_stopping = true;
        while (!CancelPendingLockHeld())
        {
            m_cv.wait_for(lock, std::chrono::milliseconds(10));
        }
    }

private:
    __requires_lock_held(m_lock) bool CancelPendingLockHeld() noexcept
    {
        for (const auto& [threadId, count] : m_threads)
        {
            const auto result = CoCancelCall(threadId, 0);
            if (result != E_NOINTERFACE && result != RPC_E_CALL_COMPLETE && result != RPC_E_CALL_CANCELED)
            {
                LOG_IF_FAILED(result);
            }
        }

        return m_threads.empty();
    }

    std::mutex m_lock;
    std::condition_variable m_cv;
    _Guarded_by_(m_lock) bool m_stopping = false;
    _Guarded_by_(m_lock) std::map<DWORD, size_t> m_threads;
};

} // namespace wsl::windows::common
