// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "PluginCallPump.h"
#include <thread>

using wsl::windows::service::PluginCallPump;

PluginCallPump::PluginCallPump() = default;

void PluginCallPump::DrainQueue()
{
    for (;;)
    {
        Call* call = nullptr;
        {
            auto lock = m_lock.lock_exclusive();
            if (m_queue.empty())
            {
                break;
            }
            call = m_queue.front();
            m_queue.pop_front();
        }

        // Run the plugin's service-side work on the pump (notification) thread,
        // which still holds the notifying lock — recursive locks re-enter here.
        // The closures already translate exceptions to HRESULTs via CATCH_RETURN,
        // but guard defensively so a throw can never skip done.SetEvent() and
        // strand the waiting RPC thread.
        try
        {
            call->result = call->work ? call->work() : E_UNEXPECTED;
        }
        catch (...)
        {
            call->result = wil::ResultFromCaughtException();
        }

        call->done.SetEvent();
    }
}

HRESULT PluginCallPump::Run(const std::function<HRESULT()>& Notification)
try
{
    HRESULT notificationResult = E_FAIL;

    // The worker makes the outbound cross-process notification call. It runs on
    // its own thread so THIS thread is free to pump the plugin's callbacks while
    // the notification is in flight. (Thread creation can throw std::system_error
    // under resource exhaustion; the surrounding try/CATCH_RETURN converts that to
    // an HRESULT so it never escapes into a non-throwing teardown notification.)
    wil::unique_event workerDone(wil::EventOptions::ManualReset);
    std::thread worker([&]() {
        // Guard defensively: a throw escaping the thread's top-level function
        // would call std::terminate() and crash the service, and would skip
        // workerDone.SetEvent() — stranding the pumping thread. Translate to an
        // HRESULT and always signal completion (mirrors DrainQueue).
        try
        {
            notificationResult = Notification();
        }
        catch (...)
        {
            notificationResult = wil::ResultFromCaughtException();
        }

        workerDone.SetEvent();
    });

    auto join = wil::scope_exit([&]() {
        if (worker.joinable())
        {
            worker.join();
        }
    });

    const HANDLE waits[] = {m_callAvailable.get(), workerDone.get()};
    for (;;)
    {
        const DWORD wait = ::WaitForMultipleObjects(ARRAYSIZE(waits), waits, FALSE, INFINITE);

        // A kernel wait failure must never spin this thread. Stop the pump and
        // fail any queued/future calls so their RPC threads aren't stranded.
        FAIL_FAST_LAST_ERROR_IF(wait == WAIT_FAILED);

        // Drain regardless of which handle woke us: the auto-reset event
        // coalesces multiple enqueues into a single signal, and a final call may
        // race in just as the worker completes.
        DrainQueue();

        // Check worker completion independently of the wait result.
        // WaitForMultipleObjects reports the LOWEST signaled index, so a steady
        // stream of callbacks on m_callAvailable (index 0) would otherwise starve
        // the workerDone (index 1) branch and keep this thread pumping (and the
        // notifying lock held) forever. workerDone is manual-reset, so this is a
        // cheap non-consuming poll.
        if (::WaitForSingleObject(workerDone.get(), 0) == WAIT_OBJECT_0)
        {
            // Worker (notification) finished. Stop accepting further work and
            // fail any call that raced in after this point so its RPC thread is
            // never stranded, then drain whatever was already queued.
            {
                auto lock = m_lock.lock_exclusive();
                m_stopped = true;
            }
            DrainQueue();
            break;
        }
    }

    // Join the worker before reading notificationResult: thread::join() is a C++
    // synchronization point, establishing happens-before for the worker's write
    // (the workerDone event alone is not a synchronization op under the C++ memory
    // model). The scope_exit above remains the join for the exception path; its
    // joinable() guard makes it a no-op once we have joined here.
    worker.join();

    return notificationResult;
}
CATCH_RETURN()

bool PluginCallPump::Invoke(std::function<HRESULT()> Work, _Out_ HRESULT& Result)
{
    Call call;
    call.work = std::move(Work);

    const HRESULT createHr = call.done.create();
    if (FAILED(createHr))
    {
        // Could not create the completion event (resource exhaustion). Surface
        // the failure as the executed result rather than reporting "not run":
        // retrying on the direct path would not help and risks double-execution.
        Result = createHr;
        return true;
    }

    {
        auto lock = m_lock.lock_exclusive();
        if (m_stopped)
        {
            // The notification already returned; there is no pump thread left to
            // run this. Report "not run" so the caller executes it directly.
            return false;
        }
        m_queue.push_back(&call);
    }

    m_callAvailable.SetEvent();
    call.done.wait();
    Result = call.result;
    return true;
}
