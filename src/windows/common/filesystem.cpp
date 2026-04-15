/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    filesystem.cpp

Abstract:

    This file contains file system function definitions.

--*/

#include "precomp.h"
#include "filesystem.hpp"

#define FULL_PATH_PREFIX L"\\\\?\\"
#define LXSS_DOMAIN_NAME_DEFAULT "localdomain"
#define LXSS_EA_BUFFER_INCREMENT_SIZE 4096

namespace {

enum CaseSensitivity
{
    Invalid,
    Disabled,
    Enabled
};

constexpr const wchar_t* c_fileSystemKeyName = L"System\\CurrentControlSet\\Control\\FileSystem";
constexpr const wchar_t* c_enableDirCaseSensitivityValue = L"NtfsEnableDirCaseSensitivity";
constexpr DWORD c_enableDirCaseSensitivity = 0x1;
constexpr DWORD c_enableDirCaseSensitivityEmptyDirOnly = 0x2;

std::vector<char> CreateMetaDataEaBuffer(LX_UID_T Uid, LX_GID_T Gid, LX_MODE_T Mode)
{
    constexpr auto c_nameSize = (RTL_NUMBER_OF(LX_FILE_METADATA_UID_EA_NAME) - 1);

    static_assert(RTL_NUMBER_OF(LX_FILE_METADATA_UID_EA_NAME) == RTL_NUMBER_OF(LX_FILE_METADATA_GID_EA_NAME));
    static_assert(RTL_NUMBER_OF(LX_FILE_METADATA_UID_EA_NAME) == RTL_NUMBER_OF(LX_FILE_METADATA_MODE_EA_NAME));

    // Simplified version of FILE_FULL_EA_INFORMATION since the names have constant sizes.
    // See: https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/ns-wdm-_file_full_ea_information

#pragma pack(push, 1)
    struct EA_ENTRY
    {
        ULONG NextEntryOffset;
        UCHAR Flags;
        UCHAR EaNameLength;
        USHORT EaValueLength;
        CHAR EaName[c_nameSize];
        char padding[1];
        ULONG EaValue{};
        char padding2[1];
    };
#pragma pack(pop)

    static_assert(sizeof(EA_ENTRY) == 20);

    std::vector<char> buffer;
    __unaligned EA_ENTRY* currentEntry = nullptr;

    auto writeEntry = [&currentEntry, &buffer](const std::string_view& Name, ULONG Value) {
        WI_ASSERT(Name.size() == c_nameSize);

        const auto currentOffset = buffer.size();
        buffer.resize(currentOffset + sizeof(EA_ENTRY));

        currentEntry = reinterpret_cast<EA_ENTRY*>(buffer.data() + currentOffset);
        currentEntry->NextEntryOffset = sizeof(EA_ENTRY);
        currentEntry->EaNameLength = c_nameSize; // Does not include null terminator.
        currentEntry->EaValueLength = sizeof(ULONG);
        currentEntry->EaValue = Value;
        std::copy(Name.begin(), Name.end(), &currentEntry->EaName[0]);
    };

    if (Uid != LX_UID_INVALID)
    {
        writeEntry(LX_FILE_METADATA_UID_EA_NAME, Uid);
    }

    if (Gid != LX_GID_INVALID)
    {
        writeEntry(LX_FILE_METADATA_GID_EA_NAME, Gid);
    }

    if (Mode != LX_MODE_INVALID)
    {
        writeEntry(LX_FILE_METADATA_MODE_EA_NAME, Mode);
    }

    if (currentEntry != nullptr)
    {
        currentEntry->NextEntryOffset = 0;
    }

    WI_ASSERT((buffer.size() % sizeof(EA_ENTRY)) == 0);

    return buffer;
}

void CopyFileWithMetadata(_In_ PCWSTR Source, _In_ PCWSTR Destination, _In_ ULONG Mode, _In_ ULONG DistroVersion)
{
    //
    // Impersonate the client, copy the file, and write the extended attributes.
    //

    {
        auto runAsUser = wil::CoImpersonateClient();
        THROW_IF_WIN32_BOOL_FALSE(CopyFileW(Source, Destination, FALSE));

        //
        // Apply DrvFs-style attributes for instances using WslFs; otherwise,
        // use the old LxFs-style attributes.
        //

        const wil::unique_hfile file{CreateFileW(
            Destination, GENERIC_WRITE, (FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE), nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};

        IO_STATUS_BLOCK ioStatus{};

        //
        // Write the extended attributes.
        //

        if (LXSS_DISTRO_USES_WSL_FS(DistroVersion) != FALSE)
        {
            auto buffer = CreateMetaDataEaBuffer(LX_UID_ROOT, LX_GID_ROOT, Mode);
            THROW_IF_NTSTATUS_FAILED(ZwSetEaFile(file.get(), &ioStatus, buffer.data(), static_cast<ULONG>(buffer.size())));
        }
        else
        {
            LX_FILE_ATTRIBUTES_EA LxFs{};
            LX_FILE_ATTRIBUTES_EA_INITIALIZE(&LxFs);
            LxFs.Attributes.Mode = Mode;

            THROW_IF_NTSTATUS_FAILED(ZwSetEaFile(file.get(), &ioStatus, &LxFs, sizeof(LxFs)));
        }
    }
}

DWORD GetNtfsDirCaseSensitivityFlags()
{
    return wsl::windows::common::registry::ReadDword(HKEY_LOCAL_MACHINE, c_fileSystemKeyName, c_enableDirCaseSensitivityValue, 0);
}

void SetNtfsDirCaseSensitivityFlags(_In_ ULONG Flags)
{
    //
    // The service is already impersonating when this is used, and the user
    // likely doesn't have permission to set this key, so temporarily revert
    // impersonation.
    //

    auto RunAsSelf = wil::run_as_self();
    wsl::windows::common::registry::WriteDword(HKEY_LOCAL_MACHINE, c_fileSystemKeyName, c_enableDirCaseSensitivityValue, Flags);
}

CaseSensitivity GetCaseSensitivity()
{
    ULONG caseSensitiveRaw;
    THROW_IF_NTSTATUS_FAILED(::NtQueryInformationThread(
        GetCurrentThread(), ThreadExplicitCaseSensitivity, &caseSensitiveRaw, sizeof(caseSensitiveRaw), nullptr));

    return (caseSensitiveRaw == 0) ? Disabled : Enabled;
}

NTSTATUS SetCaseSensitivity(_In_ CaseSensitivity value)
{
    ULONG caseSensitiveRaw;
    switch (value)
    {
    case Disabled:
        caseSensitiveRaw = 0;
        break;

    case Enabled:
        caseSensitiveRaw = 1;
        break;

    default:
        return STATUS_INVALID_PARAMETER;
    }

    return NtSetInformationThread(GetCurrentThread(), ThreadExplicitCaseSensitivity, &caseSensitiveRaw, sizeof(caseSensitiveRaw));
}

using revert_dir_case_sensitivity =
    wil::unique_any<DWORD, decltype(&SetNtfsDirCaseSensitivityFlags), SetNtfsDirCaseSensitivityFlags, wil::details::pointer_access_none, DWORD, DWORD, DWORD_MAX, DWORD>;

using revert_case_sensitivity =
    wil::unique_any<CaseSensitivity, decltype(&SetCaseSensitivity), SetCaseSensitivity, wil::details::pointer_access_none, CaseSensitivity, CaseSensitivity, Invalid, CaseSensitivity>;

revert_dir_case_sensitivity EnableNtfsDirCaseSensitivity()
{
    auto Flags = GetNtfsDirCaseSensitivityFlags();
    auto NewFlags = Flags;
    WI_SetFlag(NewFlags, c_enableDirCaseSensitivity);
    WI_ClearFlag(NewFlags, c_enableDirCaseSensitivityEmptyDirOnly);

    //
    // Check if a change needs to be made.
    //

    if (Flags == NewFlags)
    {
        return {};
    }

    SetNtfsDirCaseSensitivityFlags(NewFlags);

    //
    // Just in case, make sure at least the main enable flag is set after
    // reverting; otherwise, WSL will break.
    //

    WI_SetFlag(Flags, c_enableDirCaseSensitivity);
    return revert_dir_case_sensitivity{Flags};
}

revert_case_sensitivity EnableCaseSensitivity()
{
    CaseSensitivity oldCaseSensitive = GetCaseSensitivity();
    THROW_IF_NTSTATUS_FAILED(SetCaseSensitivity(CaseSensitivity::Enabled));
    return revert_case_sensitivity(oldCaseSensitive);
}

bool HasReadAccessToDrive(wchar_t drive)
{
    // Note: Using GetFileSecurity / AccessCheck doesn't work if the user doesn't have access
    // to a drive (for instance the EFI partition), since the ACL returned by GetFileSecurity
    // allows read access to Everyone.
    // Using FindFirstFile guarantees that the user actually has read access to that drive.

    const wchar_t path[] = {drive, ':', '\\', '*', '\0'};

    WIN32_FIND_DATAW findData{};
    const wil::unique_hfind find{FindFirstFileW(path, &findData)};

    return !!find;
}

void EnsureCaseSensitiveDirectoryRecursive(_In_ HANDLE Directory)
{
    FILE_CASE_SENSITIVE_INFORMATION CaseInfo{};
    IO_STATUS_BLOCK IoStatus{};
    std::vector<std::byte> buffer{sizeof(FILE_ID_BOTH_DIR_INFORMATION) + MAX_PATH};
    bool restart = true;

    while (true)
    {
        const auto result = NtQueryDirectoryFile(
            Directory,
            nullptr,
            nullptr,
            nullptr,
            &IoStatus,
            buffer.data(),
            static_cast<DWORD>(buffer.size()),
            static_cast<FILE_INFORMATION_CLASS>(FileIdBothDirectoryInformation),
            true,
            nullptr,
            restart);

        WI_ASSERT(result != STATUS_PENDING);

        if (result == STATUS_NO_MORE_FILES || result == STATUS_NO_SUCH_FILE)
        {
            break;
        }
        else if (result == STATUS_BUFFER_OVERFLOW)
        {
            buffer.resize(buffer.size() * 2);
            continue;
        }

        THROW_IF_NTSTATUS_FAILED(result);

        restart = false;

        const auto* information = reinterpret_cast<const FILE_ID_BOTH_DIR_INFORMATION*>(buffer.data());

        //
        // Only process non-reparse point directories.
        //
        // N.B. Nothing needs to be done for files.
        //

        if ((WI_IsFlagSet(information->FileAttributes, FILE_ATTRIBUTE_DIRECTORY)) &&
            (WI_IsFlagClear(information->FileAttributes, FILE_ATTRIBUTE_REPARSE_POINT)))
        {

            //
            // Skip the . and .. entries.
            //

            const auto name = std::wstring_view(&information->FileName[0], information->FileNameLength / sizeof(wchar_t));
            if (name == L"." || name == L"..")
            {
                continue;
            }

            UNICODE_STRING Name{};
            RtlInitUnicodeString(&Name, information->FileName);

            auto Child = wsl::windows::common::filesystem::OpenRelativeFile(
                Directory,
                &Name,
                (FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES | SYNCHRONIZE),
                FILE_OPEN,
                (FILE_OPEN_REPARSE_POINT | FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT));

            THROW_IF_NTSTATUS_FAILED(NtQueryInformationFile(Child.get(), &IoStatus, &CaseInfo, sizeof(CaseInfo), FileCaseSensitiveInformation));

            //
            // Skip if the directory already has the flag.
            //

            if (WI_IsFlagClear(CaseInfo.Flags, FILE_CS_FLAG_CASE_SENSITIVE_DIR))
            {
                EnsureCaseSensitiveDirectoryRecursive(Child.get());
            }
        }
    }

    //
    // After all children are processed, mark the directory case-sensitive.
    //
    // N.B. This is done with a retry because if the NtfsEnableDirCaseSensitivity
    //      flag was just changed from 3 to 1, NTFS may not have updated its
    //      behavior yet in which case it will fail with STATUS_DIRECTORY_NOT_EMPTY.
    //

    CaseInfo.Flags = FILE_CS_FLAG_CASE_SENSITIVE_DIR;
    wsl::shared::retry::RetryWithTimeout<void>(
        [&]() {
            THROW_IF_NTSTATUS_FAILED(NtSetInformationFile(Directory, &IoStatus, &CaseInfo, sizeof(CaseInfo), FileCaseSensitiveInformation));
        },
        std::chrono::milliseconds{100},
        std::chrono::seconds{1},
        []() { return wil::ResultFromCaughtException() == HRESULT_FROM_NT(STATUS_DIRECTORY_NOT_EMPTY); });
}

void SetDirectoryCaseSensitive(_In_ PCWSTR Path)
{
    const wil::unique_hfile Directory{CreateFileW(
        Path,
        FILE_WRITE_ATTRIBUTES,
        (FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE),
        nullptr,
        OPEN_EXISTING,
        (FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT),
        nullptr)};

    IO_STATUS_BLOCK IoStatus;
    FILE_CASE_SENSITIVE_INFORMATION CaseInfo;
    CaseInfo.Flags = FILE_CS_FLAG_CASE_SENSITIVE_DIR;
    THROW_IF_NTSTATUS_FAILED(NtSetInformationFile(Directory.get(), &IoStatus, &CaseInfo, sizeof(CaseInfo), FileCaseSensitiveInformation));
}

void SetExtendedAttributesLxFs(_In_ PCWSTR Path, _In_ ULONG Mode, _In_ ULONG Uid, _In_ ULONG Gid)
{
    const wil::unique_hfile FileHandle(::CreateFileW(
        Path,
        FILE_GENERIC_READ | FILE_GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS,
        nullptr));

    THROW_LAST_ERROR_IF(!FileHandle);

    LX_FILE_ATTRIBUTES_EA AttributesEa;
    IO_STATUS_BLOCK IoStatusBlock;
    const NTSTATUS Status = wsl::windows::common::filesystem::QuerySingleEaFileNoThrow(
        FileHandle.get(), &IoStatusBlock, LX_FILE_ATTRIBUTES_NAME, &AttributesEa, sizeof(AttributesEa));

    //
    // If the attributes exist and are valid, leave them alone. Users can
    // change the attributes on a root inode (e.g. with chmod/chown) and those
    // changes should not be overwritten.
    //

    if ((NT_SUCCESS(Status)) && (IoStatusBlock.Information == sizeof(AttributesEa)) &&
        (AttributesEa.u.EaInformation.EaValueLength == sizeof(AttributesEa.Attributes)) &&
        (AttributesEa.Attributes.u.Flags.Version == LX_FILE_ATTRIBUTES_CURRENT_VERSION))
    {
        return;
    }

    LX_FILE_ATTRIBUTES_EA_INITIALIZE(&AttributesEa);
    AttributesEa.Attributes.Uid = Uid;
    AttributesEa.Attributes.Gid = Gid;
    AttributesEa.Attributes.Mode = Mode;
    THROW_IF_NTSTATUS_FAILED(ZwSetEaFile(FileHandle.get(), &IoStatusBlock, &AttributesEa, sizeof(AttributesEa)));
}

void SetExtendedAttributesDrvFs(_In_ PCWSTR Path, _In_ ULONG Mode, _In_ ULONG Uid, _In_ ULONG Gid)
{
    const wil::unique_hfile FileHandle{::CreateFileW(
        Path,
        FILE_GENERIC_READ | FILE_GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS,
        nullptr)};

    THROW_LAST_ERROR_IF(!FileHandle);

    //
    // Use FILE_STAT_LX_INFORMATION as an easy way to determine what attributes
    // the file already has.
    //

    FILE_STAT_LX_INFORMATION Info;
    IO_STATUS_BLOCK IoStatus;
    THROW_IF_NTSTATUS_FAILED(NtQueryInformationFile(FileHandle.get(), &IoStatus, &Info, sizeof(Info), FileStatLxInformation));

    LX_UID_T UidToSet = LX_UID_INVALID;
    LX_GID_T GidToSet = LX_GID_INVALID;
    LX_MODE_T ModeToSet = LX_MODE_INVALID;
    bool NeedUpdate = false;
    if (WI_IsFlagClear(Info.LxFlags, LX_FILE_METADATA_HAS_UID))
    {
        UidToSet = Uid;
        NeedUpdate = true;
    }

    if (WI_IsFlagClear(Info.LxFlags, LX_FILE_METADATA_HAS_GID))
    {
        GidToSet = Gid;
        NeedUpdate = true;
    }

    if (WI_IsFlagClear(Info.LxFlags, LX_FILE_METADATA_HAS_MODE))
    {
        ModeToSet = Mode;
        NeedUpdate = true;
    }

    if (NeedUpdate != false)
    {
        auto buffer = CreateMetaDataEaBuffer(UidToSet, GidToSet, ModeToSet);
        IO_STATUS_BLOCK IoStatus{};

        THROW_IF_NTSTATUS_FAILED_MSG(
            ZwSetEaFile(FileHandle.get(), &IoStatus, buffer.data(), static_cast<DWORD>(buffer.size())), "%ls", Path);
    }
}

void SetExtendedAttributes(_In_ PCWSTR Path, _In_ ULONG Mode, _In_ ULONG Uid, _In_ ULONG Gid, _In_ ULONG DistroVersion)
{
    //
    // Apply DrvFs-style attributes for instances using WslFs; otherwise, use
    // the old LxFs-style attributes.
    //

    if (LXSS_DISTRO_USES_WSL_FS(DistroVersion) != FALSE)
    {
        SetExtendedAttributesDrvFs(Path, Mode, Uid, Gid);
    }
    else
    {
        SetExtendedAttributesLxFs(Path, Mode, Uid, Gid);
    }
}

} // namespace

wsl::windows::common::filesystem::TempFile::TempFile(
    _In_ DWORD DesiredAccess, _In_ DWORD ShareMode, _In_ DWORD CreationDisposition, _In_ TempFileFlags Flags, _In_opt_ std::wstring_view Extension) :
    Flags(Flags)
{
    Path = GetTempFilename();
    if (!Extension.empty())
    {
        Path.replace_extension(Extension);
    }

    LPSECURITY_ATTRIBUTES SecurityAttributes{};
    SECURITY_ATTRIBUTES Attributes = {sizeof(SECURITY_ATTRIBUTES), nullptr, true};
    if (WI_IsFlagSet(Flags, TempFileFlags::InheritHandle))
    {
        SecurityAttributes = &Attributes;
    }

    DWORD FlagsAndAttributes = FILE_ATTRIBUTE_TEMPORARY;
    WI_SetFlagIf(FlagsAndAttributes, FILE_FLAG_DELETE_ON_CLOSE, WI_IsFlagSet(Flags, TempFileFlags::DeleteOnClose));
    Handle.reset(CreateFileW(Path.c_str(), DesiredAccess, ShareMode, SecurityAttributes, CreationDisposition, FlagsAndAttributes, nullptr));
    THROW_LAST_ERROR_IF(!Handle);
}

wsl::windows::common::filesystem::TempFile::~TempFile()
{
    // If the delete on close flag is not set, close the handle and delete the file.
    if (!Path.empty() && WI_IsFlagClear(Flags, TempFileFlags::DeleteOnClose))
    {
        Handle.reset();
        LOG_IF_WIN32_BOOL_FALSE(DeleteFileW(Path.c_str()));
    }
}

wsl::windows::common::filesystem::unique_lxss_addmount wsl::windows::common::filesystem::CreateMount(
    _In_ PCWSTR NtPath, _In_ PCWSTR Source, _In_opt_ LPCSTR Target, _In_ LPCSTR FsType, _In_ ULONG Mode, _In_ bool forWrite)
{
    unique_lxss_addmount mount = {};
    mount.WindowsDataRoot = OpenDirectoryHandle(NtPath, forWrite).release();
    mount.Source =
        wil::make_unique_ansistring<wil::unique_cotaskmem_ansistring>(wsl::shared::string::WideToMultiByte(Source).c_str()).release();
    if (ARGUMENT_PRESENT(Target))
    {
        mount.Target = wil::make_unique_ansistring<wil::unique_cotaskmem_ansistring>(Target).release();
    }

    mount.FsType = wil::make_unique_ansistring<wil::unique_cotaskmem_ansistring>(FsType).release();
    mount.MountFlags = LX_MS_NOATIME;
    WI_SetFlagIf(mount.MountFlags, LX_MS_RDONLY, !forWrite);
    mount.Mode = Mode;
    mount.Uid = LX_UID_ROOT;
    mount.Gid = LX_GID_ROOT;
    return mount;
}

void wsl::windows::common::filesystem::CreateRootFs(_In_ PCWSTR Path, _In_ ULONG Version)
{
    //
    // Declare a scope exit variable to clean up on failure.
    //

    bool deleteRootFs = false;
    const wil::unique_handle userToken = wsl::windows::common::security::GetUserToken(TokenImpersonation);
    auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&] {
        if (deleteRootFs)
        {
            auto runAsUser = wil::impersonate_token(userToken.get());
            LOG_IF_FAILED(wil::RemoveDirectoryRecursiveNoThrow(Path));
        }
    });

    //
    // Create the rootfs directory while impersonating the user, fail if the
    // directory already exists.
    //
    // N.B. Throw ERROR_FILE_EXISTS instead of ERROR_ALREADY_EXISTS for consistent
    //      error messages with WSL2.
    //

    {
        auto runAsUser = wil::impersonate_token(userToken.get());
        if (!CreateDirectoryW(Path, nullptr))
        {
            const auto lastError = GetLastError();
            THROW_WIN32_MSG(lastError == ERROR_ALREADY_EXISTS ? ERROR_FILE_EXISTS : lastError, "CreateDirectoryW");
        }

        deleteRootFs = true;
        SetExtendedAttributes(Path, (LX_S_IFDIR | 0755), LX_UID_ROOT, LX_GID_ROOT, Version);
    }

    //
    // Make sure the directory is marked case-sensitive.
    //
    // N.B. This is done without impersonating the client because setting this
    //      attribute requires the "delete subfolders and files" permission on
    //      the parent directory.
    //

    SetDirectoryCaseSensitive(Path);
    cleanup.release();
}

