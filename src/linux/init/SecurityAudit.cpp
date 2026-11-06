// Copyright (C) Microsoft Corporation. All rights reserved.

#include "common.h"
#include "util.h"
#include "SocketChannel.h"
#include <arpa/inet.h>
#include <atomic>
#include <netpacket/packet.h>
#include <poll.h>
#include <sys/mount.h>
#include <sys/vfs.h>
#include <linux/types.h>
#include "ebpf_plugin_loader.h"
#include "ebpf_arch_macros.h"

namespace {
struct AuditContext
{
    wsl::shared::SocketChannel channel;
    std::mutex lock;
    std::atomic<bool> failed{};
};

void ProcessEvent(void* context, int, void* data, unsigned int size)
try
{
    auto& audit = *static_cast<AuditContext*>(context);
    THROW_ERRNO_IF(EINVAL, !data || size < sizeof(event_begin_s));
    event_begin_s header{};
    memcpy(&header, data, sizeof(header));
    THROW_ERRNO_IF(EINVAL, header.code_bytes_start != CODE_BYTES || header.version != VERSION || header.size != size);
    if (header.event_type != event_type_exec_process)
    {
        return;
    }
    THROW_ERRNO_IF(EINVAL, size != sizeof(event_exec_process_s));
    event_exec_process_s event{};
    memcpy(&event, data, sizeof(event));
    THROW_ERRNO_IF(EINVAL, event.end.code_bytes_end != CODE_BYTES || event.base.pid == 0);

    LX_INIT_PROCESS_EXECUTED message{};
    message.Header.MessageType = message.Type;
    message.Header.MessageSize = sizeof(message);
    message.Pid = event.base.pid;
    std::lock_guard lock(audit.lock);
    audit.channel.SendMessage(message);
}
catch (...)
{
    LOG_CAUGHT_EXCEPTION();
    static_cast<AuditContext*>(context)->failed = true;
}

void EventsLost(void*, int, unsigned long long count)
{
    LOG_ERROR("Security audit lost {} events", count);
}

void PollError(void* context, int error)
{
    LOG_ERROR("Security audit polling failed: {}", error);
    static_cast<AuditContext*>(context)->failed = true;
}
} // namespace

void RunSecurityAudit(int socket)
{
    AuditContext audit{wsl::shared::SocketChannel{wil::unique_fd{socket}, "SecurityAudit"}};
    THROW_LAST_ERROR_IF(signal(SIGPIPE, SIG_IGN) == SIG_ERR);
    // Keep mount changes local to this worker, independent of mini-init and distributions.
    THROW_LAST_ERROR_IF(unshare(CLONE_NEWNS) < 0);
    THROW_LAST_ERROR_IF(mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) < 0);
    struct statfs filesystem{};
    THROW_LAST_ERROR_IF(statfs("/sys/kernel/tracing", &filesystem) < 0);
    if (filesystem.f_type != 0x74726163)
    {
        THROW_LAST_ERROR_IF(mount("tracefs", "/sys/kernel/tracing", "tracefs", MS_NOSUID | MS_NODEV | MS_NOEXEC, nullptr) < 0);
    }
    THROW_LAST_ERROR_IF(access("/sys/kernel/btf/vmlinux", R_OK) < 0);
    const timeval sendTimeout{2, 0};
    THROW_LAST_ERROR_IF(setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &sendTimeout, sizeof(sendTimeout)) < 0);

    auto cleanup = wil::scope_exit([&] {
        // Unblock a callback before closing the SDK, which joins the polling thread.
        if (shutdown(socket, SHUT_RDWR) < 0 && errno != ENOTCONN)
        {
            LOG_ERROR("Security audit socket shutdown failed: {}", errno);
        }
        const auto result = ebpf_plugin_close();
        if (result != no_error && result != error_not_initialized)
        {
            LOG_ERROR("Security audit shutdown failed: {}", result);
        }
    });
    std::array<char, 8192> error{};
    const syscall_config syscalls[] = {{__NR_execve, true}};
    const auto info = ebpf_plugin_start(
        "/wsl-security-audit",
        PERF_FEATURES_BTF | FEATURE_RINGBUFF | FEATURE_ENRICHMENT_AT_SOURCE | FEATURE_RUNTIME_MAP_FLIGHTING,
        FEATURE_SYSCALL_EXTENDED_DATA,
        syscalls,
        1,
        nullptr,
        0,
        (8 * 1024 * 1024) / 512,
        nullptr,
        0,
        {},
        error.data(),
        error.size());
    if (info.return_code != no_error || info.hooks_attach_code != no_error)
    {
        LOG_ERROR("eBPF startup failed: {}, hooks {}, {}", info.return_code, info.hooks_attach_code, error.data());
        THROW_ERRNO(EIO);
    }
    const auto result = ebpf_plugin_start_polling(&audit, ProcessEvent, EventsLost, PollError, error.data(), error.size(), 64, 8);
    if (result != no_error)
    {
        LOG_ERROR("eBPF polling startup failed: {}, {}", result, error.data());
        THROW_ERRNO(EIO);
    }
    LOG_INFO("Security audit collecting ProcessExecuted PID");

    while (!audit.failed)
    {
        pollfd descriptor{socket, POLLIN, 0};
        const auto ready = TEMP_FAILURE_RETRY(poll(&descriptor, 1, 250));
        THROW_LAST_ERROR_IF(ready < 0);
        if (ready != 0)
        {
            break;
        }
    }
}
