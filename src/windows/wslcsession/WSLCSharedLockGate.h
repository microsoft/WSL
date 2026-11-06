// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

namespace wsl::windows::service::wslc {

// Takes Lock shared, failing with ERROR_INVALID_STATE once terminating, even if the lock is free.
template <typename TIsTerminating>
wil::rwlock_release_shared_scope_exit LockSharedUnlessTerminating(
    wil::srwlock& Lock, const wil::shared_event& TerminatingEvent, DWORD RetryMs, TIsTerminating&& IsTerminating)
{
    for (;;)
    {
        auto lock = Lock.try_lock_shared();
        THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), IsTerminating());
        if (lock)
        {
            return lock;
        }

        TerminatingEvent.wait(RetryMs);
    }
}

} // namespace wsl::windows::service::wslc
