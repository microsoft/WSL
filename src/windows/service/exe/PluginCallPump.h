// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include <wil/resource.h>
#include <atomic>
#include <deque>
#include <functional>

namespace wsl::windows::service {

//
// PluginCallPump implements the threaded-callback model for out-of-process
// plugin notifications.
//
// Problem: a plugin lifecycle notification (OnVMStarted, OnDistributionStarted,
// ...) is an outbound cross-process COM call. While the service thread is
// blocked inside that call, the plugin may call back into the service
// (MountFolder, ExecuteBinary, ...). That callback arrives on a *different* COM
// RPC thread, so it cannot re-enter the locks held by the notifying thread
// (m_instanceLock etc.) without a second, parallel locking scheme.
//
// Solution (matches the in-process model): make the outbound notification on a
// worker thread and "pump" the plugin's service-side API calls back onto the
// ORIGINAL notifying thread. Because the work then runs on the lock-holding
// thread, recursive locks (std::recursive_timed_mutex m_instanceLock) re-enter
// exactly as they did when plugins were loaded in-process — no second lock
// (m_callbackLock) and no out-of-band session registry are needed.
//
//   Notifying thread:  pump.Run([&]{ return host->OnVMStarted(...); });
//   RPC callback:      return pump.Invoke([&]{ return session->Mount...(); });
//
// A single pump instance services one outbound notification at a time. The
// pump thread is the single consumer; any number of RPC threads may Invoke().
//
class PluginCallPump
{
public:
    PluginCallPump();
    ~PluginCallPump() = default;

    PluginCallPump(const PluginCallPump&) = delete;
    PluginCallPump& operator=(const PluginCallPump&) = delete;
    PluginCallPump(PluginCallPump&&) = delete;
    PluginCallPump& operator=(PluginCallPump&&) = delete;

    // Runs `Notification` (the outbound host->On... COM call) on a dedicated
    // worker thread and pumps queued Invoke() calls on the CALLING thread until
    // the worker completes. The worker performs its own COM initialization, so
    // `Notification` should acquire the apartment-local host proxy itself.
    //
    // Returns the HRESULT returned by `Notification`. Any service-side work
    // requested by the plugin runs on the calling (lock-holding) thread, so
    // recursive locks behave exactly as in the in-process model.
    HRESULT Run(const std::function<HRESULT()>& Notification);

    // Called from a COM RPC callback thread. Marshals `Work` to the pump thread,
    // blocks until it has executed there, and reports its HRESULT via `Result`.
    // Returns true if `Work` was executed (`Result` is set); returns false if the
    // pump is no longer running (the notification already returned), in which case
    // `Work` was NOT run and the caller must run it itself. Reporting "not run"
    // out-of-band (rather than via a sentinel HRESULT) lets `Work` legitimately
    // return any HRESULT, including RPC_E_DISCONNECTED, without ambiguity.
    bool Invoke(std::function<HRESULT()> Work, _Out_ HRESULT& Result);

private:
    struct Call
    {
        std::function<HRESULT()> work;
        HRESULT result{E_FAIL};
        wil::unique_event_nothrow done;
    };

    // Runs every currently-queued call on the calling (pump) thread.
    void DrainQueue();

    wil::srwlock m_lock;
    _Guarded_by_(m_lock) std::deque<Call*> m_queue;
    _Guarded_by_(m_lock) bool m_stopped { false };

    // Auto-reset event signaled whenever a call is enqueued.
    wil::unique_event m_callAvailable{wil::EventOptions::None};
};

} // namespace wsl::windows::service
