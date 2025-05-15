// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include <functional>
#include <map>
#include <functional>
#include <stdio.h>
#include <asm/types.h>
#include <errno.h>
#include "common.h"
#include "util.h"
#include <linux/seccomp.h>

class SecCompDispatcher
{
public:
    SecCompDispatcher(int SecCompFd);
    ~SecCompDispatcher();

    SecCompDispatcher(const SecCompDispatcher&) = delete;
    SecCompDispatcher(SecCompDispatcher&&) = delete;
    SecCompDispatcher& operator=(const SecCompDispatcher&) = delete;
    SecCompDispatcher& operator=(SecCompDispatcher&&) = delete;

    void RegisterHandler(int SysCallNr, const std::function<int(seccomp_notif*)>& Handler);
    void UnregisterHandler(int SysCallNr);

    bool ValidateCookie(uint64_t id) noexcept;

    std::optional<std::vector<gsl::byte>> ReadProcessMemory(uint64_t cookie, pid_t pid, size_t address, size_t length) noexcept;

private:
    void Run();

    seccomp_notif_sizes m_notificationSizes;
    std::map<int, std::function<int(seccomp_notif*)>> m_handlers;
    wil::unique_fd m_notifyFd;
    wil::unique_fd m_shutdown;
    std::thread m_worker;
};
