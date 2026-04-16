/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WinCredStorage.cpp

Abstract:

    Windows Credential Manager credential storage implementation.

--*/

#include "precomp.h"
#include "WinCredStorage.h"
#include <wincred.h>

using wsl::shared::Localization;

using unique_credential = wil::unique_any<PCREDENTIALW, decltype(&CredFree), CredFree>;
using unique_credential_array = wil::unique_any<PCREDENTIALW*, decltype(&CredFree), CredFree>;

static constexpr auto WinCredPrefix = L"wslc-credential/";

namespace wsl::windows::wslc::services {

std::wstring WinCredStorage::TargetName(const std::string& serverAddress)
{
    return std::wstring(WinCredPrefix) + wsl::shared::string::MultiByteToWide(serverAddress);
}

void WinCredStorage::Store(const std::string& serverAddress, const std::string& username, const std::string& secret)
{
    auto targetName = TargetName(serverAddress);
    auto wideUsername = wsl::shared::string::MultiByteToWide(username);

    CREDENTIALW cred{};
    cred.Type = CRED_TYPE_GENERIC;
    cred.TargetName = const_cast<LPWSTR>(targetName.c_str());
    cred.UserName = const_cast<LPWSTR>(wideUsername.c_str());
    cred.CredentialBlobSize = static_cast<DWORD>(secret.size());
    cred.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char*>(secret.data()));
    cred.Persist = CRED_PERSIST_LOCAL_MACHINE;

    THROW_IF_WIN32_BOOL_FALSE(CredWriteW(&cred, 0));
}

std::pair<std::string, std::string> WinCredStorage::Get(const std::string& serverAddress)
{
    auto targetName = TargetName(serverAddress);

    unique_credential cred;
    if (!CredReadW(targetName.c_str(), CRED_TYPE_GENERIC, 0, &cred))
    {
        THROW_LAST_ERROR_IF(GetLastError() != ERROR_NOT_FOUND);
        return {};
    }

    if (cred.get()->CredentialBlobSize == 0 || cred.get()->CredentialBlob == nullptr)
    {
        return {};
    }

    std::string username;
    if (cred.get()->UserName)
    {
        username = wsl::shared::string::WideToMultiByte(cred.get()->UserName);
    }

    return {
        std::move(username),
        {reinterpret_cast<const char*>(cred.get()->CredentialBlob), cred.get()->CredentialBlobSize}};
}

void WinCredStorage::Erase(const std::string& serverAddress)
{
    auto targetName = TargetName(serverAddress);

    if (!CredDeleteW(targetName.c_str(), CRED_TYPE_GENERIC, 0))
    {
        auto error = GetLastError();
        THROW_HR_WITH_USER_ERROR_IF(
            E_NOT_SET, Localization::WSLCCLI_LogoutNotFound(wsl::shared::string::MultiByteToWide(serverAddress)), error == ERROR_NOT_FOUND);

        THROW_WIN32(error);
    }
}

std::vector<std::wstring> WinCredStorage::List()
{
    auto prefix = std::wstring(WinCredPrefix);
    auto filter = prefix + L"*";

    DWORD count = 0;
    unique_credential_array creds;
    if (!CredEnumerateW(filter.c_str(), 0, &count, &creds))
    {
        THROW_LAST_ERROR_IF(GetLastError() != ERROR_NOT_FOUND);
        return {};
    }

    std::vector<std::wstring> result;
    result.reserve(count);

    for (DWORD i = 0; i < count; ++i)
    {
        std::wstring_view name(creds.get()[i]->TargetName);
        result.emplace_back(name.substr(prefix.size()));
    }

    return result;
}

} // namespace wsl::windows::wslc::services
