/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WslActivityMarker.cpp

Abstract:

    This file contains the implementation for tracking whether WSL is in use.

--*/

#include "precomp.h"
#include "WslActivityMarker.h"

namespace {

constexpr auto c_activityObjectName = L"Global\\WslActive";

wil::srwlock g_activityLock;
_Guarded_by_(g_activityLock) size_t g_activityCount = 0;
_Guarded_by_(g_activityLock) wil::unique_handle g_activityEvent;

} // namespace

namespace wsl::windows::common {

WslActivityMarker::WslActivityMarker() noexcept
{
    auto lock = g_activityLock.lock_exclusive();

    g_activityCount++;

    if (!g_activityEvent)
    {
        g_activityEvent.reset(CreateEventW(nullptr, TRUE, FALSE, c_activityObjectName));
        LOG_LAST_ERROR_IF_MSG(!g_activityEvent, "Failed to create WSL activity object");
    }
}

WslActivityMarker::~WslActivityMarker() noexcept
{
    auto lock = g_activityLock.lock_exclusive();

    g_activityCount--;
    if (g_activityCount == 0)
    {
        g_activityEvent.reset();
    }
}

bool WslActivityMarker::IsWslActive() noexcept
{
    wil::unique_handle event{OpenEventW(SYNCHRONIZE, FALSE, c_activityObjectName)};
    return event != nullptr;
}

} // namespace wsl::windows::common
