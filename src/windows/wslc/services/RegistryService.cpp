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
#include <wslutil.h>
#include <wincred.h>

using namespace wsl::shared;
using namespace wsl::windows::common::wslutil;

namespace {

wil::unique_hfile OpenJsonFileExclusive(const std::filesystem::path& path)
{
    std::filesystem::create_directories(path.parent_path());

    wil::unique_hfile handle(CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    THROW_LAST_ERROR_IF(!handle.is_valid());
    return handle;
}

wil::unique_hfile OpenJsonFileShared(const std::filesystem::path& path)
{
    wil::unique_hfile handle(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));

    if (!handle.is_valid())
    {
        THROW_LAST_ERROR_IF(GetLastError() != ERROR_FILE_NOT_FOUND);
    }

    return handle;
}

nlohmann::json ReadJsonFile(const wil::unique_hfile& handle)
{
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

} // namespace

namespace wsl::windows::wslc::services {

void RegistryService::Store(CredentialStoreType backend, const std::string& serverAddress, const std::string& credential)
{
    THROW_HR_IF(E_INVALIDARG, serverAddress.empty());
    THROW_HR_IF(E_INVALIDARG, credential.empty());

    backend == CredentialStoreType::File ? FileStoreCredential(serverAddress, credential)
                                         : WinCredStoreCredential(serverAddress, credential);
}

std::optional<std::string> RegistryService::Get(CredentialStoreType backend, const std::string& serverAddress)
{
    if (serverAddress.empty())
    {
        return std::nullopt;
    }

    return backend == CredentialStoreType::File ? FileGetCredential(serverAddress) : WinCredGetCredential(serverAddress);
}

void RegistryService::Erase(CredentialStoreType backend, const std::string& serverAddress)
{
    THROW_HR_IF(E_INVALIDARG, serverAddress.empty());

    backend == CredentialStoreType::File ? FileEraseCredential(serverAddress) : WinCredEraseCredential(serverAddress);
}

std::vector<std::string> RegistryService::List(CredentialStoreType backend)
{
    return backend == CredentialStoreType::File ? FileListCredentials() : WinCredListCredentials();
}

// --- WinCred backend ---

void RegistryService::WinCredStoreCredential(const std::string& serverAddress, const std::string& credential)
{
    auto targetName = wsl::shared::string::MultiByteToWide(serverAddress);

    CREDENTIALW cred{};
    cred.Type = CRED_TYPE_GENERIC;
    cred.TargetName = targetName.data();
    cred.CredentialBlobSize = static_cast<DWORD>(credential.size());
    cred.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char*>(credential.data()));
    cred.Persist = CRED_PERSIST_LOCAL_MACHINE;

    THROW_IF_WIN32_BOOL_FALSE(CredWriteW(&cred, 0));
}

std::optional<std::string> RegistryService::WinCredGetCredential(const std::string& serverAddress)
{
    auto targetName = wsl::shared::string::MultiByteToWide(serverAddress);

    PCREDENTIALW cred = nullptr;
    if (!CredReadW(targetName.c_str(), CRED_TYPE_GENERIC, 0, &cred))
    {
        if (GetLastError() == ERROR_NOT_FOUND)
        {
            return std::nullopt;
        }

        THROW_LAST_ERROR();
    }

    auto cleanup = wil::scope_exit([&]() { CredFree(cred); });

    if (cred->CredentialBlobSize == 0 || cred->CredentialBlob == nullptr)
    {
        return std::nullopt;
    }

    return std::string(reinterpret_cast<const char*>(cred->CredentialBlob), cred->CredentialBlobSize);
}

void RegistryService::WinCredEraseCredential(const std::string& serverAddress)
{
    auto targetName = wsl::shared::string::MultiByteToWide(serverAddress);

    if (!CredDeleteW(targetName.c_str(), CRED_TYPE_GENERIC, 0))
    {
        auto error = GetLastError();
        THROW_HR_WITH_USER_ERROR_IF(
            E_NOT_SET, Localization::WSLCCLI_LogoutNotFound(wsl::shared::string::MultiByteToWide(serverAddress)), error == ERROR_NOT_FOUND);

        THROW_WIN32(error);
    }
}

std::vector<std::string> RegistryService::WinCredListCredentials()
{
    DWORD count = 0;
    PCREDENTIALW* creds = nullptr;
    if (!CredEnumerateW(nullptr, 0, &count, &creds))
    {
        if (GetLastError() == ERROR_NOT_FOUND)
        {
            return {};
        }

        THROW_LAST_ERROR();
    }

    auto cleanup = wil::scope_exit([&]() { CredFree(creds); });

    std::vector<std::string> result;
    result.reserve(count);
    for (DWORD i = 0; i < count; ++i)
    {
        result.push_back(wsl::shared::string::WideToMultiByte(creds[i]->TargetName));
    }

    return result;
}

// --- File backend ---

std::filesystem::path RegistryService::GetFilePath()
{
    return wsl::windows::common::filesystem::GetLocalAppDataPath(nullptr) / L"wslc" / L"registry-credentials.json";
}

nlohmann::json RegistryService::ReadFileStore()
{
    auto handle = OpenJsonFileShared(GetFilePath());
    if (!handle.is_valid())
    {
        return nlohmann::json::object();
    }

    return ReadJsonFile(handle);
}

void RegistryService::ModifyFileStore(const std::function<bool(nlohmann::json&)>& modifier)
{
    auto handle = OpenJsonFileExclusive(GetFilePath());
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
    ModifyFileStore([&](nlohmann::json& data) {
        data["registries"][serverAddress] = {{"credential", Protect(credential)}};
        return true;
    });
}

std::optional<std::string> RegistryService::FileGetCredential(const std::string& serverAddress)
{
    auto data = ReadFileStore();
    const auto registries = data.find("registries");
    if (registries == data.end() || !registries->is_object())
    {
        return std::nullopt;
    }

    const auto entry = registries->find(serverAddress);
    if (entry == registries->end() || !entry->is_object())
    {
        return std::nullopt;
    }

    const auto cred = entry->find("credential");
    if (cred == entry->end() || !cred->is_string())
    {
        return std::nullopt;
    }

    try
    {
        return Unprotect(cred->get<std::string>());
    }
    catch (...)
    {
        LOG_CAUGHT_EXCEPTION_MSG("Failed to decrypt credential for %hs", serverAddress.c_str());
        return std::nullopt;
    }
}

void RegistryService::FileEraseCredential(const std::string& serverAddress)
{
    bool erased = false;
    ModifyFileStore([&](nlohmann::json& data) {
        auto registries = data.find("registries");
        if (registries == data.end() || !registries->is_object())
        {
            return false;
        }

        erased = registries->erase(serverAddress) > 0;
        return erased;
    });

    THROW_HR_WITH_USER_ERROR_IF(E_NOT_SET, Localization::WSLCCLI_LogoutNotFound(wsl::shared::string::MultiByteToWide(serverAddress)), !erased);
}

std::vector<std::string> RegistryService::FileListCredentials()
{
    std::vector<std::string> result;
    auto data = ReadFileStore();
    const auto registries = data.find("registries");
    if (registries != data.end() && registries->is_object())
    {
        for (const auto& [key, _] : registries->items())
        {
            result.push_back(key);
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
