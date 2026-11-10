// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "SecurityAudit.h"
#include "WslSecurity.h"
#include "registry.hpp"
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
    constexpr wchar_t c_sourceKey[] =
        L"SYSTEM\\CurrentControlSet\\Services\\EventLog\\Security\\Microsoft-Windows-WSL-SecurityAudit";
    static_assert(MSG_PROCESS_EXECUTED <= UINT16_MAX);

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

    void EnsureInstalled()
    {
        static wil::srwlock installLock;
        auto lock = installLock.lock_exclusive();
        const auto messageFile = (wslutil::GetBasePath() / L"wsl.exe").wstring();
        auto [key, result] = registry::OpenKeyNoThrow(HKEY_LOCAL_MACHINE, c_sourceKey, KEY_QUERY_VALUE);
        if (result == S_OK)
        {
            if (registry::ReadString(key.get(), nullptr, L"EventMessageFile", L"") == messageFile &&
                registry::ReadDword(key.get(), nullptr, L"EventSourceFlags", 0) == AUTHZ_ALLOW_MULTIPLE_SOURCE_INSTANCES)
            {
                return;
            }
        }
        else
        {
            THROW_HR_IF(result, result != HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) && result != HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND));
        }

        auto token = Authority(SE_SECURITY_NAME);
        auto impersonation = wil::impersonate_token(token.get());
        if (key)
        {
            // AuthzInstallSecurityEventSource cannot update an existing source.
            const auto writableKey = registry::OpenKey(HKEY_LOCAL_MACHINE, c_sourceKey, KEY_SET_VALUE);
            registry::WriteString(writableKey.get(), nullptr, L"EventMessageFile", messageFile.c_str());
            registry::WriteDword(writableKey.get(), nullptr, L"EventSourceFlags", AUTHZ_ALLOW_MULTIPLE_SOURCE_INSTANCES);
            return;
        }

        std::wstring name(c_source);
        std::wstring path(messageFile);
        AUTHZ_SOURCE_SCHEMA_REGISTRATION registration{};
        registration.dwFlags = AUTHZ_ALLOW_MULTIPLE_SOURCE_INSTANCES;
        registration.szEventSourceName = name.data();
        registration.szEventMessageFile = path.data();
        THROW_IF_WIN32_BOOL_FALSE(AuthzInstallSecurityEventSource(0, &registration));
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
        // Auditing may never have been enabled, so the source may not have been installed.
        const auto error = GetLastError();
        THROW_WIN32_IF(error, error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND);
    }
}

EventSource::EventSource() : m_token(Authority(SE_AUDIT_NAME))
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
    THROW_HR_IF(E_INVALIDARG, eventId > UINT16_MAX || fields.empty() || fields.size() > USHRT_MAX);
    auto impersonation = wil::impersonate_token(m_token.get());
    std::vector<AUDIT_PARAM> values(fields.size());
    for (size_t index = 0; index < fields.size(); ++index)
    {
        values[index].Type = APT_String;
        values[index].String = fields[index].data();
    }
    AUDIT_PARAMS parameters{sizeof(AUDIT_PARAMS), APF_AuditSuccess, static_cast<USHORT>(values.size()), values.data()};
    THROW_IF_WIN32_BOOL_FALSE(AuthzReportSecurityEventFromParams(0, m_provider.get(), eventId, nullptr, &parameters));
}

} // namespace wsl::windows::common::securityaudit
