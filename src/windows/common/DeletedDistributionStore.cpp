// Copyright (C) Microsoft Corporation. All rights reserved.
#include "precomp.h"
#include "DeletedDistributionStore.h"
#include "retryshared.h"

using namespace wsl::windows::common;
using namespace wsl::windows::common::registry;

namespace {
constexpr auto c_recoveryPath = L"RecoveryPath";
constexpr auto c_recoveryFileId = L"RecoveryFileId";
constexpr auto c_recoveryDirectoryId = L"RecoveryDirectoryId";
constexpr auto c_originalDirectoryId = L"RecoveryOriginalDirectoryId";
constexpr auto c_preparingDirectory = L"RecoveryPreparingDirectory";
constexpr auto c_recoveryAnchorId = L"RecoveryAnchorId";
constexpr auto c_deletedAt = L"DeletedAt";
constexpr auto c_previousState = L"RecoveryPreviousState";
constexpr auto c_restored = L"RecoveryRestored";
constexpr auto c_restorePending = L"RecoveryRestorePending";
constexpr auto c_restoreName = L"RecoveryRestoreName";
constexpr auto c_cleanupPending = L"RecoveryCleanupPending";
constexpr auto c_permanentDelete = L"RecoveryPermanentDelete";
constexpr std::wstring_view c_deletedPrefix = L"Deleted-";

std::wstring KeyName(const GUID& id, bool deleted = false)
{
    return (deleted ? std::wstring(c_deletedPrefix) : L"") + wsl::shared::string::GuidToString<wchar_t>(id);
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

wil::unique_hfile OpenDirectory(const std::filesystem::path& path, DWORD sharing = FILE_SHARE_READ, bool allowMissing = false, DWORD access = DELETE | FILE_READ_ATTRIBUTES)
{
    wil::unique_hfile directory{CreateFileW(
        path.c_str(), access, sharing, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    if (!directory)
    {
        const auto error = GetLastError();
        if (allowMissing && (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND))
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

void VerifyIdentity(HKEY key, HANDLE file, LPCWSTR value = c_recoveryFileId)
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

void RemoveEmptySourceDirectory(const std::filesystem::path& path, const FILE_ID_INFO& expected) noexcept
try
{
    const auto directory = OpenDirectory(path, FILE_SHARE_READ | FILE_SHARE_WRITE, true);
    if (!directory)
    {
        return;
    }
    const auto actual = Identity(directory.get());
    THROW_HR_IF(
        HRESULT_FROM_WIN32(ERROR_FILE_INVALID),
        expected.VolumeSerialNumber != actual.VolumeSerialNumber || memcmp(&expected.FileId, &actual.FileId, sizeof(expected.FileId)) != 0);
    // Delete only the verified directory itself. Unknown files and replacements stay intact.
    FILE_DISPOSITION_INFO disposition{TRUE};
    if (!SetFileInformationByHandle(directory.get(), FileDispositionInfo, &disposition, sizeof(disposition)))
    {
        const auto error = GetLastError();
        THROW_WIN32_IF(error, error != ERROR_DIR_NOT_EMPTY);
    }
}
CATCH_LOG()

bool DirectoryWasDeleted(HKEY key, const std::filesystem::path& directory, LPCWSTR value = c_recoveryDirectoryId)
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
    THROW_IF_WIN32_ERROR(RegGetValueW(key, nullptr, value, RRF_RT_REG_BINARY, nullptr, &expected, &size));
    THROW_HR_IF(E_INVALIDARG, size != sizeof(expected));
    const auto anchor =
        OpenDirectory(directory.parent_path(), FILE_SHARE_READ | FILE_SHARE_WRITE, false, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES);
    VerifyIdentity(key, anchor.get(), c_recoveryAnchorId);
    const auto anchorId = Identity(anchor.get());
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_FILE_INVALID), expected.VolumeSerialNumber != anchorId.VolumeSerialNumber);
    const FILE_ID_128 emptyId{};
    THROW_HR_IF(E_INVALIDARG, memcmp(&expected.FileId, &emptyId, sizeof(emptyId)) == 0);
    auto openById = [&](const FILE_ID_INFO& id) {
        FILE_ID_DESCRIPTOR descriptor{};
        descriptor.dwSize = sizeof(descriptor);
        descriptor.Type = ExtendedFileIdType;
        descriptor.ExtendedFileId = id.FileId;
        return wil::unique_hfile{OpenFileById(
            anchor.get(), &descriptor, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT)};
    };
    // Validate lookup support with a known live identity before interpreting
    // a stale NTFS file ID, which reports ERROR_INVALID_PARAMETER after deletion.
    const auto control = openById(anchorId);
    THROW_LAST_ERROR_IF(!control);
    const auto original = openById(expected);
    if (original)
    {
        // A rename makes the path unavailable without deleting its directory.
        return false;
    }
    const auto error = GetLastError();
    THROW_WIN32_IF(error, error != ERROR_FILE_NOT_FOUND && error != ERROR_INVALID_PARAMETER);
    return !OpenDirectory(directory, FILE_SHARE_READ, true);
}

wil::unique_hfile OpenOriginalDisk(HKEY key)
{
    const auto path =
        std::filesystem::path(ReadString(key, nullptr, L"BasePath")) / ReadString(key, nullptr, L"VhdFileName", LXSS_VM_MODE_VHD_NAME);
    try
    {
        const auto parent =
            OpenDirectory(path.parent_path(), FILE_SHARE_READ | FILE_SHARE_WRITE, true, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES);
        if (!parent)
        {
            THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND), !DirectoryWasDeleted(key, path.parent_path(), c_originalDirectoryId));
            return {};
        }
        DWORD size{};
        const auto result = RegGetValueW(key, nullptr, c_originalDirectoryId, RRF_RT_REG_BINARY, nullptr, nullptr, &size);
        THROW_WIN32_IF(result, result != ERROR_SUCCESS && result != ERROR_FILE_NOT_FOUND);
        if (result == ERROR_SUCCESS)
        {
            VerifyIdentity(key, parent.get(), c_originalDirectoryId);
        }
        std::wstring resolved;
        THROW_IF_FAILED(wil::GetFinalPathNameByHandleW(parent.get(), resolved));
        const auto physicalPath = std::filesystem::path(resolved) / path.filename();
        const auto anchor = OpenDirectory(
            physicalPath.parent_path().parent_path(), FILE_SHARE_READ | FILE_SHARE_WRITE, false, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES);
        VerifyIdentity(key, anchor.get(), c_recoveryAnchorId);
        auto file = OpenDisk(physicalPath, false);
        if (file)
        {
            VerifyIdentity(key, file.get());
        }
        else
        {
            // Legacy journals without the source identity may delete a positively
            // identified disk, but cannot establish absence in a replacement folder.
            THROW_HR_IF(E_INVALIDARG, result != ERROR_SUCCESS && !ReadDword(key, nullptr, c_cleanupPending, 0));
        }
        return file;
    }
    catch (...)
    {
        const auto error = wil::ResultFromCaughtException();
        THROW_HR_IF(error, error != HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND));
        // The install directory may also have been removed. Establish absence
        // only through the same verified, available anchor as recovery storage.
        THROW_HR_IF(error, !DirectoryWasDeleted(key, path.parent_path(), c_originalDirectoryId));
        return {};
    }
}

