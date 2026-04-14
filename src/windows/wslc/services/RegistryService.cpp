/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    RegistryService.cpp

Abstract:

    This file contains the RegistryService implementation

--*/

#include "RegistryService.h"
#include <ExecutionContext.h>
#include <Localization.h>
#include <retryshared.h>
#include <wslutil.h>
#include <wincred.h>

using namespace wsl::shared;
using namespace wsl::windows::common::wslutil;

static constexpr auto WinCredPrefix = L"wslc-credential/";

using unique_credential = wil::unique_any<PCREDENTIALW, decltype(&CredFree), CredFree>;
using unique_credential_array = wil::unique_any<PCREDENTIALW*, decltype(&CredFree), CredFree>;

namespace {

std::filesystem::path GetFilePath()
{
    return wsl::windows::common::filesystem::GetLocalAppDataPath(nullptr) / L"wslc" / L"registry-credentials.json";
}

wil::unique_hfile RetryOpenFileOnSharingViolation(const std::function<wil::unique_hfile()>& openFunc)
{
    return wsl::shared::retry::RetryWithTimeout<wil::unique_hfile>(openFunc, std::chrono::milliseconds(100), std::chrono::seconds(1), []() {
        return wil::ResultFromCaughtException() == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION);
    });
}

wil::unique_hfile OpenFileExclusive()
{
    wil::unique_hfile handle(
        CreateFileW(GetFilePath().c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    THROW_LAST_ERROR_IF(!handle.is_valid() && GetLastError() != ERROR_FILE_NOT_FOUND && GetLastError() != ERROR_PATH_NOT_FOUND);

    return handle;
}

wil::unique_hfile CreateFileExclusive()
{
    auto filePath = GetFilePath();
    std::filesystem::create_directories(filePath.parent_path());

    wil::unique_hfile handle(
        CreateFileW(filePath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    THROW_LAST_ERROR_IF(!handle.is_valid());

    return handle;
}

wil::unique_hfile OpenFileShared()
{
    wil::unique_hfile handle(CreateFileW(
        GetFilePath().c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    THROW_LAST_ERROR_IF(!handle.is_valid() && GetLastError() != ERROR_FILE_NOT_FOUND && GetLastError() != ERROR_PATH_NOT_FOUND);

    return handle;
}

nlohmann::json ReadJsonFile(const wil::unique_hfile& handle)
{
    if (!handle.is_valid())
    {
        return nlohmann::json::object();
    }
    
    LARGE_INTEGER size{};
    THROW_IF_WIN32_BOOL_FALSE(GetFileSizeEx(handle.get(), &size));
    if (size.QuadPart == 0)
    {
        return nlohmann::json::object();
    }

    LARGE_INTEGER zero{};
    THROW_IF_WIN32_BOOL_FALSE(SetFilePointerEx(handle.get(), zero, nullptr, FILE_BEGIN));

    std::string buffer(static_cast<size_t>(size.QuadPart), '\0');
    DWORD bytesRead = 0;
    THROW_IF_WIN32_BOOL_FALSE(ReadFile(handle.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &bytesRead, nullptr));
    buffer.resize(bytesRead);

    try
    {
        return nlohmann::json::parse(buffer);
    }
    catch (...)
    {
        return nlohmann::json::object();
    }
}

void WriteJsonFile(const wil::unique_hfile& handle, const nlohmann::json& data)
{
    LARGE_INTEGER zero{};
    THROW_IF_WIN32_BOOL_FALSE(SetFilePointerEx(handle.get(), zero, nullptr, FILE_BEGIN));
    THROW_IF_WIN32_BOOL_FALSE(SetEndOfFile(handle.get()));

    auto content = data.dump(2);
    DWORD written = 0;
    THROW_IF_WIN32_BOOL_FALSE(WriteFile(handle.get(), content.data(), static_cast<DWORD>(content.size()), &written, nullptr));
    THROW_IF_WIN32_BOOL_FALSE(FlushFileBuffers(handle.get()));
}

std::string ResolveCredentialKey(const std::string& serverAddress)
{
    // Normalize known Docker Hub aliases to the canonical DefaultServer key,
    // matching Docker CLI's getAuthConfigKey() behavior.
    if (serverAddress == "docker.io" || serverAddress == "index.docker.io" || serverAddress == "registry-1.docker.io")
    {
        return wsl::windows::wslc::services::RegistryService::DefaultServer;
    }

    return serverAddress;
}
} // namespace

namespace wsl::windows::wslc::services {

void RegistryService::Store(const std::string& serverAddress, const std::string& credential)
{
    THROW_HR_IF(E_INVALIDARG, serverAddress.empty());
    THROW_HR_IF(E_INVALIDARG, credential.empty());

    auto key = ResolveCredentialKey(serverAddress);
    auto backend = settings::User().Get<settings::Setting::CredentialStore>();
    backend == CredentialStoreType::File ? FileStoreCredential(key, credential) : WinCredStoreCredential(key, credential);
}

std::optional<std::string> RegistryService::Get(const std::string& serverAddress)
{
    if (serverAddress.empty())
    {
        return std::nullopt;
    }

    auto key = ResolveCredentialKey(serverAddress);
    auto backend = settings::User().Get<settings::Setting::CredentialStore>();
    return backend == CredentialStoreType::File ? FileGetCredential(key) : WinCredGetCredential(key);
}

void RegistryService::Erase(const std::string& serverAddress)
{
    THROW_HR_IF(E_INVALIDARG, serverAddress.empty());

    auto key = ResolveCredentialKey(serverAddress);
    auto backend = settings::User().Get<settings::Setting::CredentialStore>();
    backend == CredentialStoreType::File ? FileEraseCredential(key) : WinCredEraseCredential(key);
}

std::vector<std::wstring> RegistryService::List()
{
    auto backend = settings::User().Get<settings::Setting::CredentialStore>();
    return backend == CredentialStoreType::File ? FileListCredentials() : WinCredListCredentials();
}

// --- WinCred backend ---

std::wstring RegistryService::WinCredTargetName(const std::string& serverAddress)
{
    return std::wstring(WinCredPrefix) + wsl::shared::string::MultiByteToWide(serverAddress);
}

void RegistryService::WinCredStoreCredential(const std::string& serverAddress, const std::string& credential)
{
    auto targetName = WinCredTargetName(serverAddress);

    CREDENTIALW cred{};
    cred.Type = CRED_TYPE_GENERIC;
    cred.TargetName = const_cast<LPWSTR>(targetName.c_str());
    cred.CredentialBlobSize = static_cast<DWORD>(credential.size());
    cred.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char*>(credential.data()));
    cred.Persist = CRED_PERSIST_LOCAL_MACHINE;

    THROW_IF_WIN32_BOOL_FALSE(CredWriteW(&cred, 0));
}

std::optional<std::string> RegistryService::WinCredGetCredential(const std::string& serverAddress)
{
    auto targetName = WinCredTargetName(serverAddress);

    unique_credential cred;
    if (!CredReadW(targetName.c_str(), CRED_TYPE_GENERIC, 0, &cred))
    {
        THROW_LAST_ERROR_IF(GetLastError() != ERROR_NOT_FOUND);
        return std::nullopt;
    }

    if (cred.get()->CredentialBlobSize == 0 || cred.get()->CredentialBlob == nullptr)
    {
        return std::nullopt;
    }

    return std::string(reinterpret_cast<const char*>(cred.get()->CredentialBlob), cred.get()->CredentialBlobSize);
}

void RegistryService::WinCredEraseCredential(const std::string& serverAddress)
{
    auto targetName = WinCredTargetName(serverAddress);

    if (!CredDeleteW(targetName.c_str(), CRED_TYPE_GENERIC, 0))
    {
        auto error = GetLastError();
        THROW_HR_WITH_USER_ERROR_IF(
            E_NOT_SET, Localization::WSLCCLI_LogoutNotFound(wsl::shared::string::MultiByteToWide(serverAddress)), error == ERROR_NOT_FOUND);

        THROW_WIN32(error);
    }
}

std::vector<std::wstring> RegistryService::WinCredListCredentials()
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
        // Strip the prefix to return the bare server address.
        std::wstring_view name(creds.get()[i]->TargetName);
        result.emplace_back(name.substr(prefix.size()));
    }

    return result;
}

// --- File backend ---

void RegistryService::ModifyFileStore(wil::unique_hfile handle, const std::function<bool(nlohmann::json&)>& modifier)
{
    WI_VERIFY(handle.is_valid());

    auto data = ReadJsonFile(handle);

    if (modifier(data))
    {
        WriteJsonFile(handle, data);
    }
}

std::string RegistryService::Protect(const std::string& plaintext)
{
    DATA_BLOB input{};
    input.cbData = static_cast<DWORD>(plaintext.size());
    input.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(plaintext.data()));

    DATA_BLOB output{};
    THROW_IF_WIN32_BOOL_FALSE(CryptProtectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output));
    auto cleanup = wil::scope_exit([&]() { LocalFree(output.pbData); });

    return Base64Encode(std::string(reinterpret_cast<const char*>(output.pbData), output.cbData));
}

std::string RegistryService::Unprotect(const std::string& cipherBase64)
{
    auto decoded = Base64Decode(cipherBase64);

    DATA_BLOB input{};
    input.cbData = static_cast<DWORD>(decoded.size());
    input.pbData = reinterpret_cast<BYTE*>(decoded.data());

    DATA_BLOB output{};
    THROW_IF_WIN32_BOOL_FALSE(CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output));
    auto cleanup = wil::scope_exit([&]() { LocalFree(output.pbData); });

    return std::string(reinterpret_cast<const char*>(output.pbData), output.cbData);
}

