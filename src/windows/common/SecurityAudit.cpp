// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "SecurityAudit.h"
#include "securityaudit/ProcessCreationAudit.h"
#include "securityaudit/ProcessExecutionAudit.h"
#include "securityaudit/NetworkAudit.h"
#include "securityaudit/SystemAudit.h"
#include "lxinitshared.h"
#include "WslSecurity.h"
#include "wslutil.h"
#include "wslsecurityaudit.h"
// precomp.h already supplies winternl's STRING and UNICODE_STRING definitions.
#pragma push_macro("_NTDEF_")
#define _NTDEF_
#include <ntsecapi.h>
#pragma pop_macro("_NTDEF_")
#include <wil/token_helpers.h>
#include <nlohmann/json.hpp>

namespace wsl::windows::common::securityaudit {
namespace {
    constexpr wchar_t c_source[] = L"Microsoft-Windows-WSL-SecurityAudit";
    static_assert(MSG_PROCESS_EXECUTED <= UINT16_MAX);

    wil::unique_handle AcquireTokenWithPrivilege(PCWSTR privilege)
    {
        wil::unique_handle processToken;
        THROW_IF_WIN32_BOOL_FALSE(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, &processToken));
        wil::unique_handle token;
        THROW_IF_WIN32_BOOL_FALSE(DuplicateTokenEx(
            processToken.get(), TOKEN_QUERY | TOKEN_ADJUST_PRIVILEGES | TOKEN_IMPERSONATE, nullptr, SecurityImpersonation, TokenImpersonation, &token));
        security::EnableTokenPrivilege(token.get(), privilege);
        // AdjustTokenPrivileges can return TRUE without assigning the requested privilege.
        THROW_IF_WIN32_ERROR(GetLastError());
        return token;
    }

    void EnsureInstalled()
    {
        static wil::srwlock installLock;
        auto lock = installLock.lock_exclusive();
        auto token = AcquireTokenWithPrivilege(SE_SECURITY_NAME);
        auto impersonation = wil::impersonate_token(token.get());

        std::wstring name(c_source);
        std::wstring path = (wslutil::GetBasePath() / L"wsl.exe").wstring();
        AUTHZ_SOURCE_SCHEMA_REGISTRATION registration{};
        registration.dwFlags = AUTHZ_ALLOW_MULTIPLE_SOURCE_INSTANCES;
        registration.szEventSourceName = name.data();
        registration.szEventMessageFile = path.data();
        if (!AuthzInstallSecurityEventSource(0, &registration))
        {
            const auto error = GetLastError();
            THROW_WIN32_IF(error, error != ERROR_OBJECT_ALREADY_EXISTS);
        }
    }
} // namespace

bool Enabled()
{
    auto self = wil::run_as_self();
    // Both collection policy and the policy required by Authz must already be enabled.
    GUID categories[] = {c_processCreationAuditSubcategory, c_applicationGeneratedAuditSubcategory};
    wil::unique_any<PAUDIT_POLICY_INFORMATION, decltype(&AuditFree), AuditFree> policy;
    THROW_LAST_ERROR_IF(!AuditQuerySystemPolicy(categories, ARRAYSIZE(categories), &policy));
    return (policy.get()[0].AuditingInformation & POLICY_AUDIT_EVENT_SUCCESS) != 0 &&
           (policy.get()[1].AuditingInformation & POLICY_AUDIT_EVENT_SUCCESS) != 0;
}

void Uninstall()
{
    auto token = AcquireTokenWithPrivilege(SE_SECURITY_NAME);
    auto impersonation = wil::impersonate_token(token.get());
    if (!AuthzUninstallSecurityEventSource(0, c_source))
    {
        // Auditing may never have been enabled, so the source may not have been installed.
        const auto error = GetLastError();
        THROW_WIN32_IF(error, error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND);
    }
}

std::string EscapeEventParameters(std::string_view text)
{
    std::string escaped;
    escaped.reserve(text.size());
    for (const char character : text)
    {
        if (character == '%')
        {
            escaped += "\\u0025"; // Prevent Event Viewer parameter-message expansion.
        }
        else
        {
            escaped += character;
        }
    }
    return escaped;
}

std::wstring FormatEventField(const nlohmann::json& object, const char* name)
{
    const auto value = object.find(name);
    if (value == object.end())
    {
        return L"-";
    }
    auto text = value->dump(-1, ' ', true);
    if (value->is_string())
    {
        text = text.substr(1, text.size() - 2);
    }
    return wsl::shared::string::MultiByteToWide(EscapeEventParameters(text));
}

std::wstring NormalizeEventMessage(std::wstring_view text)
{
    std::wstring display;
    for (const auto character : text)
    {
        if (character == L'\r')
        {
            continue;
        }
        if (character == L'\n')
        {
            display += L'\r';
        }
        display += character;
    }
    return display;
}