void CompleteDirectoryPreparation(HKEY key)
{
    const auto path = std::filesystem::path(ReadString(key, nullptr, c_recoveryPath)).parent_path();
    const auto original = OpenOriginalDisk(key);
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND), !original);
    const auto anchor =
        OpenDirectory(path.parent_path(), FILE_SHARE_READ | FILE_SHARE_WRITE, false, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES);
    VerifyIdentity(key, anchor.get(), c_recoveryAnchorId);
    DWORD size{};
    const auto result = RegGetValueW(key, nullptr, c_recoveryDirectoryId, RRF_RT_REG_BINARY, nullptr, nullptr, &size);
    THROW_WIN32_IF(result, result != ERROR_SUCCESS && result != ERROR_FILE_NOT_FOUND);
    auto directory = OpenDirectory(path, FILE_SHARE_READ | FILE_SHARE_WRITE, true);
    if (!directory)
    {
        // A saved identity still protects a directory temporarily renamed away.
        THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND), result == ERROR_SUCCESS && !DirectoryWasDeleted(key, path));
        THROW_IF_WIN32_BOOL_FALSE(CreateDirectoryW(path.c_str(), nullptr));
        directory = OpenDirectory(path, FILE_SHARE_READ | FILE_SHARE_WRITE);
    }
    else if (result == ERROR_SUCCESS)
    {
        VerifyIdentity(key, directory.get(), c_recoveryDirectoryId);
    }
    else
    {
        // The durable preparation intent owns this unique destination name, but
        // no disk has moved yet. Never claim a directory containing other files.
        THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_DIR_NOT_EMPTY), !std::filesystem::is_empty(path));
    }
    const auto identity = Identity(directory.get());
    THROW_IF_WIN32_ERROR(RegSetValueExW(key, c_recoveryDirectoryId, 0, REG_BINARY, reinterpret_cast<const BYTE*>(&identity), sizeof(identity)));
    THROW_IF_WIN32_ERROR(RegFlushKey(key));
    DeleteValue(key, c_preparingDirectory);
    THROW_IF_WIN32_ERROR(RegFlushKey(key));
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

