// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include <signal.h>

inline int IgnoreSecurityAuditListenerSigpipe() noexcept
{
    struct sigaction action{};
    action.sa_handler = SIG_IGN;
    if (sigemptyset(&action.sa_mask) < 0)
    {
        return -1;
    }
    return sigaction(SIGPIPE, &action, nullptr);
}

void StartSecurityAuditListener() noexcept;
