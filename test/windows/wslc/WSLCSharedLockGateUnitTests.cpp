// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "Common.h"
#include "../../../src/windows/wslcsession/WSLCSharedLockGate.h"
#include "retryshared.h"

namespace WSLCSharedLockGate {

using wsl::windows::service::wslc::LockSharedUnlessTerminating;

HRESULT TryLockShared(wil::srwlock& Lock, const wil::shared_event& TerminatingEvent, const std::atomic<bool>& Terminating)
try
{
    auto lock = LockSharedUnlessTerminating(Lock, TerminatingEvent, 10, Terminating);

    return lock ? S_OK : E_UNEXPECTED;
}
CATCH_RETURN();

void WaitForLockAttempt(const wil::shared_event& TerminatingEvent)
{
    // A missing initial auto-reset signal proves the worker reached the retry wait.
    wsl::shared::retry::RetryWithTimeout<void>(
        [&]() {
            if (TerminatingEvent.wait(0))
            {
                TerminatingEvent.SetEvent();
                THROW_HR(E_PENDING);
            }
        },
        std::chrono::milliseconds(1),
        std::chrono::seconds(30));
}

class WSLCSharedLockGateUnitTests
{
    WSLC_TEST_CLASS(WSLCSharedLockGateUnitTests)

    TEST_METHOD(AcquiresFreeLock)
    {
        wil::srwlock lock;
        wil::shared_event terminatingEvent(wil::EventOptions::ManualReset);
        std::atomic<bool> terminating{false};

        auto shared = LockSharedUnlessTerminating(lock, terminatingEvent, 10, terminating);
        VERIFY_IS_TRUE(static_cast<bool>(shared));
        VERIFY_IS_FALSE(static_cast<bool>(lock.try_lock_exclusive()));
    }

    TEST_METHOD(FailsOnFreeLockWhenTerminating)
    {
        wil::srwlock lock;
        wil::shared_event terminatingEvent(wil::EventOptions::ManualReset);
        std::atomic<bool> terminating{true};

        VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), TryLockShared(lock, terminatingEvent, terminating));
        VERIFY_IS_TRUE(static_cast<bool>(lock.try_lock_exclusive()));
    }

    TEST_METHOD(WaitsForExclusiveHolderWhenNotTerminating)
    {
        wil::srwlock lock;
        wil::shared_event terminatingEvent(wil::EventOptions::Signaled);
        std::atomic<bool> terminating{false};

        auto exclusive = lock.lock_exclusive();
        std::atomic<HRESULT> result{E_PENDING};
        std::thread acquirer([&]() { result = TryLockShared(lock, terminatingEvent, terminating); });
        auto joinThread = wil::scope_exit([&]() {
            exclusive.reset();
            acquirer.join();
        });

        VERIFY_NO_THROW(WaitForLockAttempt(terminatingEvent));
        VERIFY_ARE_EQUAL(E_PENDING, result.load());
        joinThread.reset();
        VERIFY_ARE_EQUAL(S_OK, result.load());
    }

    // Termination must interrupt a caller that saw it unset and is now waiting behind the exclusive holder.
    TEST_METHOD(FailsWhenTerminationStartsWhileWaiting)
    {
        wil::srwlock lock;
        wil::shared_event terminatingEvent(wil::EventOptions::Signaled);
        std::atomic<bool> terminating{false};
        wil::slim_event_manual_reset done;

        auto exclusive = lock.lock_exclusive();
        std::atomic<HRESULT> result{E_PENDING};
        std::thread acquirer([&]() {
            result = TryLockShared(lock, terminatingEvent, terminating);
            done.SetEvent();
        });
        auto joinThread = wil::scope_exit([&]() {
            exclusive.reset();
            acquirer.join();
        });

        VERIFY_NO_THROW(WaitForLockAttempt(terminatingEvent));
        terminating = true;
        terminatingEvent.SetEvent();

        // The exclusive holder stays until the caller returns, like a teardown waiting on a plugin callback.
        const bool completed = done.wait(30 * 1000);
        joinThread.reset();
        VERIFY_IS_TRUE(completed);
        VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), result.load());
    }
};

} // namespace WSLCSharedLockGate
