// Copyright (C) Microsoft Corporation. All rights reserved.

#include "common.h"
#include "util.h"
#include "SocketChannel.h"
#include "securityaudit/ProcessCreationAudit.h"
#include "securityaudit/ProcessExecutionAudit.h"
#include "securityaudit/NetworkAudit.h"
#include "securityaudit/SystemAudit.h"
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
    bool includeCommandLine = false;
};

void ProcessEvent(void* context, int, void* data, unsigned int size)
{
    auto& audit = *static_cast<AuditContext*>(context);
    std::unique_lock lock(audit.lock, std::defer_lock);
    try
    {
        lock.lock();
        if (audit.failed)
        {
            return;
        }

        THROW_ERRNO_IF(EINVAL, !data || size < sizeof(event_begin_s));
        event_begin_s header{};
        memcpy(&header, data, sizeof(header));
        THROW_ERRNO_IF(EINVAL, header.code_bytes_start != CODE_BYTES || header.version != VERSION || header.size != size);
        if (header.event_type == event_type_fork_process)
        {
            auto message =
                wsl::linux::securityaudit::DecodeProcessCreation(gsl::span<const gsl::byte>{static_cast<const gsl::byte*>(data), size});
            if (message)
            {
                audit.channel.SendMessage(*message);
            }
            return;
        }
        if (header.event_type != event_type_exec_process)
        {
            const auto bytes = gsl::span<const gsl::byte>{static_cast<const gsl::byte*>(data), size};
            if (auto message = wsl::linux::securityaudit::DecodeNetworkAudit(bytes))
            {
                audit.channel.SendMessage(*message);
            }
            else if (auto message = wsl::linux::securityaudit::DecodeSystemAudit(bytes))
            {
                audit.channel.SendMessage(*message);
            }
            return;
        }
        auto message = wsl::linux::securityaudit::DecodeProcessExecution(
            gsl::span<const gsl::byte>{static_cast<const gsl::byte*>(data), size}, audit.includeCommandLine);
        if (message)
        {
            audit.channel.SendMessage<LX_INIT_PROCESS_EXECUTED_DETAILS>(message->Span());
        }
    }
    catch (...)
    {
        audit.failed = true;
        LOG_CAUGHT_EXCEPTION();
    }
}

void EventsLost(void*, int, unsigned long long count)
{
    LOG_ERROR("Security audit lost {} events", count);
}

void PollError(void* context, int error)
{
    static_cast<AuditContext*>(context)->failed = true;
    LOG_ERROR("Security audit polling failed: {}", error);
}
} // namespace

int RunSecurityAudit(int socket, bool includeCommandLine)
{
    AuditContext audit{wsl::shared::SocketChannel{wil::unique_fd{socket}, "SecurityAudit"}};
    audit.includeCommandLine = includeCommandLine;
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
    auto cleanup = wil::scope_exit([&] {
        // Sends remain blocking to tolerate backpressure. Unblock callbacks before the SDK joins its polling thread.
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
    // The SDK's fork selector controls task_newtask for clone/fork/vfork/clone3, excluding CLONE_THREAD.
    const syscall_config syscalls[] = {
        {__NR_execve, true},   {__NR_fork, true},           {__NR_socket, false},        {__NR_connect, false},
        {__NR_bind, false},    {__NR_listen, false},        {__NR_accept, false},        {__NR_accept4, false},
        {__NR_sendmsg, false}, {__NR_init_module, false},   {__NR_finit_module, false},  {__NR_setuid, false},
        {__NR_setgid, false},  {__NR_setresuid, false},     {__NR_setresgid, false},     {__NR_capset, false},
        {__NR_mount, false},   {__NR_umount2, false},       {__NR_pivot_root, false},    {__NR_setns, false},
        {__NR_unshare, false}, {__NR_clock_settime, false}, {__NR_clock_adjtime, false}, {__NR_settimeofday, false},
        {__NR_adjtimex, false}};
    const auto info = ebpf_plugin_start(
        "/wsl-security-audit",
        PERF_FEATURES_BTF | FEATURE_RINGBUFF | FEATURE_ENRICHMENT_AT_SOURCE | FEATURE_RUNTIME_MAP_FLIGHTING | FEATURE_PID_NS_ID_QUERY,
        FEATURE_SYSCALL_EXTENDED_DATA | FEATURE_CONTAINER_RUNTIME_METADATA | FEATURE_CONNECT_ENDPOINT_ENRICHMENT | FEATURE_ACCEPT_ENDPOINT_ENRICHMENT,
        syscalls,
        COUNT_OF(syscalls),
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
    LOG_INFO(
        "Security audit collecting ProcessExecuted fields, ProcessCreated, network and system events; command line enabled: {}", includeCommandLine);

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
    return audit.failed ? 1 : 0;
}
