// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "securityaudit/SystemAudit.h"
#include "SecurityAudit.h"
#include "stringshared.h"
#include "wslsecurityaudit.h"
#include <nlohmann/json.hpp>

namespace wsl::windows::common::securityaudit {
namespace {
    const char* SystemSyscallName(LX_SYSTEM_AUDIT_OPERATION operation)
    {
        switch (operation)
        {
        case LX_SYSTEM_AUDIT_OPERATION::InitModule:
            return "init_module";
        case LX_SYSTEM_AUDIT_OPERATION::FinitModule:
            return "finit_module";
        case LX_SYSTEM_AUDIT_OPERATION::SetUid:
            return "setuid";
        case LX_SYSTEM_AUDIT_OPERATION::SetGid:
            return "setgid";
        case LX_SYSTEM_AUDIT_OPERATION::SetResGid:
            return "setresgid";
        case LX_SYSTEM_AUDIT_OPERATION::SetNamespace:
            return "setns";
        case LX_SYSTEM_AUDIT_OPERATION::Unshare:
            return "unshare";
        default:
            THROW_HR(E_INVALIDARG);
        }
    }

    uint32_t SystemAuditAllowedFields(LX_SYSTEM_AUDIT_OPERATION operation)
    {
        switch (operation)
        {
        case LX_SYSTEM_AUDIT_OPERATION::InitModule:
        case LX_SYSTEM_AUDIT_OPERATION::FinitModule:
        case LX_SYSTEM_AUDIT_OPERATION::SetNamespace:
        case LX_SYSTEM_AUDIT_OPERATION::Unshare:
            return LxSystemAuditResult;
        case LX_SYSTEM_AUDIT_OPERATION::SetUid:
        case LX_SYSTEM_AUDIT_OPERATION::SetGid:
        case LX_SYSTEM_AUDIT_OPERATION::SetResGid:
            return LxSystemAuditUid | LxSystemAuditGid;
        default:
            THROW_HR(E_INVALIDARG);
        }
    }
} // namespace

static_assert(MSG_KERNEL_MODULE_LOAD <= UINT16_MAX && MSG_PRIVILEGE_TRANSITION <= UINT16_MAX && MSG_NAMESPACE_CHANGE <= UINT16_MAX);

std::wstring FormatSystemAudit(gsl::span<gsl::byte> bytes)
{
    const auto* event = gslhelpers::try_get_struct<LX_INIT_SYSTEM_AUDIT>(bytes);
    THROW_HR_IF(
        E_INVALIDARG,
        !event || bytes.size() != sizeof(*event) || event->Header.MessageSize != sizeof(*event) ||
            event->Header.MessageType != event->Type);
    THROW_HR_IF(E_INVALIDARG, (event->Fields & ~SystemAuditAllowedFields(event->Operation)) != 0);
    THROW_HR_IF(
        E_INVALIDARG,
        ((event->Fields & LxSystemAuditUid) && event->NewUid == UINT32_MAX) ||
            ((event->Fields & LxSystemAuditGid) && event->NewGid == UINT32_MAX) ||
            ((event->Fields & LxSystemAuditResult) && event->Result == INT64_MIN));

    nlohmann::json data{{"schemaVersion", 1}, {"syscall", SystemSyscallName(event->Operation)}};
    if (event->Fields & LxSystemAuditResult)
    {
        data["result"] = event->Result;
    }
    if (event->Fields & LxSystemAuditUid)
    {
        data["newUid"] = event->NewUid;
    }
    if (event->Fields & LxSystemAuditGid)
    {
        data["newGid"] = event->NewGid;
    }
    return wsl::shared::string::MultiByteToWide(EscapeEventParameters(data.dump()));
}

std::wstring FormatSystemAuditDisplay(LX_SYSTEM_AUDIT_OPERATION operation, std::wstring_view details)
{
    const auto allowed = SystemAuditAllowedFields(operation);
    const auto data = nlohmann::json::parse(wsl::shared::string::WideToMultiByte(std::wstring(details)));
    THROW_HR_IF(E_INVALIDARG, !data.is_object());
    if (allowed == LxSystemAuditResult)
    {
        return FormatSyscallEventDisplay(
            SystemSyscallName(operation),
            wsl::shared::Localization::MessageSecurityAuditResultDisplay(FormatEventField(data, "result")));
    }
    return FormatSyscallEventDisplay(
        SystemSyscallName(operation),
        wsl::shared::Localization::MessageSecurityAuditPrivilegeDisplay(
            FormatEventField(data, "newUid"), FormatEventField(data, "newGid")));
}

void ReportSystemAudit(EventSource& source, const LX_INIT_SYSTEM_AUDIT& event, std::wstring_view details)
{
    DWORD eventId;
    std::wstring description;
    switch (event.Operation)
    {
    case LX_SYSTEM_AUDIT_OPERATION::InitModule:
    case LX_SYSTEM_AUDIT_OPERATION::FinitModule:
        eventId = MSG_KERNEL_MODULE_LOAD;
        description = wsl::shared::Localization::MessageSecurityAuditKernelModuleLoad();
        break;
    case LX_SYSTEM_AUDIT_OPERATION::SetUid:
    case LX_SYSTEM_AUDIT_OPERATION::SetGid:
    case LX_SYSTEM_AUDIT_OPERATION::SetResGid:
        eventId = MSG_PRIVILEGE_TRANSITION;
        description = wsl::shared::Localization::MessageSecurityAuditPrivilegeTransition();
        break;
    case LX_SYSTEM_AUDIT_OPERATION::SetNamespace:
    case LX_SYSTEM_AUDIT_OPERATION::Unshare:
        eventId = MSG_NAMESPACE_CHANGE;
        description = wsl::shared::Localization::MessageSecurityAuditNamespaceChange();
        break;
    default:
        THROW_HR(E_INVALIDARG);
    }
    std::array<std::wstring, 4> fields{
        std::wstring{}, std::move(description), std::wstring(details), FormatSystemAuditDisplay(event.Operation, details)};
    source.Report(eventId, fields);
}

} // namespace wsl::windows::common::securityaudit
