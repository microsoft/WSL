// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "SecurityAudit.h"
#include "WslSecurity.h"
#include "wslutil.h"
#include "wslsecurityaudit.h"
// precomp.h already supplies winternl's STRING and UNICODE_STRING definitions.
#pragma push_macro("_NTDEF_")
#define _NTDEF_
#include <ntsecapi.h>
#pragma pop_macro("_NTDEF_")
#include <wil/token_helpers.h>

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

EventSource::EventSource() : m_token(AcquireTokenWithPrivilege(SE_AUDIT_NAME))
{
    auto impersonation = wil::impersonate_token(m_token.get());
    EnsureInstalled();
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
    std::wstring text[] = {
        std::to_wstring(pid),
        wsl::shared::Localization::MessageSecurityAuditProcessExecuted(),
        wsl::shared::Localization::MessageSecurityAuditProcessId()};
    AUDIT_PARAM inserts[ARRAYSIZE(text)]{};
    for (size_t index = 0; index < ARRAYSIZE(text); ++index)
    {
        inserts[index].Type = APT_String;
        inserts[index].String = text[index].data();
    }
    AUDIT_PARAMS parameters{sizeof(AUDIT_PARAMS), APF_AuditSuccess, ARRAYSIZE(inserts), inserts};
    THROW_IF_WIN32_BOOL_FALSE(AuthzReportSecurityEventFromParams(0, m_provider.get(), MSG_PROCESS_EXECUTED, nullptr, &parameters));
}

} // namespace wsl::windows::common::securityaudit
