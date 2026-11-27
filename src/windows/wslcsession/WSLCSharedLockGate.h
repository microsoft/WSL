// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include <atomic>

namespace wsl::windows::service::wslc {

// Takes Lock shared, failing with ERROR_INVALID_STATE once terminating, even if the lock is free.
inline wil::rwlock_release_shared_scope_exit LockSharedUnlessTerminating(
    wil::srwlock& Lock, const wil::shared_event& TerminatingEvent, DWORD RetryMs, const std::atomic<bool>& Terminating)
{
    for (;;)
    {
        auto lock = Lock.try_lock_shared();
        THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), Terminating.load());
        if (lock)
        {
            return lock;
        }

        TerminatingEvent.wait(RetryMs);
    }
}

} // namespace wsl::windows::service::wslc