// Sends an ioctl to a device, and waits for the result.
void wsl::windows::common::filesystem::DeviceIoControl(_In_ HANDLE handle, _In_ ULONG code, _In_ gsl::span<const gsl::byte> input)
{
    THROW_IF_NTSTATUS_FAILED(DeviceIoControlNoThrow(handle, code, input));
}

// Sends an ioctl to a device, and waits for the result.
NTSTATUS
wsl::windows::common::filesystem::DeviceIoControlNoThrow(_In_ HANDLE handle, _In_ ULONG code, _In_ gsl::span<const gsl::byte> input)
{
    PVOID inputBuffer{};
    if (input.size() > 0)
    {
        inputBuffer = const_cast<gsl::byte*>(input.data());
    }

    IO_STATUS_BLOCK ioStatus;
    wil::unique_event event;
    event.create();
    NTSTATUS status = NtDeviceIoControlFile(
        handle, event.get(), nullptr, nullptr, &ioStatus, code, inputBuffer, gsl::narrow_cast<ULONG>(input.size()), nullptr, 0);

    if (status == STATUS_PENDING)
    {
        event.wait();
        status = ioStatus.Status;
    }

    return status;
}

std::pair<ULONG, ULONG> wsl::windows::common::filesystem::EnumerateFixedDrives(HANDLE Token)
{
    std::variant<wil::unique_coreverttoself_call, wil::unique_token_reverter> runAsUser;

    if (Token == nullptr)
    {
        runAsUser = wil::CoImpersonateClient();
    }
    else
    {
        runAsUser = wil::impersonate_token(Token);
    }

    ULONG fixedDriveBitmap = GetLogicalDrives();
    ULONG driveBitmap = fixedDriveBitmap;
    ULONG index = 0;
    ULONG nonReadableDrives = 0;
    wchar_t drivePath[] = L"A:\\";
    while (driveBitmap != 0)
    {
        WI_VERIFY(_BitScanForward(&index, driveBitmap) != FALSE);

        const ULONG driveMask = (1 << index);
        driveBitmap ^= driveMask;
        const auto driveName = static_cast<wchar_t>(L'A' + index);
        drivePath[0] = driveName;
        if (GetDriveTypeW(drivePath) != DRIVE_FIXED)
        {
            // Don't try to check if the user has read access to non-fixed drives.
            // This can cause a hang for network devices. See https://github.com/microsoft/WSL/issues/11460 .
            fixedDriveBitmap ^= driveMask;
            continue;
        }

        if (!HasReadAccessToDrive(driveName))
        {
            nonReadableDrives |= driveMask;
        }
    }

    return {fixedDriveBitmap & ~nonReadableDrives, nonReadableDrives};
}

