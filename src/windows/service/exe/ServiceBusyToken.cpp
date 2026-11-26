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

    if (s_busyEvent)
    {
        return;
    }

    auto [serviceSid, serviceSidBuffer] = wsl::windows::common::security::GetAccountSid(L"NT SERVICE\\WslService");
    wil::unique_hlocal_string serviceSidString;
    THROW_IF_WIN32_BOOL_FALSE(ConvertSidToStringSidW(serviceSid, &serviceSidString));

    const auto sddl = std::format(
        // Owner: WslService
        L"O:{0}"
        // DACL is protected
        L"D:P"
        // 0x100000 = Synchronize, 0x020000 = Read control, 0x000002 = Modify state
        // Allow WslService synchronize, read-control, and modify-state
        L"(A;;0x120002;;;{0})"
        // Allow Builtin Administrators synchronize and read-control
        L"(A;;0x120000;;;BA)",
        serviceSidString.get());
    wil::unique_hlocal_security_descriptor descriptor;
    THROW_IF_WIN32_BOOL_FALSE(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr));

    SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor.get(), FALSE};
    wil::unique_event event;
    event.reset(CreateEventExW(
        &attributes, wsl::windows::common::install::c_serviceBusyEventName, CREATE_EVENT_MANUAL_RESET, SYNCHRONIZE | READ_CONTROL | EVENT_MODIFY_STATE));
    THROW_LAST_ERROR_IF(!event);

    // Reject an event not owned by the service.
    THROW_HR_IF(E_ACCESSDENIED, !wsl::windows::common::install::IsOwnedByService(event.get()));

    // An existing, abandoned event may need to be reset
    event.ResetEvent();

    s_busyEvent = std::move(event);
}
CATCH_LOG()

ServiceBusyToken::ServiceBusyToken() noexcept
{
    try
    {
        auto lock = s_lock.lock_exclusive();

        // Signal the event on the first workload
        if (s_busyEvent && ++s_count == 1)
        {
            s_busyEvent.SetEvent();
        }
    }
    CATCH_LOG()
}

ServiceBusyToken::~ServiceBusyToken()
{
    auto lock = s_lock.lock_exclusive();

    // Reset the event if there are no more workloads
    if (s_busyEvent && --s_count == 0)
    {
        s_busyEvent.ResetEvent();
    }
}

} // namespace wsl::windows::service
