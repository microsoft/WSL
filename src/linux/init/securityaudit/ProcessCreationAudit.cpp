// Copyright (C) Microsoft Corporation. All rights reserved.

#include "common.h"
#include "securityaudit/ProcessCreationAudit.h"
#include <arpa/inet.h>
#include <netpacket/packet.h>
#include <linux/types.h>
#include "event_defs.h"

namespace wsl::linux::securityaudit {

std::optional<LX_INIT_PROCESS_CREATED> DecodeProcessCreation(gsl::span<const gsl::byte> data)
{
    THROW_ERRNO_IF(EINVAL, data.size() != sizeof(event_fork_process_s));
    THROW_ERRNO_IF(EINVAL, std::to_integer<unsigned int>(data[offsetof(event_fork_process_s, kthread)]) > 1);
    event_fork_process_s event{};
    memcpy(&event, data.data(), sizeof(event));
    THROW_ERRNO_IF(EINVAL, event.end.code_bytes_end != CODE_BYTES || event.base.pid == 0 || event.base.tid == 0);
    if (event.kthread)
    {
        return {};
    }
    if (event.base.enriched != 1)
    {
        LOG_ERROR("Security audit ignored a process-creation record without enrichment");
        return {};
    }

    LX_INIT_PROCESS_CREATED message{};
    message.Header.MessageType = message.Type;
    message.Header.MessageSize = sizeof(message);
    // The SDK fork record overwrites pid/tid with the child IDs and keeps the creator TGID in pid2.
    message.ParentPid = event.base.pid2;
    message.ChildPid = event.base.pid;
    message.ChildTid = event.base.tid;
    // Namespace, cgroup and credential enrichment belongs to the creating task, not a new child namespace.
    message.PidNamespace = event.base.pid_ns_id;
    message.MountNamespace = event.base.mnt_ns_id;
    message.NetworkNamespace = event.base.net_ns_id;
    message.CgroupId = event.base.cgroup_id;
    message.Uid = (event.base.status & STATUS_CRED) ? INVALID_CRED : event.base.uid;
    message.Gid = (event.base.status & STATUS_CRED) ? INVALID_CRED : event.base.gid;
    // Fork tracepoints have no syscall-entry timestamp; the SDK records emission using bpf_ktime_get_ns.
    message.TimestampNs = event.base.timestamp_exit_ns;
    return message;
}

} // namespace wsl::linux::securityaudit
