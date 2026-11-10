// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include "defs.h"
#include <vector>
#include <wil/resource.h>

namespace wsl::windows::service::wslc {

// Cancellation sources are retained for the incoming call, including its original caller process.
class ComposeCancellation
{
public:
    NON_COPYABLE(ComposeCancellation);
    NON_MOVABLE(ComposeCancellation);

    ComposeCancellation(HANDLE sessionTerminatingEvent, HANDLE cancelEvent);
    void Check() const;
    wil::mutex_release_scope_exit AcquireMutex(HANDLE mutex) const;

private:
    wil::unique_handle m_callerProcess;
    std::vector<HANDLE> m_handles;
};

} // namespace wsl::windows::service::wslc