void wsl::windows::common::filesystem::EnsureCaseSensitiveDirectory(_In_ PCWSTR Path, _In_ ULONG Flags)
{
    // N.B. Passing SYNCHRONIZE and FILE_SYNCHRONOUS_IO_NONALERT is required; otherwise, NtQueryDirectoryFile
    // might return STATUS_PENDING, which would break our folder enumeration logic.

    const wil::unique_hfile Directory{CreateFileW(
        Path,
        (FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES | SYNCHRONIZE),
        (FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE),
        nullptr,
        OPEN_EXISTING,
        (FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT | FILE_SYNCHRONOUS_IO_NONALERT),
        nullptr)};

    FILE_CASE_SENSITIVE_INFORMATION CaseInfo;
    QueryInformationFile(Directory.get(), CaseInfo, FileCaseSensitiveInformation);

    //
    // Because upgrading is done depth-first, if the directory already has the
    // flag all its children must too; this allows checking for upgrade at
    // every start with low cost, and resuming of interrupted upgrades.
    //

    if (WI_IsFlagSet(CaseInfo.Flags, FILE_CS_FLAG_CASE_SENSITIVE_DIR))
    {
        return;
    }

    //
    // Abort if upgrading is not allowed.
    //

    if (WI_IsFlagClear(Flags, LXSS_CREATE_INSTANCE_FLAGS_ALLOW_FS_UPGRADE))
    {
        THROW_HR(WSL_E_FS_UPGRADE_NEEDED);
    }

    //
    // Enable per-thread case sensitivity on the thread.
    //
    // N.B. This requires the service is running as PPL. The lifted service will
    //      return an error in this case but this is a legacy upgrade path for
    //      WSL distributions that have not been launched since RS3. This logic
    //      should be refactored in the lifted service to not require per-thread
    //      case sensitivity
    //

    revert_case_sensitivity revertCase;
    if (WI_IsFlagClear(Flags, c_case_sensitive_folders_only))
    {
        auto runAsSelf = wil::run_as_self();
        auto revertPrivilege = wsl::windows::common::security::AcquirePrivilege(SE_DEBUG_NAME);
        revertCase = EnableCaseSensitivity();
    }

    //
    // Upgrading requires that setting the per-directory case sensitivity flag
    // is allowed on non-empty directories, which requires changing the
    // registry.
    //
    // N.B. This change is reverted after the operation is complete.
    //

    auto dirCaseSensitivity = EnableNtfsDirCaseSensitivity();
    EnsureCaseSensitiveDirectoryRecursive(Directory.get());
}

