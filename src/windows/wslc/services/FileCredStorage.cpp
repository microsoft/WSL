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
    THROW_IF_WIN32_ERROR(err);

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

nlohmann::json ReadJsonFile(FILE* f)
{
    if (!f)
    {
        return nlohmann::json::object();
    }

    fseek(f, 0, SEEK_SET);

    // Handle newly created empty files (from CreateFileExclusive).
    if (_filelengthi64(_fileno(f)) <= 0)
    {
        return nlohmann::json::object();
    }

    try
    {
        return nlohmann::json::parse(f);
    }
    catch (const nlohmann::json::parse_error&)
    {
        THROW_HR_WITH_USER_ERROR(WSL_E_INVALID_JSON, Localization::WSLCCLI_CredentialFileCorrupt(GetFilePath()));
    }
}

void WriteJsonFile(FILE* f, const nlohmann::json& data)
{
    fseek(f, 0, SEEK_SET);
    _chsize_s(_fileno(f), 0);

    auto content = data.dump(2);
    fwrite(content.data(), 1, content.size(), f);
    fflush(f);
}

void ModifyFileStore(FILE* f, const std::function<bool(nlohmann::json&)>& modifier)
{
    WI_VERIFY(f != nullptr);

    auto data = ReadJsonFile(f);

    if (modifier(data))
    {
        WriteJsonFile(f, data);
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

void FileCredStorage::Store(const std::string& serverAddress, const std::string& credential)
{
    auto file = RetryOpenFileOnSharingViolation(CreateFileExclusive);

    ModifyFileStore(file.get(), [&](nlohmann::json& data) {
        data[serverAddress] = Protect(credential);
        return true;
    });
}

std::optional<std::string> FileCredStorage::Get(const std::string& serverAddress)
{
    auto file = RetryOpenFileOnSharingViolation(OpenFileShared);
    auto data = ReadJsonFile(file.get());

    const auto entry = data.find(serverAddress);
    if (entry == data.end() || !entry->is_string())
    {
        return std::nullopt;
    }

    return Unprotect(entry->get<std::string>());
}

void FileCredStorage::Erase(const std::string& serverAddress)
{
    auto file = RetryOpenFileOnSharingViolation(OpenFileExclusive);
    if (!file)
    {
        THROW_HR_WITH_USER_ERROR(E_NOT_SET, Localization::WSLCCLI_LogoutNotFound(wsl::shared::string::MultiByteToWide(serverAddress)));
    }

    bool erased = false;
    ModifyFileStore(file.get(), [&](nlohmann::json& data) {
        erased = data.erase(serverAddress) > 0;
        return erased;
    });

    THROW_HR_WITH_USER_ERROR_IF(E_NOT_SET, Localization::WSLCCLI_LogoutNotFound(wsl::shared::string::MultiByteToWide(serverAddress)), !erased);
}

std::vector<std::wstring> FileCredStorage::List()
{
    auto file = RetryOpenFileOnSharingViolation(OpenFileShared);
    auto data = ReadJsonFile(file.get());

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

} // namespace wsl::windows::wslc::services
