// Copyright (c) Microsoft. All rights reserved.
#include "precomp.h"
#include "DeletedDistributionStore.h"
#include "retryshared.h"

using namespace wsl::windows::common;
using namespace wsl::windows::common::registry;

namespace {
constexpr auto RecoveryPath = L"RecoveryPath";
constexpr auto RecoveryFileId = L"RecoveryFileId";
constexpr auto DeletedAt = L"DeletedAt";
constexpr auto PreviousState = L"RecoveryPreviousState";
constexpr auto Restored = L"RecoveryRestored";
constexpr std::wstring_view DeletedPrefix = L"Deleted-";

std::wstring KeyName(const GUID& id, bool deleted = false)
{
    return (deleted ? std::wstring(DeletedPrefix) : L"") + wsl::shared::string::GuidToString<wchar_t>(id);
}

wil::unique_hfile OpenDisk(const std::filesystem::path& path)
{
    wil::unique_hfile file{CreateFileW(
        path.c_str(), DELETE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    if (!file)
    {
        const auto error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
        {
            return {};
        }
        THROW_WIN32(error);
    }
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    THROW_IF_WIN32_BOOL_FALSE(GetFileInformationByHandleEx(file.get(), FileAttributeTagInfo, &attributes, sizeof(attributes)));
    THROW_HR_IF(
        HRESULT_FROM_WIN32(ERROR_REPARSE_TAG_INVALID),
        WI_IsAnyFlagSet(attributes.FileAttributes, FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY));
    return file;
}

FILE_ID_INFO Identity(HANDLE file)
{
    FILE_ID_INFO id{};
    THROW_IF_WIN32_BOOL_FALSE(GetFileInformationByHandleEx(file, FileIdInfo, &id, sizeof(id)));
    return id;
}

void VerifyIdentity(HKEY key, HANDLE file)
{
    FILE_ID_INFO expected{};
    DWORD size = sizeof(expected);
    THROW_IF_WIN32_ERROR(RegGetValueW(key, nullptr, RecoveryFileId, RRF_RT_REG_BINARY, nullptr, &expected, &size));
    const auto actual = Identity(file);
    THROW_HR_IF(
        HRESULT_FROM_WIN32(ERROR_FILE_INVALID),
        size != sizeof(expected) || expected.VolumeSerialNumber != actual.VolumeSerialNumber ||
            memcmp(&expected.FileId, &actual.FileId, sizeof(expected.FileId)) != 0);
}

void RenameDisk(HANDLE file, const std::filesystem::path& path)
{
    const auto name = path.wstring();
    const auto nameSize = name.size() * sizeof(wchar_t);
    std::vector<BYTE> buffer(sizeof(FILE_RENAME_INFO) + nameSize);
    auto info = reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
    info->ReplaceIfExists = FALSE;
    info->RootDirectory = nullptr;
    info->FileNameLength = gsl::narrow<DWORD>(nameSize);
    memcpy(info->FileName, name.data(), nameSize);
    THROW_IF_WIN32_BOOL_FALSE(SetFileInformationByHandle(file, FileRenameInfo, info, gsl::narrow<DWORD>(buffer.size())));
}

bool IsRegisteredDisk(HKEY lxssKey, HANDLE file)
{
    const auto retainedId = Identity(file);
    for (const auto& [id, name] : EnumGuidKeys(lxssKey))
    {
        try
        {
            const auto key = OpenKey(lxssKey, name.c_str(), KEY_READ);
            const auto path = std::filesystem::path(ReadString(key.get(), nullptr, L"BasePath")) /
                              ReadString(key.get(), nullptr, L"VhdFileName", LXSS_VM_MODE_VHD_NAME);
            const wil::unique_hfile active{CreateFileW(
                path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr)};
            if (active)
            {
                const auto activeId = Identity(active.get());
                if (activeId.VolumeSerialNumber == retainedId.VolumeSerialNumber &&
                    memcmp(&activeId.FileId, &retainedId.FileId, sizeof(activeId.FileId)) == 0)
                {
                    return true;
                }
            }
            else
            {
                const auto error = GetLastError();
                if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
                {
                    // Cannot establish that this is unrelated to an active registration; retry later.
                    return true;
                }
            }
        }
        catch (...)
        {
            LOG_CAUGHT_EXCEPTION();
            return true;
        }
    }
    return false;
}

void ClearRecoveryValues(HKEY key)
{
    for (const auto value : {RecoveryPath, RecoveryFileId, DeletedAt, Restored, PreviousState})
    {
        DeleteValue(key, value);
    }
}
} // namespace

ULONG64 DeletedDistributionStore::Now()
{
    FILETIME time{};
    GetSystemTimeAsFileTime(&time);
    return (static_cast<ULONG64>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}

bool DeletedDistributionStore::Retain(HKEY lxssKey, const GUID& id, const std::filesystem::path& vhdPath)
{
    // HCS may briefly keep a handle after ejecting the disk. Match the existing
    // unregister retry window for sharing violations rather than failing a normal teardown.
    auto file = wsl::shared::retry::RetryWithTimeout<wil::unique_hfile>(
        [&] { return OpenDisk(vhdPath); }, std::chrono::milliseconds(100), std::chrono::seconds(10), {HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION)});
    if (!file)
    {
        // Broken registrations with no filesystem must still be removable.
        return false;
    }

    std::wstring resolvedPath;
    THROW_IF_FAILED(wil::GetFinalPathNameByHandleW(file.get(), resolvedPath));
    const std::filesystem::path originalPath{resolvedPath};
    GUID storageId{};
    THROW_IF_FAILED(CoCreateGuid(&storageId));
    // Stay on the disk's volume: an atomic rename avoids copying a potentially huge VHD.
    // Use a sibling so reinstalling into or removing the original directory cannot delete the recovery copy.
    const auto directory =
        originalPath.parent_path().parent_path() / (L".wsl-recovery-" + wsl::shared::string::GuidToString<wchar_t>(storageId));
    THROW_IF_WIN32_BOOL_FALSE(CreateDirectoryW(directory.c_str(), nullptr));
    const auto target = directory / originalPath.filename();
    auto removeEmptyDirectory = wil::scope_exit([&] { RemoveDirectoryW(directory.c_str()); });
    const auto key = OpenKey(lxssKey, KeyName(id).c_str(), KEY_READ | KEY_WRITE);
    const auto originalState = ReadDword(key.get(), nullptr, L"State", LxssDistributionStateInstalled);
    bool moved = false;
    auto rollback = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&] {
        if (moved)
        {
            RenameDisk(file.get(), originalPath);
        }
        WriteDword(key.get(), nullptr, L"State", originalState);
        ClearRecoveryValues(key.get());
    });