bool IsRegisteredDisk(HKEY lxssKey, HANDLE file, const GUID* excludedId = nullptr)
{
    const auto retainedId = Identity(file);
    for (const auto& [id, name] : EnumGuidKeys(lxssKey))
    {
        if (excludedId && IsEqualGUID(id, *excludedId))
        {
            continue;
        }
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

bool DeleteOriginalDisk(HKEY lxssKey, HKEY key, const GUID* excludedId = nullptr)
{
    auto file = OpenOriginalDisk(key);
    if (file)
    {
        if (IsRegisteredDisk(lxssKey, file.get(), excludedId))
        {
            return false;
        }
        FILE_DISPOSITION_INFO disposition{TRUE};
        THROW_IF_WIN32_BOOL_FALSE(SetFileInformationByHandle(file.get(), FileDispositionInfo, &disposition, sizeof(disposition)));
        file.reset();
    }
    WriteDword(key, nullptr, c_cleanupPending, 1);
    THROW_IF_WIN32_ERROR(RegFlushKey(key));
    return true;
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
                       !ReadOptionalString(key.get(), nullptr, c_recoveryPath);
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
    for (const auto value :
         {c_recoveryPath,
          c_recoveryFileId,
          c_recoveryDirectoryId,
          c_originalDirectoryId,
          c_preparingDirectory,
          c_recoveryAnchorId,
          c_deletedAt,
          c_restored,
          c_restorePending,
          c_restoreName,
          c_cleanupPending,
          c_previousState,
          c_permanentDelete})
    {
        DeleteValue(key, value);
    }
}

void CompleteRestore(HKEY lxssKey, HKEY key, const GUID& id, bool deleted)
{
    const auto path = std::filesystem::path(ReadString(key, nullptr, c_recoveryPath));
    const auto name = ReadString(key, nullptr, c_restoreName);
    WriteString(key, nullptr, L"BasePath", path.parent_path().c_str());
    WriteString(key, nullptr, L"VhdFileName", path.filename().c_str());
    WriteString(key, nullptr, L"DistributionName", name.c_str());
    // A restored store distro becomes independently managed, like an imported VHD.
    for (const auto value : {L"PackageFamilyName", L"ShortcutPath", L"TerminalProfilePath"})
    {
        DeleteValue(key, value);
    }
    WriteDword(key, nullptr, c_restored, 1);
    WriteDword(key, nullptr, L"State", LxssDistributionStateInstalled);
    THROW_IF_WIN32_ERROR(RegFlushKey(key));
    if (deleted)
    {
        THROW_IF_WIN32_ERROR(RegRenameKey(lxssKey, KeyName(id, true).c_str(), KeyName(id).c_str()));
    }
    CommitRestore(lxssKey, id);
    ClearRecoveryValues(key);
}

} // namespace

ULONG64 DeletedDistributionStore::Now()
{
    FILETIME time{};
    GetSystemTimeAsFileTime(&time);
    return (static_cast<ULONG64>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}

bool DeletedDistributionStore::Retain(
    HKEY lxssKey, const GUID& id, const std::filesystem::path& vhdPath, const std::function<void(const std::filesystem::path&)>& cleanupArtifacts)
{
    // Reject a redirected install directory and keep it locked against replacement.
    // Resolve ancestor aliases once, then open the disk through this verified parent.
    // Attribute-only opens do not enforce share access; include directory-list
    // access so denying delete sharing prevents a concurrent replacement.
    auto sourceDirectory =
        OpenDirectory(vhdPath.parent_path(), FILE_SHARE_READ | FILE_SHARE_WRITE, false, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES);
    const auto sourceIdentity = Identity(sourceDirectory.get());
    std::wstring resolvedDirectory;
    THROW_IF_FAILED(wil::GetFinalPathNameByHandleW(sourceDirectory.get(), resolvedDirectory));
    const auto originalPath = std::filesystem::path(resolvedDirectory) / vhdPath.filename();
    // HCS may briefly keep a handle after ejecting the disk. Match the existing
    // unregister retry window for sharing violations rather than failing a normal teardown.
    auto file = wsl::shared::retry::RetryWithTimeout<wil::unique_hfile>(
        [&] { return OpenDisk(originalPath, false); },
        std::chrono::milliseconds(100),
        std::chrono::seconds(10),
        {HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION)});
    if (!file)
    {
        // Broken registrations with no filesystem must still be removable.
        return false;
    }

    GUID storageId{};
    THROW_IF_FAILED(CoCreateGuid(&storageId));
    // Stay on the disk's volume: an atomic rename avoids copying a potentially huge VHD.
    // Use a sibling so reinstalling into or removing the original directory cannot delete the recovery copy.
    const auto directory =
        originalPath.parent_path().parent_path() / (L".wsl-recovery-" + wsl::shared::string::GuidToString<wchar_t>(storageId));
    const auto target = directory / originalPath.filename();
    bool createdDirectory = false;
    auto removeEmptyDirectory = wil::scope_exit([&] {
        if (createdDirectory)
        {
            RemoveDirectoryW(directory.c_str());
        }
    });
    wil::unique_hfile directoryHandle;
    // The anchor is never deleted through this handle. Read/list access also
    // permits it to coincide with an already locked source directory at a volume root.
    const auto anchorHandle =
        OpenDirectory(directory.parent_path(), FILE_SHARE_READ | FILE_SHARE_WRITE, false, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES);
    const auto key = OpenKey(lxssKey, KeyName(id).c_str(), KEY_READ | KEY_WRITE);
    // Attempt the existing best-effort artifact cleanup before making the disk
    // recoverable. A crash during cleanup leaves the original registration/VHD
    // available for retry; a crash after the move cannot skip this cleanup attempt.
    if (cleanupArtifacts)
    {
        cleanupArtifacts(originalPath.parent_path());
    }
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
    WriteDword(key.get(), nullptr, c_previousState, originalState);
    WriteString(key.get(), nullptr, c_recoveryPath, target.c_str());
    WriteQword(key.get(), nullptr, c_deletedAt, Now());
    THROW_IF_WIN32_ERROR(RegSetValueExW(key.get(), c_recoveryFileId, 0, REG_BINARY, reinterpret_cast<const BYTE*>(&identity), sizeof(identity)));
    THROW_IF_WIN32_ERROR(RegSetValueExW(
        key.get(), c_originalDirectoryId, 0, REG_BINARY, reinterpret_cast<const BYTE*>(&sourceIdentity), sizeof(sourceIdentity)));
    const auto anchorIdentity = Identity(anchorHandle.get());
    THROW_IF_WIN32_ERROR(RegSetValueExW(
        key.get(), c_recoveryAnchorId, 0, REG_BINARY, reinterpret_cast<const BYTE*>(&anchorIdentity), sizeof(anchorIdentity)));
    // Record the destination before creating it so startup can repair a crash
    // before its directory identity has been saved. Artifact cleanup already ran.
    WriteDword(key.get(), nullptr, c_preparingDirectory, 1);
    THROW_IF_WIN32_ERROR(RegFlushKey(key.get()));
    THROW_IF_WIN32_BOOL_FALSE(CreateDirectoryW(directory.c_str(), nullptr));
    createdDirectory = true;
    // Keep the destination and its anchor locked against replacement during the move.
    directoryHandle = OpenDirectory(directory, FILE_SHARE_READ | FILE_SHARE_WRITE);
    const auto directoryIdentity = Identity(directoryHandle.get());
    THROW_IF_WIN32_ERROR(RegSetValueExW(
        key.get(), c_recoveryDirectoryId, 0, REG_BINARY, reinterpret_cast<const BYTE*>(&directoryIdentity), sizeof(directoryIdentity)));
    THROW_IF_WIN32_ERROR(RegFlushKey(key.get()));
    DeleteValue(key.get(), c_preparingDirectory);
    THROW_IF_WIN32_ERROR(RegFlushKey(key.get()));
    RenameDisk(file.get(), target);
    moved = true;
    WriteQword(key.get(), nullptr, c_deletedAt, Now());
    WriteDword(key.get(), nullptr, L"State", LxssDistributionStateDeleted);
    THROW_IF_WIN32_ERROR(RegRenameKey(lxssKey, KeyName(id).c_str(), KeyName(id, true).c_str()));
    rollback.release();
    removeEmptyDirectory.release();
    THROW_IF_WIN32_ERROR(RegFlushKey(key.get()));
    if (cleanupArtifacts)
    {
        sourceDirectory.reset();
        RemoveEmptySourceDirectory(originalPath.parent_path(), sourceIdentity);
    }
    return true;
}

std::vector<DeletedDistributionStore::Entry> DeletedDistributionStore::Enumerate(HKEY lxssKey, bool includePermanentDelete)
{
    std::vector<Entry> result;
    for (const auto& [name, key] : EnumKeys(lxssKey, KEY_READ))
    {
        if (!name.starts_with(c_deletedPrefix))
        {
            continue;
        }
        try
        {
            if (!includePermanentDelete && ReadDword(key.get(), nullptr, c_permanentDelete, 0))
            {
                continue;
            }
            const auto id = wsl::shared::string::ToGuid(name.substr(c_deletedPrefix.size()));
            THROW_HR_IF(E_INVALIDARG, !id);
            result.push_back(
                {*id,
                 ReadString(key.get(), nullptr, L"DistributionName"),
                 ReadString(key.get(), nullptr, c_recoveryPath),
                 ReadQword(key.get(), nullptr, c_deletedAt, 0)});
        }
        CATCH_LOG()
    }
    return result;
}

void DeletedDistributionStore::Restore(HKEY lxssKey, const Entry& distribution, LPCWSTR name)
{
    const auto key = OpenKey(lxssKey, KeyName(distribution.Id, true).c_str(), KEY_READ | KEY_WRITE);
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED), ReadDword(key.get(), nullptr, c_permanentDelete, 0) != 0);
    const auto now = Now();
    THROW_HR_WITH_USER_ERROR_IF(
        HRESULT_FROM_WIN32(ERROR_TIMEOUT),
        wsl::shared::Localization::MessageRestoreExpired(),
        distribution.DeletedAt != 0 && now >= distribution.DeletedAt && now - distribution.DeletedAt >= c_retention);
    const auto directory = OpenDirectory(distribution.Path.parent_path(), FILE_SHARE_READ | FILE_SHARE_WRITE);
    VerifyIdentity(key.get(), directory.get(), c_recoveryDirectoryId);
    auto file = OpenDisk(distribution.Path);
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND), !file);
    VerifyIdentity(key.get(), file.get());

    // Persist the intended restore before changing registration fields. Startup can
    // complete the operation even if the process stops between individual writes.
    WriteString(key.get(), nullptr, c_restoreName, name);
    WriteDword(key.get(), nullptr, c_restorePending, 1);
    THROW_IF_WIN32_ERROR(RegFlushKey(key.get()));
    CompleteRestore(lxssKey, key.get(), distribution.Id, true);
}

