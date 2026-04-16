/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    FileCredStorage.cpp

Abstract:

    DPAPI-encrypted JSON file credential storage implementation.

--*/

#include "precomp.h"
#include "FileCredStorage.h"

using wsl::shared::Localization;

using namespace wsl::shared;
using namespace wsl::windows::common::wslutil;
using namespace wsl::windows::wslc::services;

namespace {

std::filesystem::path GetFilePath()
{
    return wsl::windows::common::filesystem::GetLocalAppDataPath(nullptr) / L"wslc" / L"registry-credentials.json";
}

wil::unique_file RetryOpenFileOnSharingViolation(const std::function<wil::unique_file()>& openFunc)
{
    try
    {
        return wsl::shared::retry::RetryWithTimeout<wil::unique_file>(openFunc, std::chrono::milliseconds(100), std::chrono::seconds(1), []() {
            return wil::ResultFromCaughtException() == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION);
        });
    }
    catch (...)
    {
        auto result = wil::ResultFromCaughtException();
        auto errorString = wsl::windows::common::wslutil::GetSystemErrorString(result);
        THROW_HR_WITH_USER_ERROR(result, Localization::MessageWslcFailedToOpenFile(GetFilePath(), errorString));
    }
}

wil::unique_file OpenFileExclusive()
{
    wil::unique_file f(_wfsopen(GetFilePath().c_str(), L"r+b", _SH_DENYRW));
    if (f)
    {
        return f;
    }

    auto dosError = _doserrno;
    if (dosError == ERROR_FILE_NOT_FOUND || dosError == ERROR_PATH_NOT_FOUND)
    {
        return nullptr;
    }

    THROW_WIN32(dosError);
}

wil::unique_file CreateFileExclusive()
{
    auto filePath = GetFilePath();
    std::filesystem::create_directories(filePath.parent_path());

    // Use _wsopen_s with _O_CREAT to atomically create-or-open without truncation.
    int fd = -1;
    auto err = _wsopen_s(&fd, filePath.c_str(), _O_RDWR | _O_CREAT | _O_BINARY, _SH_DENYRW, _S_IREAD | _S_IWRITE);
    THROW_WIN32_IF(_doserrno, err != 0);

    wil::unique_file f(_fdopen(fd, "r+b"));
    if (!f)
    {
        _close(fd);
        THROW_WIN32(_doserrno);
    }

    return f;
}

wil::unique_file OpenFileShared()
{
    wil::unique_file f(_wfsopen(GetFilePath().c_str(), L"rb", _SH_DENYWR));
    if (f)
    {
        return f;
    }

    auto dosError = _doserrno;
    if (dosError == ERROR_FILE_NOT_FOUND || dosError == ERROR_PATH_NOT_FOUND)
    {
        return nullptr;
    }

    THROW_WIN32(dosError);
}

CredentialFile ReadCredentialFile(FILE* f)
{
    WI_ASSERT(f != nullptr);

    auto seekResult = fseek(f, 0, SEEK_SET);
    THROW_HR_WITH_USER_ERROR_IF(E_FAIL, Localization::MessageWslcFailedToOpenFile(GetFilePath(), _wcserror(errno)), seekResult != 0);

    // Handle newly created empty files (from CreateFileExclusive).
    if (_filelengthi64(_fileno(f)) <= 0)
    {
        return {};
    }

    try
    {
        return nlohmann::json::parse(f).get<CredentialFile>();
    }
    catch (const nlohmann::json::exception&)
    {
        THROW_HR_WITH_USER_ERROR(WSL_E_INVALID_JSON, Localization::WSLCCLI_CredentialFileCorrupt(GetFilePath()));
    }
}

void WriteCredentialFile(FILE* f, const CredentialFile& data)
{
    auto error = fseek(f, 0, SEEK_SET);
    THROW_HR_WITH_USER_ERROR_IF(E_FAIL, Localization::MessageWslcFailedToOpenFile(GetFilePath(), _wcserror(errno)), error != 0);

    error = _chsize_s(_fileno(f), 0);
    THROW_HR_WITH_USER_ERROR_IF(
        HRESULT_FROM_WIN32(_doserrno),
        Localization::MessageWslcFailedToOpenFile(GetFilePath(), GetSystemErrorString(HRESULT_FROM_WIN32(_doserrno))),
        error != 0);

    auto content = nlohmann::json(data).dump(2);
    auto written = fwrite(content.data(), 1, content.size(), f);
    THROW_HR_WITH_USER_ERROR_IF(
        E_FAIL, Localization::MessageWslcFailedToOpenFile(GetFilePath(), _wcserror(errno)), written != content.size());
}

void ModifyFileStore(FILE* f, const std::function<bool(CredentialFile&)>& modifier)
{
    auto data = ReadCredentialFile(f);

    if (modifier(data))
    {
        WriteCredentialFile(f, data);
    }
}

std::string Protect(const std::string& plaintext)
{
    DATA_BLOB input{};
    input.cbData = static_cast<DWORD>(plaintext.size());
    input.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(plaintext.data()));

    DATA_BLOB output{};
    THROW_IF_WIN32_BOOL_FALSE(CryptProtectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output));
    auto cleanup = wil::scope_exit([&]() { LocalFree(output.pbData); });

    return Base64Encode(std::string(reinterpret_cast<const char*>(output.pbData), output.cbData));
}

std::string Unprotect(const std::string& cipherBase64)
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

} // namespace

namespace wsl::windows::wslc::services {

void FileCredStorage::Store(const std::string& serverAddress, const std::string& username, const std::string& secret)
{
    auto file = RetryOpenFileOnSharingViolation(CreateFileExclusive);

    ModifyFileStore(file.get(), [&](CredentialFile& data) {
        data.Credentials[serverAddress] = CredentialEntry{username, Protect(secret)};
        return true;
    });
}

std::pair<std::string, std::string> FileCredStorage::Get(const std::string& serverAddress)
{
    auto file = RetryOpenFileOnSharingViolation(OpenFileShared);
    if (!file)
    {
        return {};
    }

    auto data = ReadCredentialFile(file.get());
    const auto entry = data.Credentials.find(serverAddress);

    if (entry == data.Credentials.end())
    {
        return {};
    }

    return {entry->second.UserName, Unprotect(entry->second.Secret)};
}

void FileCredStorage::Erase(const std::string& serverAddress)
{
    auto file = RetryOpenFileOnSharingViolation(OpenFileExclusive);
    bool erased = false;

    if (file)
    {
        ModifyFileStore(file.get(), [&](CredentialFile& data) {
            erased = data.Credentials.erase(serverAddress) > 0;
            return erased;
        });
    }

    THROW_HR_WITH_USER_ERROR_IF(E_NOT_SET, Localization::WSLCCLI_LogoutNotFound(wsl::shared::string::MultiByteToWide(serverAddress)), !erased);
}

std::vector<std::wstring> FileCredStorage::List()
{
    auto file = RetryOpenFileOnSharingViolation(OpenFileShared);
    if (!file)
    {
        return {};
    }

    auto data = ReadCredentialFile(file.get());

    std::vector<std::wstring> result;

    for (const auto& [key, value] : data.Credentials)
    {
        result.push_back(wsl::shared::string::MultiByteToWide(key));
    }

    return result;
}

} // namespace wsl::windows::wslc::services
