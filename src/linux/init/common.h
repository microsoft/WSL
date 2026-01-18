/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    common.h

Abstract:

    This file contains common/shared information.

--*/

#pragma once

#ifndef TEMP_FAILURE_RETRY
#define TEMP_FAILURE_RETRY(expression) \
    (__extension__({ \
        long int __result; \
        do \
            __result = (long int)(expression); \
        while (__result == -1L && errno == EINTR); \
        __result; \
    }))
#endif

#include <stdlib.h>
#include <stdio.h>
#include <stddef.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sched.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/reboot.h>
#include <sys/un.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <poll.h>
#include <utility>
#include <format>
#include <new>
#include <thread>
#include <vector>
#include <gsl/gsl>
#include <gslhelpers.h>
#include <lxwil.h>
#include <cstdarg>
#include "lxinitshared.h"
#include "defs.h"

#define ETC_FOLDER "/etc/"
#define NAME_ENV "NAME"
#define INIT_PATH "/sbin/init"
#define INTEROP_TIMEOUT_MS (INTEROP_TIMEOUT_SEC * 1000)
#define SESSION_LEADER_ACCEPT_TIMEOUT_MS (30 * 1000)
#define INTEROP_TIMEOUT_SEC (10)
#define RUN_FOLDER "/run"
#define WSL_SAFE_MODE_WARNING "SAFE MODE ENABLED"
#define CONFIG_FILE ETC_FOLDER "wsl.conf"

extern thread_local std::string g_threadName;
extern int g_LogFd;
extern int g_TelemetryFd;
extern struct sigaction g_SavedSignalActions[_NSIG];

//
// N.B. The clone syscall is used directly instead of the libc wrapper, because
//      it allows execution to continue at the point of the call with a
//      copy-on-write stack.
//
// The clone syscall argument are architecture-specific:
//     x86, arm, arm64: clone(flags, stack, ptid, tls, ctid)
//     amd64:           clone(flags, stack, ptid, ctid, tls)
//

#if defined(__i386__) || defined(__arm__) || defined(__aarch64__)
#define CLONE(_flags) syscall(SYS_clone, (_flags), NULL, NULL, NULL, NULL);
#elif defined(__x86_64__)
#define CLONE(_flags) syscall(SYS_clone, (_flags), NULL, NULL, NULL, NULL);
#else
#error CLONE function signature is architecture-specific.
#endif

#define COUNT_OF(x) (sizeof(x) / sizeof((x)[0]))

#define CLOSE(_fd) \
    { \
        if (_fd != -1 && close(_fd) < 0) \
        { \
            FATAL_ERROR("close({}) {}", _fd, errno); \
        } \
    }

template <typename... Args>
auto LogImpl(int fd, const std::format_string<Args...>& format, Args&&... args)
{
    auto logline = std::format(format, std::forward<Args>(args)...);
    if (logline.empty())
    {
        return;
    }

    if (logline.back() != '\n')
    {
        logline.push_back('\n');
    }

    write(fd, logline.c_str(), logline.size());
}

#define LOG_ERROR(str, ...) \
    { \
        LogImpl(g_LogFd, "<3>WSL ({} - {}) ERROR: {}:{}: " str "\n", getpid(), g_threadName.c_str(), __FUNCTION__, __LINE__, ##__VA_ARGS__); \
    }

#define LOG_INFO(str, ...) \
    { \
        LogImpl(g_LogFd, "<6>WSL ({} - {}): " str "\n", getpid(), g_threadName.c_str(), ##__VA_ARGS__); \
    }

#define LOG_WARNING(str, ...) \
    { \
        LogImpl(g_LogFd, "<4>WSL ({} - {}) WARNING: " str "\n", getpid(), g_threadName.c_str(), ##__VA_ARGS__); \
    }

#define FATAL_ERROR_EX(status, str, ...) \
    { \
        LOG_ERROR(str, ##__VA_ARGS__); \
        _exit(status); \
    }

#define GNS_LOG_INFO(str, ...) \
    { \
        LogImpl(g_TelemetryFd, "{}: {} - " str "\n", g_threadName.c_str(), __FUNCTION__, ##__VA_ARGS__); \
    }

#define GNS_LOG_ERROR(str, ...) \
    { \
        LogImpl(g_TelemetryFd, "{}: {} - ERROR: " str "\n", g_threadName.c_str(), __FUNCTION__, ##__VA_ARGS__); \
    }

#define FATAL_ERROR(str, ...) FATAL_ERROR_EX(1, str, ##__VA_ARGS__)

// Some of these files need the LOG_* macros.
#include "retryshared.h"
#include "socketshared.h"
#include "stringshared.h"

int InitializeLogging(bool SetStderr, wil::LogFunction* ExceptionCallback = nullptr) noexcept;

void LogException(const char* Message, const char* Description) noexcept;