void DeletedDistributionStore::Purge(HKEY lxssKey, const GUID& id)
{
    auto [key, result] = OpenKeyNoThrow(lxssKey, KeyName(id).c_str(), KEY_READ | KEY_WRITE);
    if (FAILED(result))
    {
        THROW_HR_IF(result, result != HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND));
        key = OpenKey(lxssKey, KeyName(id, true).c_str(), KEY_READ | KEY_WRITE);
    }
    THROW_HR_IF(E_INVALIDARG, !ReadOptionalString(key.get(), nullptr, c_recoveryPath));
    // Commit permanent intent first. An unavailable disk keeps its identity
    // journal for deletion when it returns, and can never be restored afterward.
    WriteDword(key.get(), nullptr, c_permanentDelete, 1);
    THROW_IF_WIN32_ERROR(RegFlushKey(key.get()));
    DeleteValue(key.get(), c_restorePending);
    DeleteValue(key.get(), c_restored);
    THROW_IF_WIN32_ERROR(RegFlushKey(key.get()));
    RecoverPending(lxssKey);
    Cleanup(lxssKey);
}

void DeletedDistributionStore::RecoverPending(HKEY lxssKey) noexcept
try
{
    auto registrations = EnumGuidKeys(lxssKey);
    for (const auto& entry : Enumerate(lxssKey, true))
    {
        registrations.emplace_back(entry.Id, KeyName(entry.Id, true));
    }
    for (const auto& [id, name] : registrations)
    {
        try
        {
            const auto key = OpenKey(lxssKey, name.c_str(), KEY_READ | KEY_WRITE);
            if (ReadDword(key.get(), nullptr, c_preparingDirectory, 0))
            {
                CompleteDirectoryPreparation(key.get());
            }
            if (ReadDword(key.get(), nullptr, c_permanentDelete, 0))
            {
                const auto path = std::filesystem::path(ReadString(key.get(), nullptr, c_recoveryPath));
                wil::unique_hfile directory;
                wil::unique_hfile file;
                const auto excludedId = name.starts_with(c_deletedPrefix) ? nullptr : &id;
                if (DirectoryWasDeleted(key.get(), path.parent_path()))
                {
                    // There is no destination left to move into. Finish explicit
                    // deletion through the verified original disk handle instead.
                    if (!DeleteOriginalDisk(lxssKey, key.get(), excludedId))
                    {
                        continue;
                    }
                }
                else
                {
                    directory = OpenDirectory(path.parent_path(), FILE_SHARE_READ | FILE_SHARE_WRITE);
                    VerifyIdentity(key.get(), directory.get(), c_recoveryDirectoryId);
                    file = OpenDisk(path);
                    if (!file)
                    {
                        file = OpenOriginalDisk(key.get());
                        if (file)
                        {
                            // The journal itself still references the original path. Protect
                            // any other registration that has imported this disk in place.
                            if (IsRegisteredDisk(lxssKey, file.get(), excludedId))
                            {
                                continue;
                            }
                            RenameDisk(file.get(), path);
                        }
                    }
                    if (file)
                    {
                        VerifyIdentity(key.get(), file.get());
                    }
                }
                DeleteValue(key.get(), c_restorePending);
                DeleteValue(key.get(), c_restored);
                WriteDword(key.get(), nullptr, L"State", LxssDistributionStateDeleted);
                THROW_IF_WIN32_ERROR(RegFlushKey(key.get()));
                if (!name.starts_with(c_deletedPrefix))
                {
                    THROW_IF_WIN32_ERROR(RegRenameKey(lxssKey, name.c_str(), KeyName(id, true).c_str()));
                }
                continue;
            }
            const bool pendingRestore = ReadDword(key.get(), nullptr, c_restorePending, 0) != 0;
            if (pendingRestore || ReadDword(key.get(), nullptr, c_restored, 0))
            {
                const auto path = ReadOptionalString(key.get(), nullptr, c_recoveryPath);
                wil::unique_hfile directory;
                wil::unique_hfile file;
                if (path)
                {
                    directory = OpenDirectory(std::filesystem::path(*path).parent_path(), FILE_SHARE_READ | FILE_SHARE_WRITE);
                    VerifyIdentity(key.get(), directory.get(), c_recoveryDirectoryId);
                    file = OpenDisk(*path);
                    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND), !file);
                    VerifyIdentity(key.get(), file.get());
                }
                else
                {
                    THROW_HR_IF(
                        E_INVALIDARG,
                        name.starts_with(c_deletedPrefix) ||
                            ReadDword(key.get(), nullptr, L"State", LxssDistributionStateInvalid) != LxssDistributionStateInstalled);
                }
                // Keep the verified directory and disk handles open through
                // registration and journal updates so neither can be replaced.
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
                    CompleteRestore(lxssKey, key.get(), id, name.starts_with(c_deletedPrefix));
                }
                else
                {
                    WriteDword(key.get(), nullptr, L"State", LxssDistributionStateInstalled);
                    if (name.starts_with(c_deletedPrefix))
                    {
                        THROW_IF_WIN32_ERROR(RegRenameKey(lxssKey, name.c_str(), KeyName(id).c_str()));
                    }
                    CommitRestore(lxssKey, id);
                    ClearRecoveryValues(key.get());
                }
                continue;
            }
            const auto path = ReadOptionalString(key.get(), nullptr, c_recoveryPath);
            if (!path || name.starts_with(c_deletedPrefix))
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
                    if (!DirectoryWasDeleted(key.get(), recoveryDirectory))
                    {
                        const auto directory = OpenDirectory(recoveryDirectory);
                        VerifyIdentity(key.get(), directory.get(), c_recoveryDirectoryId);
                        FILE_DISPOSITION_INFO disposition{TRUE};
                        THROW_IF_WIN32_BOOL_FALSE(
                            SetFileInformationByHandle(directory.get(), FileDispositionInfo, &disposition, sizeof(disposition)));
                    }
                    WriteDword(key.get(), nullptr, L"State", ReadDword(key.get(), nullptr, c_previousState, LxssDistributionStateInstalled));
                    ClearRecoveryValues(key.get());
                }
            }
        }
        CATCH_LOG()
    }

    // An interrupted unregister may have committed the key rename before choosing a new default.
    const auto defaultName = ReadOptionalString(lxssKey, nullptr, L"DefaultDistribution");
    if (defaultName && FAILED(OpenKeyNoThrow(lxssKey, defaultName->c_str(), KEY_READ).second) &&
        SUCCEEDED(OpenKeyNoThrow(lxssKey, (std::wstring(c_deletedPrefix) + *defaultName).c_str(), KEY_READ).second))
    {
        DeleteValue(lxssKey, L"DefaultDistribution");
        for (const auto& [id, name] : EnumGuidKeys(lxssKey))
        {
            const auto key = OpenKey(lxssKey, name.c_str(), KEY_READ);
            if (ReadDword(key.get(), nullptr, L"State", LxssDistributionStateInvalid) == LxssDistributionStateInstalled &&
                !ReadOptionalString(key.get(), nullptr, c_recoveryPath))
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
    for (const auto& entry : Enumerate(lxssKey, true))
    {
        try
        {
            const auto key = OpenKey(lxssKey, KeyName(entry.Id, true).c_str(), KEY_READ | KEY_WRITE);
            // Permanent deletion is explicit; ordinary retention still protects
            // missing/corrupt timestamps and clocks moving backwards.
            if (!ReadDword(key.get(), nullptr, c_permanentDelete, 0) &&
                (entry.DeletedAt == 0 || currentTime < entry.DeletedAt || currentTime - entry.DeletedAt < c_retention))
            {
                continue;
            }
            // A failed or interrupted restore must never become eligible for deletion again.
            if (ReadDword(key.get(), nullptr, c_restored, 0) || ReadDword(key.get(), nullptr, c_restorePending, 0))
            {
                continue;
            }
            const auto recoveryDirectory = entry.Path.parent_path();
            if (DirectoryWasDeleted(key.get(), recoveryDirectory))
            {
                if (ReadDword(key.get(), nullptr, c_permanentDelete, 0) && !DeleteOriginalDisk(lxssKey, key.get()))
                {
                    continue;
                }
                DeleteKey(lxssKey, KeyName(entry.Id, true).c_str());
                continue;
            }
            // Keep the verified directory locked against replacement until deletion completes.
            const auto directory = OpenDirectory(recoveryDirectory);
            VerifyIdentity(key.get(), directory.get(), c_recoveryDirectoryId);
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
            else if (ReadDword(key.get(), nullptr, c_permanentDelete, 0) && !!OpenOriginalDisk(key.get()))
            {
                // Recovery may have deferred moving a disk owned by another
                // registration. Keep its journal until that move can complete.
                continue;
            }
            // Persist intent before deleting the directory so startup can remove the
            // tombstone if the process stops after the directory has been removed.
            WriteDword(key.get(), nullptr, c_cleanupPending, 1);
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