void RegistryService::FileStoreCredential(const std::string& serverAddress, const std::string& credential)
{
    auto handle = RetryOpenFileOnSharingViolation(CreateFileExclusive);

    ModifyFileStore(std::move(handle), [&](nlohmann::json& data) {
        data[serverAddress] = Protect(credential);
        return true;
    });
}

std::optional<std::string> RegistryService::FileGetCredential(const std::string& serverAddress)
{
    auto handle = RetryOpenFileOnSharingViolation(OpenFileShared);
    auto data = ReadJsonFile(handle);

    const auto entry = data.find(serverAddress);
    if (entry == data.end() || !entry->is_string())
    {
        return std::nullopt;
    }

    return Unprotect(entry->get<std::string>());
}

void RegistryService::FileEraseCredential(const std::string& serverAddress)
{
    auto handle = RetryOpenFileOnSharingViolation(OpenFileExclusive);
    if (!handle.is_valid())
    {
        THROW_HR_WITH_USER_ERROR(E_NOT_SET, Localization::WSLCCLI_LogoutNotFound(wsl::shared::string::MultiByteToWide(serverAddress)));
    }

    bool erased = false;
    ModifyFileStore(std::move(handle), [&](nlohmann::json& data) {
        erased = data.erase(serverAddress) > 0;
        return erased;
    });

    THROW_HR_WITH_USER_ERROR_IF(E_NOT_SET, Localization::WSLCCLI_LogoutNotFound(wsl::shared::string::MultiByteToWide(serverAddress)), !erased);
}

std::vector<std::wstring> RegistryService::FileListCredentials()
{
    auto handle = RetryOpenFileOnSharingViolation(OpenFileShared);
    auto data = ReadJsonFile(handle);

    std::vector<std::wstring> result;

    for (const auto& [key, value] : data.items())
    {
        if (value.is_string())
        {
            result.push_back(wsl::shared::string::MultiByteToWide(key));
        }
    }

    return result;
}

std::string RegistryService::Authenticate(
    wsl::windows::wslc::models::Session& session, const std::string& serverAddress, const std::string& username, const std::string& password)
{
    wil::unique_cotaskmem_ansistring identityToken;
    THROW_IF_FAILED(session.Get()->Authenticate(serverAddress.c_str(), username.c_str(), password.c_str(), &identityToken));

    // If the registry returned an identity token, use it. Otherwise fall back to username/password.
    if (identityToken && strlen(identityToken.get()) > 0)
    {
        return BuildRegistryAuthHeader(identityToken.get(), serverAddress);
    }

    return BuildRegistryAuthHeader(username, password, serverAddress);
}

} // namespace wsl::windows::wslc::services