bool wsl::windows::common::filesystem::EnsureDirectory(_In_ LPCWSTR pPath)
{
    //
    // Return true if a new directory is created.
    //

    if (CreateDirectoryW(pPath, nullptr))
    {
        return true;
    }

    //
    // Return false if the directory existed.
    //

    const auto lastError = GetLastError();
    if (lastError == ERROR_ALREADY_EXISTS)
    {
        return false;
    }
    else if (lastError == ERROR_PATH_NOT_FOUND)
    {
        wil::CreateDirectoryDeep(pPath);
    }
    else
    {
        THROW_WIN32_MSG(lastError, "CreateDirectoryW(%ls)", pPath);
    }

    return true;
}

void wsl::windows::common::filesystem::EnsureDirectoryWithAttributes(
    _In_ PCWSTR Path, _In_ ULONG Mode, _In_ ULONG Uid, _In_ ULONG Gid, _In_ ULONG Flags, _In_ ULONG DistroVersion)
{
    const bool newDirectory = EnsureDirectory(Path);
    SetExtendedAttributes(Path, LX_S_IFDIR | Mode, Uid, Gid, DistroVersion);

    //
    // Mark a new directory case-sensitive, or upgrade the entire tree if it
    // exists. If the root is already case-sensitive, it's assumed the entire
    // tree is.
    //

    if (newDirectory)
    {
        SetDirectoryCaseSensitive(Path);
    }
    else
    {
        EnsureCaseSensitiveDirectory(Path, Flags);
    }
}

