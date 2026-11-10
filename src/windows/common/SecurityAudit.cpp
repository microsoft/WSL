// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "SecurityAudit.h"
#include "WslSecurity.h"
#include "wsleventschema.h"
// precomp.h already supplies winternl's STRING and UNICODE_STRING definitions.
#pragma push_macro("_NTDEF_")
#define _NTDEF_
#include <ntsecapi.h>
#pragma pop_macro("_NTDEF_")
#include <wil/token_helpers.h>

namespace wsl::windows::common::securityaudit {
namespace {
    constexpr wchar_t c_source[] = L"Microsoft-Windows-WSL-SecurityAudit";

    wil::unique_handle Authority(PCWSTR privilege)
    {
        wil::unique_handle processToken;
        THROW_IF_WIN32_BOOL_FALSE(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, &processToken));
        wil::unique_handle token;
        THROW_IF_WIN32_BOOL_FALSE(DuplicateTokenEx(
            processToken.get(), TOKEN_QUERY | TOKEN_ADJUST_PRIVILEGES | TOKEN_IMPERSONATE, nullptr, SecurityImpersonation, TokenImpersonation, &token));
        SetLastError(ERROR_SUCCESS);
        security::EnableTokenPrivilege(token.get(), privilege);
        THROW_IF_WIN32_ERROR(GetLastError());
        return token;
    }
} // namespace

bool Enabled()
{
    auto self = wil::run_as_self();
    // Both collection policy and the policy required by Authz must already be enabled.
    GUID categories[] = {
        {0x0cce922b, 0x69ae, 0x11d9, {0xbe, 0xd3, 0x50, 0x50, 0x54, 0x50, 0x30, 0x30}},
        {0x0cce9222, 0x69ae, 0x11d9, {0xbe, 0xd3, 0x50, 0x50, 0x54, 0x50, 0x30, 0x30}}};
    wil::unique_any<PAUDIT_POLICY_INFORMATION, decltype(&AuditFree), AuditFree> policy;
    THROW_LAST_ERROR_IF(!AuditQuerySystemPolicy(categories, ARRAYSIZE(categories), &policy));
    return (policy.get()[0].AuditingInformation & POLICY_AUDIT_EVENT_SUCCESS) != 0 &&
           (policy.get()[1].AuditingInformation & POLICY_AUDIT_EVENT_SUCCESS) != 0;
}

void Uninstall()
{
    auto token = Authority(SE_SECURITY_NAME);
    auto impersonation = wil::impersonate_token(token.get());
    if (!AuthzUninstallSecurityEventSource(0, c_source))
    {
        // MSI normally removes the values and the empty key before this commit cleanup.
        const auto error = GetLastError();
        THROW_WIN32_IF(error, error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND);
    }
}

EventSource::EventSource() : m_token(Authority(SE_AUDIT_NAME))
{
    auto impersonation = wil::impersonate_token(m_token.get());
    THROW_IF_WIN32_BOOL_FALSE(AuthzRegisterSecurityEventSource(0, c_source, &m_provider));
}

void EventSource::Close(AUTHZ_SECURITY_EVENT_PROVIDER_HANDLE provider) noexcept
{
    LOG_IF_WIN32_BOOL_FALSE(AuthzUnregisterSecurityEventSource(0, &provider));
}

void EventSource::ProcessExecuted(uint32_t pid)
{
    THROW_HR_IF(E_INVALIDARG, pid == 0);
    auto impersonation = wil::impersonate_token(m_token.get());
    auto text = std::to_wstring(pid);
    AUDIT_PARAM parameter{};
    parameter.Type = APT_String;
    parameter.String = text.data();
    AUDIT_PARAMS parameters{sizeof(AUDIT_PARAMS), APF_AuditSuccess, 1, &parameter};
    THROW_IF_WIN32_BOOL_FALSE(AuthzReportSecurityEventFromParams(0, m_provider.get(), MSG_PROCESS_EXECUTED, nullptr, &parameters));
}

} // namespace wsl::windows::common::securityaudit
