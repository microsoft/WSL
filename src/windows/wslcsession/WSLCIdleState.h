/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCIdleState.h

Abstract:

    Shared idle-termination state for WSLC session VM lifecycle.

--*/
#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <utility>
#include <wil/resource.h>

namespace wsl::windows::service::wslc {

// Shared idle-termination state for a WSLC session.
//
// A single activity refcount is the only source of truth for "the VM is needed". Everything that
// requires the VM holds a reference for as long as it needs it:
//   * in-flight operations (WSLCSession::VmLease),
//   * running/created containers themselves (WSLCContainerImpl's ActivityRef),
//   * client-held process wrappers (WSLCProcess keep-alive token),
//   * multi-round-trip CLI operations (WSLCSession::BeginContainerOperation).
//
// When the count drops to zero a threadpool timer is armed for the idle grace period; if it
// elapses without new activity the session-supplied OnIdle callback tears the VM down. Any new
// activity before it fires cancels the timer.
//
// Held via shared_ptr so activity holders (container/process wrappers, operation tokens) can
// outlive the owning session and release activity without dereferencing it. The session clears the
// callback and drains the timer in Disarm() during teardown, after which a late release simply
// decrements the count and never re-enters the destroyed session.
class IdleState
{
public:
    IdleState() = default;

    IdleState(const IdleState&) = delete;
    IdleState& operator=(const IdleState&) = delete;

    // Installs the idle-teardown callback and grace period and creates the timer. Called once by
    // the owning session after construction. OnIdle runs on a threadpool thread.
    void Initialize(std::chrono::milliseconds GracePeriod, std::function<void()> OnIdle)
    {
        auto lock = m_lock.lock_exclusive();
        m_gracePeriod = GracePeriod;
        m_onIdle = std::move(OnIdle);
        m_timer.reset(CreateThreadpoolTimer(&IdleState::TimerCallback, this, nullptr));
        THROW_LAST_ERROR_IF(!m_timer);
    }

    // Permanently disables idle teardown: clears the callback so no further arm has any effect, and
    // drains any pending/running timer callback. Must be called by the session (with its own lock
    // released) during teardown, before the session object is destroyed, so no callback can
    // reference it afterwards.
    void Disarm() noexcept
    {
        PTP_TIMER timer = nullptr;
        {
            auto lock = m_lock.lock_exclusive();
            m_onIdle = nullptr;
            timer = m_timer.get();
            if (timer != nullptr)
            {
                SetThreadpoolTimer(timer, nullptr, 0, 0);
            }
        }

        // Drain any in-flight callback outside the lock; it may take the session lock.
        if (timer != nullptr)
        {
            WaitForThreadpoolTimerCallbacks(timer, TRUE);
        }
    }

    // Records the start of an activity; cancels any pending idle teardown on the 0->1 transition.
    void AddActivity() noexcept
    {
        auto lock = m_lock.lock_exclusive();
        if (m_activityCount.fetch_add(1) == 0)
        {
            CancelLockHeld();
        }
    }

    // Records the end of an activity; arms the idle timer on the 1->0 transition.
    void ReleaseActivity() noexcept
    {
        auto lock = m_lock.lock_exclusive();
        const int previous = m_activityCount.fetch_sub(1);
        FAIL_FAST_IF(previous <= 0); // Underflow is a fatal bug, not a recoverable condition.
        if (previous == 1)
        {
            ArmLockHeld();
        }
    }

    int ActivityCount() const noexcept
    {
        return m_activityCount.load();
    }

private:
    static void CALLBACK TimerCallback(PTP_CALLBACK_INSTANCE, PVOID Context, PTP_TIMER) noexcept
    try
    {
        auto* self = static_cast<IdleState*>(Context);

        std::function<void()> onIdle;
        {
            auto lock = self->m_lock.lock_exclusive();

            // Activity resumed (count != 0) or teardown raced us (callback cleared): nothing to do.
            if (self->m_activityCount.load() != 0 || !self->m_onIdle)
            {
                return;
            }

            // Copy and invoke outside the lock: OnIdle takes the session lock, and holding this
            // lock across that would invert the session-lock -> idle-lock ordering.
            onIdle = self->m_onIdle;
        }

        onIdle();
    }
    CATCH_LOG()

    void ArmLockHeld() noexcept
    {
        if (!m_timer || !m_onIdle)
        {
            return;
        }

        // Relative due time is expressed as a negative count of 100ns intervals.
        const int64_t relative = -static_cast<int64_t>(m_gracePeriod.count()) * 10000;
        FILETIME due{};
        due.dwLowDateTime = static_cast<DWORD>(relative & 0xFFFFFFFF);
        due.dwHighDateTime = static_cast<DWORD>((relative >> 32) & 0xFFFFFFFF);
        SetThreadpoolTimer(m_timer.get(), &due, 0, 0);
    }

    void CancelLockHeld() noexcept
    {
        if (m_timer)
        {
            SetThreadpoolTimer(m_timer.get(), nullptr, 0, 0);
        }
    }

    std::atomic<int> m_activityCount{0};
    wil::srwlock m_lock;

    _Guarded_by_(m_lock) std::function<void()> m_onIdle;
    _Guarded_by_(m_lock) std::chrono::milliseconds m_gracePeriod { 0 };
    _Guarded_by_(m_lock) wil::unique_threadpool_timer m_timer;
};

// RAII activity hold on an IdleState: increments on construction and decrements on destruction or
// reset(). Movable, non-copyable. Used by running/created containers to keep the VM alive without
// a client reference. Holds the IdleState via shared_ptr so it is safe even if it outlives the
// owning session.
class ActivityRef
{
public:
    ActivityRef() = default;

    explicit ActivityRef(std::shared_ptr<IdleState> State) noexcept : m_state(std::move(State))
    {
        if (m_state)
        {
            m_state->AddActivity();
        }
    }

    ActivityRef(ActivityRef&& Other) noexcept : m_state(std::exchange(Other.m_state, nullptr))
    {
    }

    ActivityRef& operator=(ActivityRef&& Other) noexcept
    {
        if (this != &Other)
        {
            reset();
            m_state = std::exchange(Other.m_state, nullptr);
        }

        return *this;
    }

    ActivityRef(const ActivityRef&) = delete;
    ActivityRef& operator=(const ActivityRef&) = delete;

    ~ActivityRef()
    {
        reset();
    }

    void reset() noexcept
    {
        if (m_state)
        {
            m_state->ReleaseActivity();
            m_state.reset();
        }
    }

    explicit operator bool() const noexcept
    {
        return m_state != nullptr;
    }

private:
    std::shared_ptr<IdleState> m_state;
};

} // namespace wsl::windows::service::wslc