bool wsl::windows::common::filesystem::FileExists(_In_ LPCWSTR Path)
{
    const DWORD Attributes = GetFileAttributesW(Path);
    return (Attributes != INVALID_FILE_ATTRIBUTES);
}

std::filesystem::path wsl::windows::common::filesystem::GetFullPath(_In_ LPCWSTR Path)
{
    DWORD Attributes = GetFileAttributesW(Path);
    THROW_LAST_ERROR_IF(Attributes == INVALID_FILE_ATTRIBUTES);

    const wil::unique_hfile Handle(CreateFileW(
        Path,
        GENERIC_READ,
        (FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE),
        nullptr,
        OPEN_EXISTING,
        (WI_IsFlagSet(Attributes, FILE_ATTRIBUTE_DIRECTORY) ? FILE_FLAG_BACKUP_SEMANTICS : FILE_ATTRIBUTE_NORMAL),
        nullptr));

    THROW_LAST_ERROR_IF(!Handle);

    std::wstring FullPath;
    THROW_IF_FAILED(wil::GetFinalPathNameByHandleW(Handle.get(), FullPath));

    return std::filesystem::path(std::move(FullPath));
}

std::pair<std::string, std::string> wsl::windows::common::filesystem::GetHostAndDomainNames()
{
    std::string hostName = GetLinuxHostName();

    DWORD size = 0;
    WI_VERIFY(GetComputerNameExA(ComputerNameDnsDomain, nullptr, &size) == FALSE);

    // If there is no domain name, initialize with a default. Truncate the
    // domain name to the max size that the driver allows.
    // N.B. If the buffer is too small, GetComputerNameEx() sets 'size' to the string size,
    // ** including ** the null terminator. On success it returns the string size,
    // See: https://learn.microsoft.com/en-us/windows/win32/api/sysinfoapi/nf-sysinfoapi-getcomputernameexa

    std::string domainName{};
    if (size <= 1)
    {
        domainName = LXSS_DOMAIN_NAME_DEFAULT;
    }
    else
    {
        domainName.resize(size - 1, L'\0');
        THROW_LAST_ERROR_IF(!GetComputerNameExA(ComputerNameDnsDomain, domainName.data(), &size));
        WI_ASSERT(domainName.size() == size);

        if (domainName.size() > LX_DOMAIN_NAME_MAX)
        {
            domainName.resize(LX_DOMAIN_NAME_MAX);
        }
    }

    return {std::move(hostName), std::move(domainName)};
}

