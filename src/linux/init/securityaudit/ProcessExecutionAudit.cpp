// Copyright (C) Microsoft Corporation. All rights reserved.

#include "common.h"
#include "securityaudit/ProcessExecutionAudit.h"
#include <arpa/inet.h>
#include <netpacket/packet.h>
#include <linux/types.h>
#include "event_defs.h"

namespace wsl::linux::securityaudit {
namespace {
    std::optional<std::string> KernelString(const char* data, size_t size)
    {
        const auto* end = static_cast<const char*>(memchr(data, '\0', size));
        if (!end)
        {
            LOG_ERROR("Security audit omitted an unterminated text field");
            return {};
        }
        return std::string(data, end);
    }

    std::optional<std::string> KernelPath(const event_kpath_s& path)
    {
        if (path.dentries_number == 0)
        {
            return {};
        }
        if (path.dentries_number > KPATH_ENTRIES)
        {
            LOG_ERROR("Security audit omitted an invalid path");
            return {};
        }
        std::string result;
        for (size_t index = path.dentries_number; index > 0; --index)
        {
            const auto component = KernelString(path.dentry[index - 1], KPATH_ENTRY_SIZE);
            if (!component)
            {
                return {};
            }
            if (component->empty() || (index == path.dentries_number && *component == "/"))
            {
                continue;
            }
            if (component->find('/') != std::string::npos)
            {
                LOG_ERROR("Security audit omitted an invalid path component");
                return {};
            }
            result += '/';
            result += *component;
        }
        return result.empty() ? "/" : result;
    }
} // namespace

std::optional<wsl::shared::MessageWriter<LX_INIT_PROCESS_EXECUTED_DETAILS>> DecodeProcessExecution(gsl::span<const gsl::byte> data, bool includeCommandLine)
{
    THROW_ERRNO_IF(EINVAL, data.size() != sizeof(event_exec_process_s));
    for (const auto offset :
         {offsetof(event_exec_process_s, truncated_cmdline),
          offsetof(event_exec_process_s, kpath_binary) + offsetof(event_kpath_s, is_truncated),
          offsetof(event_exec_process_s, kpath_cwd) + offsetof(event_kpath_s, is_truncated)})
    {
        THROW_ERRNO_IF(EINVAL, std::to_integer<unsigned int>(data[offset]) > 1);
    }
    event_exec_process_s event{};
    memcpy(&event, data.data(), sizeof(event));
    THROW_ERRNO_IF(EINVAL, event.end.code_bytes_end != CODE_BYTES || event.base.pid == 0);
    if (event.base.enriched != 1)
    {
        LOG_ERROR("Security audit ignored an exec record without enrichment");
        return {};
    }

    wsl::shared::MessageWriter<LX_INIT_PROCESS_EXECUTED_DETAILS> message;
    message->Pid = event.base.pid;
    message->Tid = event.base.tid;
    message->Ppid = event.parent_pid;
    message->TimestampNs = event.operation_time;
    message->PidNamespace = event.base.pid_ns_id;
    message->MountNamespace = event.base.mnt_ns_id;
    message->NetworkNamespace = event.base.net_ns_id;
    const auto credential = [&](uint32_t value) { return (event.base.status & STATUS_CRED) ? INVALID_CRED : value; };
    message->Uid = credential(event.base.uid);
    message->Euid = credential(event.base.euid);
    message->Gid = credential(event.base.gid);
    message->Egid = credential(event.base.egid);
    if (!(event.base.status & STATUS_EXE))
    {
        if (const auto path = KernelPath(event.kpath_binary))
        {
            message.WriteString(message->ExecutablePathOffset, *path);
            if (event.kpath_binary.is_truncated)
            {
                message->Flags |= LxProcessExecutablePathTruncated;
            }
        }
    }
    if (!(event.base.status & STATUS_COMM))
    {
        if (const auto name = KernelString(event.comm, sizeof(event.comm)))
        {
            message.WriteString(message->ExecutableOffset, *name);
        }
    }
    if (!(event.base.status & STATUS_PWD))
    {
        if (const auto cwd = KernelPath(event.kpath_cwd))
        {
            message.WriteString(message->CwdOffset, *cwd);
            if (event.kpath_cwd.is_truncated)
            {
                message->Flags |= LxProcessCwdTruncated;
            }
        }
    }
    if (includeCommandLine && event.argsize != 0)
    {
        if (event.argsize > sizeof(event.cmdline) || event.argcount < 0 || event.argcount > MAX_CMDLINE_ARGS ||
            (!event.truncated_cmdline && event.cmdline[event.argsize - 1] != '\0'))
        {
            LOG_ERROR("Security audit omitted an invalid argv field");
        }
        else
        {
            std::string arguments(event.cmdline, event.argsize);
            if (arguments.back() != '\0')
            {
                arguments.push_back('\0');
            }
            const auto payload = message.InsertBuffer(message->ArgvOffset, arguments.size());
            memcpy(payload.data(), arguments.data(), arguments.size());
            message->ArgvBytes = static_cast<uint32_t>(arguments.size());
            if (event.truncated_cmdline)
            {
                message->Flags |= LxProcessArgvTruncated;
            }
        }
    }
    return message;
}

} // namespace wsl::linux::securityaudit
