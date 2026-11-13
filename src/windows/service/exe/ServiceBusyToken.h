// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include <cstddef>
#include <thread>
#include <wil/resource.h>
#include "defs.h"

namespace wsl::windows::service {

// The lifetime of this token indicates that the service is busy and should not be stopped.
// Must only be used from the wslservice.exe process.
class ServiceBusyToken
{
public:
    NON_COPYABLE(ServiceBusyToken);
    NON_MOVABLE(ServiceBusyToken);

    ServiceBusyToken() noexcept;
    ~ServiceBusyToken();

    static void Initialize() noexcept;

private:
    static inline wil::srwlock s_lock;
    _Guarded_by_(s_lock) static inline size_t s_count {};
    _Guarded_by_(s_lock) static inline wil::unique_mutex s_busyMutex;

    // The mutex needs to be acquired and released from the same thread
    _Guarded_by_(s_lock) static inline std::jthread s_mutexThread;
};

} // namespace wsl::windows::service
