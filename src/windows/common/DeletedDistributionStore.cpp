// Copyright (C) Microsoft Corporation. All rights reserved.
#include "precomp.h"
#include "DeletedDistributionStore.h"
#include "retryshared.h"

using namespace wsl::windows::common;
using namespace wsl::windows::common::registry;

namespace {
constexpr auto RecoveryPath = L"RecoveryPath";
constexpr auto RecoveryFileId = L"RecoveryFileId";
constexpr auto RecoveryDirectoryId = L"RecoveryDirectoryId";
constexpr auto RecoveryAnchorId = L"RecoveryAnchorId";
constexpr auto DeletedAt = L"DeletedAt";
constexpr auto PreviousState = L"RecoveryPreviousState";
constexpr auto Restored = L"RecoveryRestored";
constexpr auto RestorePending = L"RecoveryRestorePending";
constexpr auto RestoreName = L"RecoveryRestoreName";
constexpr auto CleanupPending = L"RecoveryCleanupPending";
constexpr std::wstring_view DeletedPrefix = L"Deleted-";

std::wstring KeyName(const GUID& id, bool deleted = false)
{
    return (deleted ? std::wstring(DeletedPrefix) : L"") + wsl::shared::string::GuidToString<wchar_t>(id);
}

wil::unique_hfile OpenDisk(const std::filesystem::path& path, bool allowMissingParent = true)
{
    wil::unique_hfile file{CreateFileW(
        path.c_str(), DELETE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    if (!file)
    {
        const auto error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || (allowMissingParent && error == ERROR_PATH_NOT_FOUND))
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

wil::unique_hfile OpenDirectory(const std::filesystem::path& path, DWORD sharing = FILE_SHARE_READ, bool allowMissing = false)
{
    wil::unique_hfile directory{CreateFileW(
        path.c_str(),
        DELETE | FILE_READ_ATTRIBUTES,
        sharing,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr)};
    if (!directory)
    {
        const auto error = GetLastError();
        if (allowMissing && error == ERROR_FILE_NOT_FOUND)
        {
            return {};
        }
        THROW_WIN32(error);
    }
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    THROW_IF_WIN32_BOOL_FALSE(GetFileInformationByHandleEx(directory.get(), FileAttributeTagInfo, &attributes, sizeof(attributes)));
    THROW_HR_IF(
        HRESULT_FROM_WIN32(ERROR_REPARSE_TAG_INVALID),
        WI_IsFlagSet(attributes.FileAttributes, FILE_ATTRIBUTE_REPARSE_POINT) ||
            !WI_IsFlagSet(attributes.FileAttributes, FILE_ATTRIBUTE_DIRECTORY));
    return directory;
}

FILE_ID_INFO Identity(HANDLE file)
{
    FILE_ID_INFO id{};
    THROW_IF_WIN32_BOOL_FALSE(GetFileInformationByHandleEx(file, FileIdInfo, &id, sizeof(id)));
    return id;
}

void VerifyIdentity(HKEY key, HANDLE file, LPCWSTR value = RecoveryFileId)
{
    FILE_ID_INFO expected{};
    DWORD size = sizeof(expected);
    THROW_IF_WIN32_ERROR(RegGetValueW(key, nullptr, value, RRF_RT_REG_BINARY, nullptr, &expected, &size));
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

void CommitRestore(HKEY lxssKey, const GUID& id)
{
    const auto defaultName = ReadOptionalString(lxssKey, nullptr, L"DefaultDistribution");
    const auto defaultId = defaultName ? wsl::shared::string::ToGuid(*defaultName) : std::nullopt;
    bool validDefault = false;
    if (defaultId)
    {
        const auto [key, result] = OpenKeyNoThrow(lxssKey, KeyName(*defaultId).c_str(), KEY_READ);
        validDefault = SUCCEEDED(result) &&
                       ReadDword(key.get(), nullptr, L"State", LxssDistributionStateInvalid) == LxssDistributionStateInstalled &&
                       !ReadOptionalString(key.get(), nullptr, RecoveryPath);
    }
    if (!validDefault)
    {
        WriteString(lxssKey, nullptr, L"DefaultDistribution", KeyName(id).c_str());
    }
    // Persist the registration and default selection before clearing the restore journal.
    THROW_IF_WIN32_ERROR(RegFlushKey(lxssKey));
}

void ClearRecoveryValues(HKEY key)
{
    for (const auto value : {RecoveryPath, RecoveryFileId, RecoveryDirectoryId, RecoveryAnchorId, DeletedAt, Restored, RestorePending, RestoreName, CleanupPending, PreviousState})
    {
        DeleteValue(key, value);
    }
}

void CompleteRestore(HKEY lxssKey, HKEY key, const GUID& id, bool deleted)
{
    const auto path = std::filesystem::path(ReadString(key, nullptr, RecoveryPath));
    const auto name = ReadString(key, nullptr, RestoreName);
    WriteString(key, nullptr, L"BasePath", path.parent_path().c_str());
    WriteString(key, nullptr, L"VhdFileName", path.filename().c_str());
    WriteString(key, nullptr, L"DistributionName", name.c_str());
    // A restored store distro becomes independently managed, like an imported VHD.
    for (const auto value : {L"PackageFamilyName", L"ShortcutPath", L"TerminalProfilePath"})
    {
        DeleteValue(key, value);
    }
    WriteDword(key, nullptr, Restored, 1);
    WriteDword(key, nullptr, L"State", LxssDistributionStateInstalled);
    THROW_IF_WIN32_ERROR(RegFlushKey(key));
    if (deleted)
    {
        THROW_IF_WIN32_ERROR(RegRenameKey(lxssKey, KeyName(id, true).c_str(), KeyName(id).c_str()));
    }
    CommitRestore(lxssKey, id);
    ClearRecoveryValues(key);
}

bool RecoveryDirectoryWasDeleted(HKEY key, const std::filesystem::path& directory)
{
    // Probe the directory itself, including a dangling link, without following
    // its reparse point. OpenDirectory rejects every existing reparse point.
    if (OpenDirectory(directory, FILE_SHARE_READ, true))
    {
        return false;
    }
    // A missing child is conclusive only while the original parent is available
    // and locked against replacement. An offline volume must keep its journal.
    FILE_ID_INFO expected{};
    DWORD size = sizeof(expected);
    THROW_IF_WIN32_ERROR(RegGetValueW(key, nullptr, RecoveryDirectoryId, RRF_RT_REG_BINARY, nullptr, &expected, &size));
    THROW_HR_IF(E_INVALIDARG, size != sizeof(expected));
    const auto anchor = OpenDirectory(directory.parent_path(), FILE_SHARE_READ | FILE_SHARE_WRITE);
    VerifyIdentity(key, anchor.get(), RecoveryAnchorId);
    const auto anchorId = Identity(anchor.get());
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_FILE_INVALID), expected.VolumeSerialNumber != anchorId.VolumeSerialNumber);
    return !OpenDirectory(directory, FILE_SHARE_READ, true);
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
        [&] { return OpenDisk(vhdPath, false); }, std::chrono::milliseconds(100), std::chrono::seconds(10), {HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION)});
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
    // Moving a file here opens its destination directory for write access. Keep
    // delete sharing disabled so the directory cannot be replaced during the move.
    const auto directoryHandle = OpenDirectory(directory, FILE_SHARE_READ | FILE_SHARE_WRITE);
    const auto anchorHandle = OpenDirectory(directory.parent_path(), FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE);
    const auto key = OpenKey(lxssKey, KeyName(id).c_str(), KEY_READ | KEY_WRITE);
    const auto originalState = ReadDword(key.get(), nullptr, L"State", LxssDistributionStateInstalled);
    bool moved = false;
    auto rollback = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&] {
        if (moved)
        {
            try
            {
                RenameDisk(file.get(), originalPath);
            }
            catch (...)
            {
                // Keep the journal intact; RecoverPending can finish the forward move.
                LOG_CAUGHT_EXCEPTION();
                return;
            }
        }
        WriteDword(key.get(), nullptr, L"State", originalState);
        ClearRecoveryValues(key.get());
    });

    const auto identity = Identity(file.get());
    WriteDword(key.get(), nullptr, PreviousState, originalState);
    WriteString(key.get(), nullptr, RecoveryPath, target.c_str());
    WriteQword(key.get(), nullptr, DeletedAt, Now());
    THROW_IF_WIN32_ERROR(RegSetValueExW(key.get(), RecoveryFileId, 0, REG_BINARY, reinterpret_cast<const BYTE*>(&identity), sizeof(identity)));
    const auto directoryIdentity = Identity(directoryHandle.get());
    THROW_IF_WIN32_ERROR(RegSetValueExW(
        key.get(), RecoveryDirectoryId, 0, REG_BINARY, reinterpret_cast<const BYTE*>(&directoryIdentity), sizeof(directoryIdentity)));
    const auto anchorIdentity = Identity(anchorHandle.get());
    THROW_IF_WIN32_ERROR(RegSetValueExW(
        key.get(), RecoveryAnchorId, 0, REG_BINARY, reinterpret_cast<const BYTE*>(&anchorIdentity), sizeof(anchorIdentity)));
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

    // Persist the intended restore before changing registration fields. Startup can
    // complete the operation even if the process stops between individual writes.
    WriteString(key.get(), nullptr, RestoreName, name);
    WriteDword(key.get(), nullptr, RestorePending, 1);
    THROW_IF_WIN32_ERROR(RegFlushKey(key.get()));
    CompleteRestore(lxssKey, key.get(), distribution.Id, true);
}

