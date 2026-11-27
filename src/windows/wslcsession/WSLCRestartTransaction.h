// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include <memory>
#include <wil/resource.h>

namespace wsl::windows::service::wslc {

// The caller serializes transaction ownership and cleanup with the container lock.
struct WSLCRestartTransaction
{
    template <typename FailureCleanup>
    void CleanupIfOwned(std::shared_ptr<WSLCRestartTransaction>& owner, bool succeeded, FailureCleanup&& failureCleanup)
    {
        if (owner.get() != this)
        {
            return;
        }

        owner.reset();
        if (!succeeded)
        {
            failureCleanup();
        }
    }

    wil::unique_event Completed{wil::EventOptions::ManualReset};
};

} // namespace wsl::windows::service::wslc
