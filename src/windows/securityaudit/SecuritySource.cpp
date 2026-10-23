// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "SecuritySource.h"
#include "WindowsSecuritySink.h"
#include <wil/token_helpers.h>

namespace wsl::audit {
namespace {
    constexpr wchar_t c_owner[] = L"{DAEF1E4E-B7C2-485F-A634-90FF2D781424}";
    constexpr wchar_t c_ownerValue[] = L"WslNativeAuditOwner";

    std::wstring SourcePath()
    {
        return std::wstring(L"SYSTEM\\CurrentControlSet\\Services\\EventLog\\Security\\") + c_securitySourceName;
    }

    bool OwnedSource()
    {
        wchar_t owner[64]{};
        DWORD size = sizeof(owner);
        return RegGetValueW(HKEY_LOCAL_MACHINE, SourcePath().c_str(), c_ownerValue, RRF_RT_REG_SZ, nullptr, owner, &size) == ERROR_SUCCESS &&
               std::wstring_view(owner) == c_owner;
    }

    void PreserveHistory(std::wstring_view messageFile)
    {
        wil::unique_hkey key;
        THROW_IF_WIN32_ERROR(RegCreateKeyExW(
            HKEY_LOCAL_MACHINE, SourcePath().c_str(), 0, nullptr, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &key, nullptr));
        THROW_IF_WIN32_ERROR(RegSetValueExW(key.get(), c_ownerValue, 0, REG_SZ, reinterpret_cast<const BYTE*>(c_owner), sizeof(c_owner)));
        const std::wstring path(messageFile);
        THROW_IF_WIN32_ERROR(RegSetValueExW(
            key.get(), L"EventMessageFile", 0, REG_SZ, reinterpret_cast<const BYTE*>(path.c_str()), static_cast<DWORD>((path.size() + 1) * sizeof(wchar_t))));
    }

    void VerifyOwner(const security_detail::SourceInfo& source, std::wstring_view messageFile)
    {
        const bool hasKey = source.registryStatus != ERROR_FILE_NOT_FOUND && source.registryStatus != ERROR_PATH_NOT_FOUND;
        THROW_HR_IF_MSG(
            HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS),
            (source.installed || hasKey) && (!OwnedSource() || source.registryMessageStatus != ERROR_SUCCESS ||
                                             _wcsicmp(source.registryMessageFile.c_str(), std::wstring(messageFile).c_str()) != 0 ||
                                             (source.installed && !security_detail::SourceReadbackMatches(source, messageFile))),
            "Native WSL Security source conflicts with an unowned source or different resource path; it was not changed.");
    }
} // namespace

bool IsNativeSecuritySourceOwned() noexcept
try
{
    return OwnedSource();
}
catch (...)
{
    return false;
}

void InstallSecuritySource(std::wstring_view messageFile)
{
    security_detail::ValidateMessageResource(messageFile);
    auto authority = security_detail::PrimaryAuthority(SE_SECURITY_NAME);
    auto impersonation = wil::impersonate_token(authority.get());
    auto source = security_detail::QuerySource();
    VerifyOwner(source, messageFile);
    if (!source.installed)
    {
        std::wstring name(c_securitySourceName);
        std::wstring file(messageFile);
        AUTHZ_SOURCE_SCHEMA_REGISTRATION registration{};
        registration.dwFlags = c_securitySourceInstallFlags;
        registration.szEventSourceName = name.data();
        registration.szEventMessageFile = file.data();
        THROW_IF_WIN32_BOOL_FALSE(AuthzInstallSecurityEventSource(0, &registration));
        PreserveHistory(messageFile);
    }
    THROW_HR_IF_MSG(
        E_UNEXPECTED,
        !security_detail::SourceReadbackMatches(security_detail::QuerySource(), messageFile),
        "Native WSL Security source registration readback failed.");
}

void UninstallSecuritySource(std::wstring_view messageFile)
{
    auto authority = security_detail::PrimaryAuthority(SE_SECURITY_NAME);
    auto impersonation = wil::impersonate_token(authority.get());
    const auto source = security_detail::QuerySource();
    VerifyOwner(source, messageFile);
    if (source.installed)
    {
        THROW_IF_WIN32_BOOL_FALSE(AuthzUninstallSecurityEventSource(0, c_securitySourceName));
        // MSI retains this resource-only DLL. Keep its lookup metadata after deregistration.
        PreserveHistory(messageFile);
    }
    THROW_HR_IF(E_UNEXPECTED, security_detail::QuerySource().installed);
}
} // namespace wsl::audit