    const auto identity = Identity(file.get());
    WriteDword(key.get(), nullptr, PreviousState, originalState);
    WriteString(key.get(), nullptr, RecoveryPath, target.c_str());
    WriteQword(key.get(), nullptr, DeletedAt, Now());
    THROW_IF_WIN32_ERROR(RegSetValueExW(key.get(), RecoveryFileId, 0, REG_BINARY, reinterpret_cast<const BYTE*>(&identity), sizeof(identity)));
    // Persist the journal before moving the file. Startup repairs an interrupted transition.
    THROW_IF_WIN32_ERROR(RegFlushKey(key.get()));
    RenameDisk(file.get(), target);
    moved = true;
    WriteQword(key.get(), nullptr, DeletedAt, Now());
    WriteDword(key.get(), nullptr, L"State", LxssDistributionStateDeleted);
    THROW_IF_WIN32_ERROR(RegRenameKey(lxssKey, KeyName(id).c_str(), KeyName(id, true).c_str()));
    rollback.release();
    removeEmptyDirectory.release();
    THROW_IF_WIN32_ERROR(RegFlushKey(key.get()));
    return true;
}

std::vector<DeletedDistributionStore::Entry> DeletedDistributionStore::Enumerate(HKEY lxssKey)
{
    std::vector<Entry> result;
    for (const auto& [name, key] : EnumKeys(lxssKey, KEY_READ))
    {
        if (!name.starts_with(DeletedPrefix))
        {
            continue;
        }
        try
        {
            const auto id = wsl::shared::string::ToGuid(name.substr(DeletedPrefix.size()));
            THROW_HR_IF(E_INVALIDARG, !id);
            result.push_back(
                {*id,
                 ReadString(key.get(), nullptr, L"DistributionName"),
                 ReadString(key.get(), nullptr, RecoveryPath),
                 ReadQword(key.get(), nullptr, DeletedAt, 0)});
        }
        CATCH_LOG()
    }
    return result;
}

