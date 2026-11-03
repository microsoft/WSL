// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include "ExecutionContext.h"
#include "Localization.h"

namespace wsl::windows::service::wslc {

inline void CheckGuestHostsFileWriteResult(int Result)
{
    if (Result != 0)
    {
        THROW_HR_WITH_USER_ERROR(E_FAIL, wsl::shared::Localization::MessageWslcLocalhostWriteFailed("/etc/hosts", Result));
    }
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
