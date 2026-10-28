// Copyright (C) Microsoft Corporation. All rights reserved.

#include "SecurityAuditLaunch.h"
#include "common.h"
#include "util.h"
#include "Launch.h"
#include "../securityaudit/Collector.h"

void StartSecurityAuditCollector() noexcept
try
{
    const auto* setting = getenv(WSL_SECURITY_AUDIT_ENV);
    if (!setting)
    {
        return;
    }
    const bool enabled = strcmp(setting, "1") == 0;
    const bool valid = enabled || strcmp(setting, "0") == 0;
    THROW_LAST_ERROR_IF(unsetenv(WSL_SECURITY_AUDIT_ENV) < 0);
    THROW_ERRNO_IF(EINVAL, !valid);
    if (!enabled)
    {
        return;
    }
    THROW_LAST_ERROR_IF(UtilCreateChildProcess("wsl-audit", [] {
                            THROW_LAST_ERROR_IF(UtilRestoreBlockedSignals() < 0);
                            THROW_LAST_ERROR_IF(dup2(g_LogFd, STDERR_FILENO) < 0);
                            THROW_LAST_ERROR_IF(fcntl(STDERR_FILENO, F_SETFD, 0) < 0);
                            // Exec gives the static SDK fresh process state instead of inheriting init's globals.
                            execl(LX_INIT_PATH, wsl::audit::c_nativeAuditCollectorName, nullptr);
                            THROW_LAST_ERROR();
                        }) < 0);
}
CATCH_LOG()

int RunSecurityAuditCollector() noexcept
try
{
    auto socket = UtilConnectVsock(wsl::audit::c_nativeAuditPort, true);
    THROW_LAST_ERROR_IF(!socket);
    return wsl::audit::RunCollector(socket.get(), socket.get());
}
CATCH_RETURN_ERRNO()