std::filesystem::path wsl::windows::common::filesystem::GetLegacyBasePath(_In_ HANDLE UserToken)
{
    return GetLocalAppDataPath(UserToken) / L"lxss";
}

std::string wsl::windows::common::filesystem::GetLinuxHostName()
{
    DWORD size = 0;
    WI_VERIFY(GetComputerNameExA(ComputerNamePhysicalDnsHostname, nullptr, &size) == FALSE);
    std::string hostName(size, '\0');
    THROW_LAST_ERROR_IF(!GetComputerNameExA(ComputerNamePhysicalDnsHostname, hostName.data(), &size));

    WI_ASSERT((size <= LX_HOST_NAME_MAX) && (hostName.size() == size + 1));

    return wsl::shared::string::CleanHostname(hostName);
}

std::filesystem::path wsl::windows::common::filesystem::GetLocalAppDataPath(_In_ HANDLE userToken)
{
    return GetKnownFolderPath(FOLDERID_LocalAppData, (KF_FLAG_CREATE | KF_FLAG_NO_APPCONTAINER_REDIRECTION), userToken);
}

std::filesystem::path wsl::windows::common::filesystem::GetKnownFolderPath(const KNOWNFOLDERID& id, DWORD flags, HANDLE token)
{
    wil::unique_cotaskmem_string path;
    THROW_IF_FAILED(::SHGetKnownFolderPath(id, flags, token, &path));

    return std::filesystem::path(path.get());
}