void DeletedDistributionStore::Restore(HKEY lxssKey, const Entry& distribution, LPCWSTR name)
{
    const auto key = OpenKey(lxssKey, KeyName(distribution.Id, true).c_str(), KEY_READ | KEY_WRITE);
    auto file = OpenDisk(distribution.Path);
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND), !file);
    VerifyIdentity(key.get(), file.get());
    const auto now = Now();
    THROW_HR_IF(
        HRESULT_FROM_WIN32(ERROR_TIMEOUT),
        distribution.DeletedAt != 0 && now >= distribution.DeletedAt && now - distribution.DeletedAt >= Retention);

    // Keep the disk at its recovery location. Restoring never overwrites the original
    // path, which may now contain a replacement distribution or other user files.
    WriteString(key.get(), nullptr, L"BasePath", distribution.Path.parent_path().c_str());
    WriteString(key.get(), nullptr, L"VhdFileName", distribution.Path.filename().c_str());
    WriteString(key.get(), nullptr, L"DistributionName", name);
    // A restored store distro becomes independently managed, like an imported VHD.
    for (const auto value : {L"PackageFamilyName", L"ShortcutPath", L"TerminalProfilePath"})
    {
        DeleteValue(key.get(), value);
    }
    WriteDword(key.get(), nullptr, Restored, 1);
    WriteDword(key.get(), nullptr, L"State", LxssDistributionStateInstalled);
    THROW_IF_WIN32_ERROR(RegFlushKey(key.get()));
    THROW_IF_WIN32_ERROR(RegRenameKey(lxssKey, KeyName(distribution.Id, true).c_str(), KeyName(distribution.Id).c_str()));
    THROW_IF_WIN32_ERROR(RegFlushKey(key.get()));
    // Atomic key rename removes the disk from the cleanup set before success is reported.
    try
    {
        ClearRecoveryValues(key.get());
    }
    CATCH_LOG()
}

