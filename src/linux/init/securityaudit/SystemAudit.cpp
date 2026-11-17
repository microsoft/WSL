// Copyright (C) Microsoft Corporation. All rights reserved.

#include "common.h"
#include "securityaudit/SystemAudit.h"
#include <arpa/inet.h>
#include <netpacket/packet.h>
#include <sys/syscall.h>
#include <linux/types.h>
#include "event_defs.h"

namespace wsl::linux::securityaudit {
namespace {
    std::optional<LX_SYSTEM_AUDIT_OPERATION> SystemAuditOperation(unsigned long syscall)
    {
        switch (syscall)
        {
        case __NR_init_module:
            return LX_SYSTEM_AUDIT_OPERATION::InitModule;
        case __NR_finit_module:
            return LX_SYSTEM_AUDIT_OPERATION::FinitModule;
        case __NR_setuid:
            return LX_SYSTEM_AUDIT_OPERATION::SetUid;
        case __NR_setgid:
            return LX_SYSTEM_AUDIT_OPERATION::SetGid;
        case __NR_setresgid:
            return LX_SYSTEM_AUDIT_OPERATION::SetResGid;
        case __NR_setns:
            return LX_SYSTEM_AUDIT_OPERATION::SetNamespace;
        case __NR_unshare:
            return LX_SYSTEM_AUDIT_OPERATION::Unshare;
        default:
            return {};
        }
    }

    template <typename T>
    void ValidateSystemAuditRecord(gsl::span<const gsl::byte> data)
    {
        THROW_ERRNO_IF(EINVAL, data.size() != sizeof(T));
        event_end_s trailer{};
        memcpy(&trailer, data.data() + offsetof(T, end), sizeof(trailer));
        THROW_ERRNO_IF(EINVAL, trailer.code_bytes_end != CODE_BYTES);
    }
} // namespace

std::optional<LX_INIT_SYSTEM_AUDIT> DecodeSystemAudit(gsl::span<const gsl::byte> data)
{
    THROW_ERRNO_IF(EINVAL, data.size() < sizeof(event_begin_s));
    event_begin_s header{};
    memcpy(&header, data.data(), sizeof(header));
    if (header.event_type != event_type_regular && header.event_type != event_type_namespace && header.event_type != event_type_container_namespace)
    {
        return {};
    }

    THROW_ERRNO_IF(EINVAL, data.size() < sizeof(header) + sizeof(event_base_s));
    event_base_s base{};
    memcpy(&base, data.data() + sizeof(header), sizeof(base));
    const auto operation = SystemAuditOperation(base.syscall_id);
    if (!operation)
    {
        return {};
    }

    const bool isNamespace = *operation == LX_SYSTEM_AUDIT_OPERATION::SetNamespace || *operation == LX_SYSTEM_AUDIT_OPERATION::Unshare;
    if (header.event_type == event_type_regular)
    {
        ValidateSystemAuditRecord<event_regular_s>(data);
    }
    else
    {
        if (!isNamespace)
        {
            return {};
        }
        if (header.event_type == event_type_namespace)
        {
            ValidateSystemAuditRecord<event_namespace_s>(data);
        }
        else
        {
            ValidateSystemAuditRecord<event_container_namespace_s>(data);
        }
    }

    LX_INIT_SYSTEM_AUDIT message{};
    message.Header.MessageType = message.Type;
    message.Header.MessageSize = sizeof(message);
    message.Operation = *operation;
    // Unknown or entry-only records must not turn a zero-initialized return code into success.
    const bool hasResult = !(base.status & STATUS_RC) && base.return_code != RETURN_CODE_UNKNOWN && base.timestamp_exit_ns > base.timestamp_enter_ns;
    switch (*operation)
    {
    case LX_SYSTEM_AUDIT_OPERATION::SetUid:
    case LX_SYSTEM_AUDIT_OPERATION::SetGid:
    case LX_SYSTEM_AUDIT_OPERATION::SetResGid:
        // The SDK samples real IDs with bpf_get_current_uid_gid() at syscall exit, not from requested arguments.
        if (hasResult && base.return_code == 0 && base.enriched == 1 && !(base.status & STATUS_CRED))
        {
            if (base.uid != INVALID_CRED)
            {
                message.Fields |= LxSystemAuditUid;
                message.NewUid = base.uid;
            }
            if (base.gid != INVALID_CRED)
            {
                message.Fields |= LxSystemAuditGid;
                message.NewGid = base.gid;
            }
        }
        break;
    default:
        if (hasResult)
        {
            message.Fields = LxSystemAuditResult;
            message.Result = base.return_code;
        }
        break;
    }
    return message;
}

} // namespace wsl::linux::securityaudit