std::filesystem::path wsl::windows::common::filesystem::GetTempFilename()
{
    WCHAR Path[MAX_PATH + 1];
    std::wstring File(MAX_PATH + 1, L'\0');
    THROW_LAST_ERROR_IF(GetTempPathW(ARRAYSIZE(Path), Path) == 0);
    THROW_LAST_ERROR_IF(GetTempFileNameW(Path, L"lx", 0, File.data()) == 0);
    File.resize(wcsnlen(File.c_str(), File.size()));
    return std::filesystem::path(std::move(File));
}

std::filesystem::path wsl::windows::common::filesystem::GetTempFolderPath(_In_ HANDLE userToken)
{
    return GetLocalAppDataPath(userToken) / L"temp";
}

std::string wsl::windows::common::filesystem::GetWindowsHosts(const std::filesystem::path& Path)
{
    std::ifstream Stream(Path.c_str());
    THROW_HR_IF_MSG(E_FAIL, (Stream.bad() || !Stream.is_open()), "errno = %d", errno);

    // Discard any BOM header.
    int potentialHeader[] = {Stream.get(), Stream.get(), Stream.get()};
    if (potentialHeader[0] != 0xEF || potentialHeader[1] != 0xBB || potentialHeader[2] != 0xBF)
    {
        Stream.seekg(0); // Reset the position to beginning of the file if no BOM header is found.
    }

    std::string WindowsHosts;
    std::string Line;
    while (std::getline(Stream, Line))
    {
        // Ignore all text after comment characters.

        const size_t Comment = Line.find_first_of('#');
        if (Comment != std::string::npos)
        {
            Line.resize(Comment);
        }

        if (Line.size() == 0)
        {
            continue;
        }

        // Create a copy of the line since the string tokenizing API is
        // destructive.

        std::string LineCopy = Line;

        // Each line is in the following format:
        // <host-address> <host-alias1> <host-alias2> ...
        //
        // N.B. There must be at least one host aliases for each host address.

        std::string CurrentEntry;
        PCHAR ElementContext = nullptr;
        PCHAR Element = strtok_s(&LineCopy[0], " \t\r\n", &ElementContext);
        while (Element != nullptr)
        {
            CurrentEntry.append(Element);
            Element = strtok_s(nullptr, " \t\r\n", &ElementContext);
            if (Element != nullptr)
            {
                CurrentEntry.append("\t");
            }
            else
            {
                CurrentEntry.append("\n");
                WindowsHosts.append(CurrentEntry);
            }
        }
    }

    WI_ASSERT(Stream.eof());

    return WindowsHosts;
}

wil::unique_hfile wsl::windows::common::filesystem::OpenDirectoryHandle(_In_ LPCWSTR pPath, _In_ bool forWrite)
{
    wil::unique_hfile handle(OpenDirectoryHandleNoThrow(pPath, forWrite));
    THROW_LAST_ERROR_IF(!handle);

    return handle;
}

wil::unique_hfile wsl::windows::common::filesystem::OpenDirectoryHandleNoThrow(_In_ LPCWSTR pPath, _In_ bool forWrite)
{
    DWORD AccessMask = FILE_GENERIC_READ | FILE_GENERIC_EXECUTE;
    if (forWrite)
    {
        WI_SetAllFlags(AccessMask, FILE_GENERIC_WRITE);
    }

    wil::unique_hfile handle(CreateFileW(
        pPath, AccessMask, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));

    return handle;
}