void DeletedDistributionStore::RecoverPending(HKEY lxssKey) noexcept
try
{
    for (const auto& [id, name] : EnumGuidKeys(lxssKey))
    {
        try
        {
            const auto key = OpenKey(lxssKey, name.c_str(), KEY_READ | KEY_WRITE);
            const auto path = ReadOptionalString(key.get(), nullptr, RecoveryPath);
            if (!path)
            {
                continue;
            }
            if (ReadDword(key.get(), nullptr, Restored, 0))
            {
                ClearRecoveryValues(key.get());
                continue;
            }
            auto file = OpenDisk(*path);
            if (file)
            {
                VerifyIdentity(key.get(), file.get());
                WriteDword(key.get(), nullptr, L"State", LxssDistributionStateDeleted);
                THROW_IF_WIN32_ERROR(RegRenameKey(lxssKey, name.c_str(), KeyName(id, true).c_str()));
            }
            else
            {
                // A missing recovery path can also mean its volume is offline. Only
                // roll back the journal when the original disk is positively identified.
                const auto originalPath = std::filesystem::path(ReadString(key.get(), nullptr, L"BasePath")) /
                                          ReadString(key.get(), nullptr, L"VhdFileName", LXSS_VM_MODE_VHD_NAME);
                auto original = OpenDisk(originalPath);
                if (original)
                {
                    VerifyIdentity(key.get(), original.get());
                    WriteDword(key.get(), nullptr, L"State", ReadDword(key.get(), nullptr, PreviousState, LxssDistributionStateInstalled));
                    ClearRecoveryValues(key.get());
                }
            }
        }
        CATCH_LOG()
    }

    // An interrupted unregister may have committed the key rename before choosing a new default.
    const auto defaultName = ReadOptionalString(lxssKey, nullptr, L"DefaultDistribution");
    if (defaultName && FAILED(OpenKeyNoThrow(lxssKey, defaultName->c_str(), KEY_READ).second) &&
        SUCCEEDED(OpenKeyNoThrow(lxssKey, (std::wstring(DeletedPrefix) + *defaultName).c_str(), KEY_READ).second))
    {
        DeleteValue(lxssKey, L"DefaultDistribution");
        for (const auto& [id, name] : EnumGuidKeys(lxssKey))
        {
            const auto key = OpenKey(lxssKey, name.c_str(), KEY_READ);
            if (ReadDword(key.get(), nullptr, L"State", LxssDistributionStateInvalid) == LxssDistributionStateInstalled)
            {
                WriteString(lxssKey, nullptr, L"DefaultDistribution", name.c_str());
                break;
            }
        }
    }
}
CATCH_LOG()

void DeletedDistributionStore::Cleanup(HKEY lxssKey, ULONG64 currentTime) noexcept
try
{
    for (const auto& entry : Enumerate(lxssKey))
    {
        // Missing/corrupt timestamps and clocks moving backwards never cause early deletion.
        if (entry.DeletedAt == 0 || currentTime < entry.DeletedAt || currentTime - entry.DeletedAt < Retention)
        {
            continue;
        }
        try
        {
            const auto key = OpenKey(lxssKey, KeyName(entry.Id, true).c_str(), KEY_READ);
            auto file = OpenDisk(entry.Path);
            if (file)
            {
                VerifyIdentity(key.get(), file.get());
                if (IsRegisteredDisk(lxssKey, file.get()))
                {
                    continue;
                }
                FILE_DISPOSITION_INFO disposition{TRUE};
                THROW_IF_WIN32_BOOL_FALSE(SetFileInformationByHandle(file.get(), FileDispositionInfo, &disposition, sizeof(disposition)));
                file.reset();
            }
            else
            {
                const wil::unique_hfile directory{CreateFileW(
                    entry.Path.parent_path().c_str(),
                    FILE_READ_ATTRIBUTES,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr,
                    OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                    nullptr)};
                if (!directory)
                {
                    continue;
                }
                FILE_ATTRIBUTE_TAG_INFO attributes{};
                THROW_IF_WIN32_BOOL_FALSE(
                    GetFileInformationByHandleEx(directory.get(), FileAttributeTagInfo, &attributes, sizeof(attributes)));
                if (WI_IsFlagSet(attributes.FileAttributes, FILE_ATTRIBUTE_REPARSE_POINT))
                {
                    continue;
                }
                FILE_ID_INFO expected{};
                DWORD size = sizeof(expected);
                THROW_IF_WIN32_ERROR(RegGetValueW(key.get(), nullptr, RecoveryFileId, RRF_RT_REG_BINARY, nullptr, &expected, &size));
                if (size != sizeof(expected) || Identity(directory.get()).VolumeSerialNumber != expected.VolumeSerialNumber)
                {
                    continue;
                }
            }
            // Never recursively delete a directory or revisit the original distribution path.
            RemoveDirectoryW(entry.Path.parent_path().c_str());
            DeleteKey(lxssKey, KeyName(entry.Id, true).c_str());
        }
        CATCH_LOG()
    }
}
CATCH_LOG()