std::wstring FormatSyscallEventDisplay(std::string_view syscall, std::wstring_view text)
{
    return NormalizeEventMessage(
        wsl::shared::Localization::MessageSecurityAuditSystemCall(wsl::shared::string::MultiByteToWide(std::string(syscall))) +
        L"\n\n" + std::wstring(text));
}

EventSource::EventSource(std::wstring wslcVmId) :
    m_wslcVmId(std::move(wslcVmId)), m_token(AcquireTokenWithPrivilege(SE_AUDIT_NAME))
{
    auto impersonation = wil::impersonate_token(m_token.get());
    EnsureInstalled();
    THROW_IF_WIN32_BOOL_FALSE(AuthzRegisterSecurityEventSource(0, c_source, &m_provider));
}

void EventSource::Close(AUTHZ_SECURITY_EVENT_PROVIDER_HANDLE provider) noexcept
{
    LOG_IF_WIN32_BOOL_FALSE(AuthzUnregisterSecurityEventSource(0, &provider));
}

void EventSource::Report(DWORD eventId, std::span<std::wstring> fields)
{
    THROW_HR_IF(E_INVALIDARG, eventId > UINT16_MAX || fields.size() < 2 || fields.size() >= USHRT_MAX);
    if (!m_wslcVmId.empty())
    {
        fields[1] += L"\r\n" + wsl::shared::Localization::MessageWslcAuditVmId(m_wslcVmId);
    }
    auto impersonation = wil::impersonate_token(m_token.get());
    std::vector<AUDIT_PARAM> values(fields.size());
    for (size_t index = 0; index < fields.size(); ++index)
    {
        values[index].Type = APT_String;
        values[index].String = fields[index].data();
    }
    if (!m_wslcVmId.empty())
    {
        // Append a machine-readable origin without shifting existing PID, JSON or display insertions.
        AUDIT_PARAM origin{};
        origin.Type = APT_String;
        origin.String = m_wslcVmId.data();
        values.push_back(origin);
    }
    AUDIT_PARAMS parameters{sizeof(AUDIT_PARAMS), APF_AuditSuccess, static_cast<USHORT>(values.size()), values.data()};
    THROW_IF_WIN32_BOOL_FALSE(AuthzReportSecurityEventFromParams(0, m_provider.get(), eventId, nullptr, &parameters));
}

void ReportEvent(EventSource& source, gsl::span<gsl::byte> bytes, bool includeCommandLine, const ProcessExecutionDistribution* distribution)
{
    const auto* header = gslhelpers::try_get_struct<MESSAGE_HEADER>(bytes);
    THROW_HR_IF(E_INVALIDARG, !header);
    if (header->MessageType == LxInitMessageProcessExecuted)
    {
        const auto* event = gslhelpers::try_get_struct<LX_INIT_PROCESS_EXECUTED>(bytes);
        THROW_HR_IF(E_INVALIDARG, !event || bytes.size() != sizeof(*event));
        ReportProcessExecution(source, event->Pid);
        return;
    }

    THROW_HR_IF(
        E_INVALIDARG,
        header->MessageType != LxInitMessageProcessCreated && header->MessageType != LxInitMessageProcessExecutedDetails &&
            header->MessageType != LxInitMessageNetworkAudit && header->MessageType != LxInitMessageSystemAudit);
    std::wstring details;
    try
    {
        switch (header->MessageType)
        {
        case LxInitMessageProcessCreated:
            details = FormatProcessCreation(bytes);
            break;
        case LxInitMessageNetworkAudit:
            details = FormatNetworkAudit(bytes);
            break;
        case LxInitMessageSystemAudit:
            details = FormatSystemAudit(bytes);
            break;
        case LxInitMessageProcessExecutedDetails:
            details = FormatProcessExecution(bytes, includeCommandLine, distribution);
            break;
        }
    }
    catch (const wil::ResultException& error)
    {
        THROW_HR_IF(error.GetErrorCode(), error.GetErrorCode() != E_INVALIDARG);
        LOG_CAUGHT_EXCEPTION();
        return;
    }

    switch (header->MessageType)
    {
    case LxInitMessageProcessCreated:
    {
        const auto* event = gslhelpers::try_get_struct<LX_INIT_PROCESS_CREATED>(bytes);
        ReportProcessCreation(source, event->ChildPid, details);
        break;
    }
    case LxInitMessageNetworkAudit:
    {
        const auto* event = gslhelpers::try_get_struct<LX_INIT_NETWORK_AUDIT>(bytes);
        ReportNetworkAudit(source, *event, details);
        break;
    }
    case LxInitMessageSystemAudit:
    {
        const auto* event = gslhelpers::try_get_struct<LX_INIT_SYSTEM_AUDIT>(bytes);
        ReportSystemAudit(source, *event, details);
        break;
    }
    case LxInitMessageProcessExecutedDetails:
    {
        const auto* event = gslhelpers::try_get_struct<LX_INIT_PROCESS_EXECUTED_DETAILS>(bytes);
        ReportProcessExecutionDetails(source, event->Pid, details);
        break;
    }
    }
}

} // namespace wsl::windows::common::securityaudit
