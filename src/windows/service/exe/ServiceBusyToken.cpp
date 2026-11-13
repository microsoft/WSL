// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "ServiceBusyToken.h"
#include "InstallGuard.h"
#include <stop_token>

namespace wsl::windows::service {

// Reporting that the service is busy is not strictly necessary.
// If anything fails here, we proceed normally.
void ServiceBusyToken::Initialize() noexcept
try
{
    auto lock = s_lock.lock_exclusive();

    // Prevent unprivileged callers from acquiring the mutex
    constexpr PCWSTR c_MutexSDDL =
        // Owner: System
        L"O:SY"
        // Group: System
        L"G:SY"
        // DACL is Protected
        L"D:P"
        // Allow full access (GA) to System
        L"(A;;GA;;;SY)"
        // Allow Builtin Administrators synchronize (0x00100000) and read-control (0x00020000)
        L"(A;;0x120000;;;BA)";
    wil::unique_hlocal_security_descriptor descriptor;
    THROW_IF_WIN32_BOOL_FALSE(ConvertStringSecurityDescriptorToSecurityDescriptorW(c_MutexSDDL, SDDL_REVISION_1, &descriptor, nullptr));

    SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor.get(), FALSE};
    wil::unique_mutex mutex;
    mutex.create(wsl::windows::common::install::c_serviceBusyMutexName, 0, MUTEX_ALL_ACCESS, &attributes);

    // Avoid using an existing mutex created by an unprivileged process.
    // This is best-effort only, it does not guarantee that it was created by the service.
    THROW_HR_IF(E_ACCESSDENIED, !wsl::windows::common::install::IsOwnedBySystem(mutex.get()));

    s_busyMutex = std::move(mutex);
}
CATCH_LOG()

ServiceBusyToken::ServiceBusyToken() noexcept
{
    try
    {
        auto lock = s_lock.lock_exclusive();

        // Acquire the mutex on the first workload
        if (s_busyMutex && ++s_count == 1)
        {
            wil::unique_event startupCompleted{wil::EventOptions::None};

            s_mutexThread = std::jthread([mutexHandle = s_busyMutex.get(), startupEvent = startupCompleted.get()](std::stop_token stop) {
                auto notifyStartup = wil::scope_exit([=]() { LOG_IF_WIN32_BOOL_FALSE(SetEvent(startupEvent)); });
                try
                {
                    wil::unique_event stopRequested{wil::EventOptions::None};
                    std::stop_callback stopCallback{stop, [&]() { stopRequested.SetEvent(); }};

                    const auto result = WaitForSingleObject(mutexHandle, INFINITE);
                    THROW_LAST_ERROR_IF(result == WAIT_FAILED);
                    THROW_HR_IF(E_UNEXPECTED, result != WAIT_OBJECT_0 && result != WAIT_ABANDONED);
                    auto releaseMutex = wil::scope_exit([=]() { LOG_IF_WIN32_BOOL_FALSE(ReleaseMutex(mutexHandle)); });

                    notifyStartup.reset();
                    stopRequested.wait();
                }
                CATCH_LOG()
            });

            // Do not allow the first workload to start before the owner thread is ready.
            startupCompleted.wait();
        }
    }
    CATCH_LOG()
}

ServiceBusyToken::~ServiceBusyToken()
{
    auto lock = s_lock.lock_exclusive();

    // Release the mutex if there are no more workloads
    if (s_busyMutex && --s_count == 0)
    {
        // Request stop and join under the count lock before a new busy period can begin.
        s_mutexThread = {};
    }
}

} // namespace wsl::windows::service
