// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include "ExecutionContext.h"
#include "Localization.h"
#include "lxinitshared.h"

namespace wsl::windows::service::wslc {

inline void CheckGuestMountResult(const WSLC_MOUNT_RESULT& Response)
{
    if (Response.Result == 0)
    {
        return;
    }

    switch (Response.LocalhostFailure)
    {
    case WSLC_MOUNT_RESULT::LocalhostConfigurationFailure::Hosts:
        THROW_HR_WITH_USER_ERROR(E_FAIL, wsl::shared::Localization::MessageWslcLocalhostInitFailed("/etc/hosts", Response.Result));
        break;

    case WSLC_MOUNT_RESULT::LocalhostConfigurationFailure::NameServiceSwitch:
        THROW_HR_WITH_USER_ERROR(E_FAIL, wsl::shared::Localization::MessageWslcLocalhostInitFailed("/etc/nsswitch.conf", Response.Result));
        break;

    case WSLC_MOUNT_RESULT::LocalhostConfigurationFailure::None:
        THROW_HR(E_FAIL);
        break;
    }

    THROW_HR_MSG(E_UNEXPECTED, "Unexpected localhost configuration failure: %u", static_cast<uint32_t>(Response.LocalhostFailure));
}

template <typename GetExitCode>
inline void WaitForDockerStartup(HANDLE Ready, HANDLE Exited, HANDLE Terminating, DWORD TimeoutMs, GetExitCode&& GetProcessExitCode)
{
    const HANDLE handles[] = {Terminating, Exited, Ready};
    const auto result = WaitForMultipleObjects(RTL_NUMBER_OF(handles), handles, FALSE, TimeoutMs);
    THROW_LAST_ERROR_IF(result == WAIT_FAILED);

    switch (result)
    {
    case WAIT_OBJECT_0:
        THROW_HR_MSG(E_ABORT, "Session terminated while waiting for Docker Engine to start");
        break;

    case WAIT_OBJECT_0 + 1:
        THROW_HR_WITH_USER_ERROR(E_FAIL, wsl::shared::Localization::MessageWslcDockerExitedBeforeReady(GetProcessExitCode()));
        break;

    case WAIT_OBJECT_0 + 2:
        return;

    case WAIT_TIMEOUT:
        THROW_HR_WITH_USER_ERROR(HRESULT_FROM_WIN32(ERROR_TIMEOUT), wsl::shared::Localization::MessageWslcDockerStartupTimedOut(TimeoutMs));
        break;

    default:
        THROW_HR_MSG(E_UNEXPECTED, "Unexpected Docker Engine startup wait result: %lu", result);
    }
}

} // namespace wsl::windows::service::wslc