void DeletedDistributionStore::RecoverPending(HKEY lxssKey) noexcept
try
{
    auto registrations = EnumGuidKeys(lxssKey);
    for (const auto& entry : Enumerate(lxssKey))
    {
        registrations.emplace_back(entry.Id, KeyName(entry.Id, true));
    }
    for (const auto& [id, name] : registrations)
    {
        try
        {
            const auto key = OpenKey(lxssKey, name.c_str(), KEY_READ | KEY_WRITE);
            const bool pendingRestore = ReadDword(key.get(), nullptr, RestorePending, 0) != 0;
            if (pendingRestore || ReadDword(key.get(), nullptr, Restored, 0))
            {
                const auto path = ReadOptionalString(key.get(), nullptr, RecoveryPath);
                wil::unique_hfile file;
                if (path)
                {
                    file = OpenDisk(*path);
                    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND), !file);
                    VerifyIdentity(key.get(), file.get());
                }
                else
                {
                    THROW_HR_IF(
                        E_INVALIDARG,
                        name.starts_with(DeletedPrefix) ||
                            ReadDword(key.get(), nullptr, L"State", LxssDistributionStateInvalid) != LxssDistributionStateInstalled);
                }
                // Keep the verified disk handle open through registration and
                // journal updates so the file cannot be replaced during recovery.
                // Clearing the restore journal may have been interrupted after the
                // registration and default selection were durably committed.
                if (!path)
                {
                    CommitRestore(lxssKey, id);
                    ClearRecoveryValues(key.get());
                    continue;
                }
                if (pendingRestore)
                {
                    CompleteRestore(lxssKey, key.get(), id, name.starts_with(DeletedPrefix));
                }
                else
                {
                    WriteDword(key.get(), nullptr, L"State", LxssDistributionStateInstalled);
                    if (name.starts_with(DeletedPrefix))
                    {
                        THROW_IF_WIN32_ERROR(RegRenameKey(lxssKey, name.c_str(), KeyName(id).c_str()));
                    }
                    CommitRestore(lxssKey, id);
                    ClearRecoveryValues(key.get());
                }
                continue;
            }
            const auto path = ReadOptionalString(key.get(), nullptr, RecoveryPath);
            if (!path || name.starts_with(DeletedPrefix))
            {
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
                    const auto recoveryDirectory = std::filesystem::path(*path).parent_path();
                    if (!RecoveryDirectoryWasDeleted(key.get(), recoveryDirectory))
                    {
                        const auto directory = OpenDirectory(recoveryDirectory);
                        VerifyIdentity(key.get(), directory.get(), RecoveryDirectoryId);
                        FILE_DISPOSITION_INFO disposition{TRUE};
                        THROW_IF_WIN32_BOOL_FALSE(
                            SetFileInformationByHandle(directory.get(), FileDispositionInfo, &disposition, sizeof(disposition)));
                    }
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
            if (ReadDword(key.get(), nullptr, L"State", LxssDistributionStateInvalid) == LxssDistributionStateInstalled &&
                !ReadOptionalString(key.get(), nullptr, RecoveryPath))
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
            const auto key = OpenKey(lxssKey, KeyName(entry.Id, true).c_str(), KEY_READ | KEY_WRITE);
            // A failed or interrupted restore must never become eligible for deletion again.
            if (ReadDword(key.get(), nullptr, Restored, 0) || ReadDword(key.get(), nullptr, RestorePending, 0))
            {
                continue;
            }
            const auto recoveryDirectory = entry.Path.parent_path();
            if (ReadDword(key.get(), nullptr, CleanupPending, 0) && RecoveryDirectoryWasDeleted(key.get(), recoveryDirectory))
            {
                DeleteKey(lxssKey, KeyName(entry.Id, true).c_str());
                continue;
            }
            // Keep the verified directory locked against replacement until deletion completes.
            const auto directory = OpenDirectory(recoveryDirectory);
            VerifyIdentity(key.get(), directory.get(), RecoveryDirectoryId);
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
            // Persist intent before deleting the directory so startup can remove the
            // tombstone if the process stops after the directory has been removed.
            WriteDword(key.get(), nullptr, CleanupPending, 1);
            THROW_IF_WIN32_ERROR(RegFlushKey(key.get()));
            // Delete only this verified directory, and only if it is empty.
            FILE_DISPOSITION_INFO disposition{TRUE};
            THROW_IF_WIN32_BOOL_FALSE(SetFileInformationByHandle(directory.get(), FileDispositionInfo, &disposition, sizeof(disposition)));
            DeleteKey(lxssKey, KeyName(entry.Id, true).c_str());
        }
        CATCH_LOG()
    }
}
CATCH_LOG()