wil::unique_hfile wsl::windows::common::filesystem::OpenNulDevice(_In_ DWORD DesiredAccess)
{
    wil::unique_hfile nulDevice{CreateFileW(
        L"nul", DesiredAccess, (FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE), nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};

    THROW_LAST_ERROR_IF(!nulDevice);

    return nulDevice;
}

wil::unique_hfile wsl::windows::common::filesystem::OpenRelativeFile(
    _In_opt_ HANDLE Parent,
    _In_ PUNICODE_STRING RelativePath,
    _In_ ACCESS_MASK DesiredAccess,
    _In_ ULONG Disposition,
    _In_ ULONG CreateOptions,
    _In_opt_ PVOID EaBuffer,
    _In_ ULONG EaSize)

{
    auto [Status, File] = OpenRelativeFileNoThrow(Parent, RelativePath, DesiredAccess, Disposition, CreateOptions, EaBuffer, EaSize);
    THROW_IF_NTSTATUS_FAILED_MSG(Status, "Path: %.*ls", RelativePath->Length, RelativePath->Buffer);

    return std::move(File);
}

std::pair<NTSTATUS, wil::unique_hfile> wsl::windows::common::filesystem::OpenRelativeFileNoThrow(
    _In_opt_ HANDLE Parent,
    _In_ PUNICODE_STRING RelativePath,
    _In_ ACCESS_MASK DesiredAccess,
    _In_ ULONG Disposition,
    _In_ ULONG CreateOptions,
    _In_opt_ PVOID EaBuffer,
    _In_ ULONG EaSize)

{
    OBJECT_ATTRIBUTES Attributes;
    InitializeObjectAttributes(&Attributes, RelativePath, 0, Parent, nullptr);
    wil::unique_hfile File;
    IO_STATUS_BLOCK IoStatus;
    NTSTATUS Status = NtCreateFile(
        &File, DesiredAccess, &Attributes, &IoStatus, nullptr, 0, (FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE), Disposition, CreateOptions, EaBuffer, EaSize);

    return std::make_pair(Status, std::move(File));
}

wil::unique_hfile wsl::windows::common::filesystem::ReopenFile(_In_ HANDLE Handle, _In_ ACCESS_MASK DesiredAccess, _In_ ULONG CreateOptions)
{
    UNICODE_STRING Empty;
    RtlInitUnicodeString(&Empty, L"");
    return OpenRelativeFile(Handle, &Empty, DesiredAccess, FILE_OPEN, CreateOptions);
}

void wsl::windows::common::filesystem::QueryInformationFile(
    _In_ HANDLE Handle, _Out_writes_bytes_(Length) PVOID Buffer, _In_ ULONG Length, _In_ FILE_INFORMATION_CLASS FileInformationClass)
{
    IO_STATUS_BLOCK IoStatus;
    THROW_IF_NTSTATUS_FAILED(NtQueryInformationFile(Handle, &IoStatus, Buffer, Length, FileInformationClass));
}

VOID wsl::windows::common::filesystem::QuerySingleEaFile(
    _In_ HANDLE Handle, _Out_ PIO_STATUS_BLOCK IoStatus, _In_ std::string_view EaName, _Out_writes_bytes_(Length) PVOID Buffer, _In_ ULONG Length)
{
    THROW_IF_NTSTATUS_FAILED(QuerySingleEaFileNoThrow(Handle, IoStatus, EaName, Buffer, Length));
}

std::vector<CHAR> wsl::windows::common::filesystem::QuerySingleEaFile(_In_ HANDLE Handle, _In_ std::string_view EaName)
{
    std::vector<CHAR> Buffer;
    NTSTATUS Status;
    IO_STATUS_BLOCK IoStatus;
    ULONG Size = 0;
    do
    {
        Size += LXSS_EA_BUFFER_INCREMENT_SIZE;
        Buffer.resize(Size);
        Status = QuerySingleEaFileNoThrow(Handle, &IoStatus, EaName, &Buffer[0], Size);
    } while ((Status == STATUS_BUFFER_OVERFLOW) && (Size <= USHORT_MAX));

    THROW_IF_NTSTATUS_FAILED(Status);

    //
    // Resize to the actual size of the attribute.
    //

    Buffer.resize(IoStatus.Information);
    return Buffer;
}

NTSTATUS
wsl::windows::common::filesystem::QuerySingleEaFileNoThrow(
    _In_ HANDLE Handle, _Out_ PIO_STATUS_BLOCK IoStatus, _In_ std::string_view EaName, _Out_writes_bytes_(Length) PVOID Buffer, _In_ ULONG Length)
{
    union
    {
        FILE_GET_EA_INFORMATION List;
        CHAR Buffer[offsetof(FILE_GET_EA_INFORMATION, EaName) + UCHAR_MAX];
    } EaList;

    RtlZeroMemory(&EaList, sizeof(EaList));

    WI_ASSERT(EaName.size() < UCHAR_MAX);

    EaList.List.EaNameLength = static_cast<UCHAR>(EaName.size());
    RtlCopyMemory(EaList.List.EaName, EaName.data(), EaName.size());
    return ZwQueryEaFile(Handle, IoStatus, Buffer, Length, TRUE, &EaList, sizeof(EaList), nullptr, TRUE);
}

void wsl::windows::common::filesystem::SetInformationFile(
    _In_ HANDLE Handle, _In_reads_bytes_(Length) PVOID Buffer, _In_ ULONG Length, _In_ FILE_INFORMATION_CLASS FileInformationClass)
{
    IO_STATUS_BLOCK IoStatus;
    THROW_IF_NTSTATUS_FAILED(NtSetInformationFile(Handle, &IoStatus, Buffer, Length, FileInformationClass));
}

std::optional<std::filesystem::path> wsl::windows::common::filesystem::TryGetPathFromFileUrl(const std::wstring& Url)
{
    constexpr auto filePrefix = L"file://";

    if (!Url.starts_with(filePrefix))
    {
        return {};
    }

    // Skip third '/', if any
    auto startIndex = wcslen(filePrefix);
    if (Url.size() > startIndex && Url[startIndex] == L'/')
    {
        startIndex++;
    }

    // Replace '/' with '\', for convenience.
    auto path = Url.substr(startIndex);
    std::replace(path.begin(), path.end(), '/', '\\');

    return path;
}

std::wstring wsl::windows::common::filesystem::UnquotePath(_In_ LPCWSTR Path)
{
    std::wstring UnquotedPath{Path};

    // N.B. PathUnquoteSpaces() returns false if no quotes were found. No error handling is needed.
    PathUnquoteSpaces(UnquotedPath.data());
    UnquotedPath.resize(wcslen(UnquotedPath.c_str()));

    return UnquotedPath;
}

void wsl::windows::common::filesystem::UpdateInit(_In_ PCWSTR BasePath, _In_ ULONG DistroVersion)
{
    const auto source = wsl::windows::common::wslutil::GetBasePath() / L"tools" / L"init";
    const auto dest = std::filesystem::path(BasePath) / LXSS_ROOTFS_DIRECTORY / L"init";
    CopyFileWithMetadata(source.c_str(), dest.c_str(), (LX_S_IFREG | 0755), DistroVersion);
}

wil::unique_hfile wsl::windows::common::filesystem::WipeAndOpenDirectory(_In_ LPCWSTR pPath)
{
    const auto result = wil::RemoveDirectoryRecursiveNoThrow(pPath);
    THROW_HR_IF(result, (result != HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) && (result != HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND)));

    EnsureDirectory(pPath);

    return OpenDirectoryHandle(pPath, true);
}
