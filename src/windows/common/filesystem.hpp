/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    filesystem.hpp

Abstract:

    This file contains file system function declarations.

--*/

#pragma once

#include "wslservice.h"

#define LXSS_FS_TYPE_DRVFS "drvfs"
#define LXSS_FS_TYPE_LXFS "lxfs"
#define LXSS_FS_TYPE_SHAREFS "sharefs"
#define LXSS_FS_TYPE_TMPFS "tmpfs"
#define LXSS_FS_TYPE_WSLFS "wslfs"

namespace wsl::windows::common::filesystem {

enum class TempFileFlags
{
    None = 0x0,
    DeleteOnClose = 0x1,
    InheritHandle = 0x2
};

DEFINE_ENUM_FLAG_OPERATORS(TempFileFlags);

// Used only in unit tests.
constexpr ULONG c_case_sensitive_folders_only = 0x100;

// Make sure that the above flag doesn't conflict with create instance flags
static_assert((LXSS_CREATE_INSTANCE_FLAGS_ALL & c_case_sensitive_folders_only) == 0);

struct TempFile
{
    std::filesystem::path Path;
    wil::unique_hfile Handle;
    TempFileFlags Flags = TempFileFlags::None;

    TempFile(
        _In_ DWORD DesiredAccess,
        _In_ DWORD ShareMode,
        _In_ DWORD CreationDisposition,
        _In_ TempFileFlags Flags = TempFileFlags::None,
        _In_opt_ std::wstring_view Extension = {});

    ~TempFile();

    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;

    TempFile(TempFile&& other) noexcept
    {
        *this = std::move(other);
    }

    TempFile& operator=(TempFile&& other) noexcept
    {
        std::swap(Path, other.Path);
        std::swap(Handle, other.Handle);
        std::swap(Flags, other.Flags);
        return *this;
    }
};

inline void FreeLXSS_ADDMOUNT(_Inout_opt_ PLX_KMAPPATHS_ADDMOUNT pMount)
{
    if (pMount)
    {
        if (pMount->Source)
        {
            CoTaskMemFree((LPVOID)pMount->Source);
        }

        if (pMount->Target)
        {
            CoTaskMemFree((LPVOID)pMount->Target);
        }

        if (pMount->FsType)
        {
            CoTaskMemFree((LPVOID)pMount->FsType);
        }

        if (pMount->WindowsDataRoot && (pMount->WindowsDataRoot != INVALID_HANDLE_VALUE))
        {
            CloseHandle(pMount->WindowsDataRoot);
        }
    }
}

using unique_lxss_addmount = wil::unique_struct<LX_KMAPPATHS_ADDMOUNT, decltype(&FreeLXSS_ADDMOUNT), FreeLXSS_ADDMOUNT>;

/// <summary>
/// Creates a mount for instance creation.
/// </summary>
unique_lxss_addmount CreateMount(
    _In_ PCWSTR NtPath, _In_ PCWSTR Source, _In_opt_ LPCSTR Target, _In_ LPCSTR FsType, _In_ ULONG Mode, _In_ bool forWrite = true);

/// <summary>
/// Creates a directory for the root file system.
/// </summary>
void CreateRootFs(_In_ PCWSTR Path, _In_ ULONG Version);

void DeviceIoControl(_In_ HANDLE handle, _In_ ULONG code, _In_ gsl::span<const gsl::byte> input = {});

NTSTATUS
DeviceIoControlNoThrow(_In_ HANDLE handle, _In_ ULONG code, _In_ gsl::span<const gsl::byte> input = {});

std::pair<ULONG, ULONG> EnumerateFixedDrives(HANDLE Token = nullptr);

/// <summary>
/// Creates a directory with the given path if it does not exist. Throws if creating the directory
/// failed.
/// </summary>
bool EnsureDirectory(_In_ LPCWSTR pPath);

/// <summary>
/// Marks every directory in a tree case-sensitive.
/// </summary>
void EnsureCaseSensitiveDirectory(_In_ PCWSTR Path, _In_ ULONG Flags);

/// <summary>
/// Creates a directory with the given path if it does not exist, and applies
/// the specified attributes if the directory doesn't have any. Throws if
/// creating the directory or applying the attributes failed.
/// </summary>
void EnsureDirectoryWithAttributes(_In_ PCWSTR Path, _In_ ULONG Mode, _In_ ULONG Uid, _In_ ULONG Gid, _In_ ULONG Flags, _In_ ULONG DistroVersion);

bool FileExists(_In_ LPCWSTR Path);

std::filesystem::path GetFullPath(_In_ LPCWSTR Path);

std::pair<std::string, std::string> GetHostAndDomainNames();

std::string GetLinuxHostName();

/// <summary>
/// Gets the base path for legacy installs.
/// </summary>
std::filesystem::path GetLegacyBasePath(_In_ HANDLE UserToken);

std::filesystem::path GetLocalAppDataPath(_In_ HANDLE userToken);

std::filesystem::path GetKnownFolderPath(const KNOWNFOLDERID& id, DWORD flags, HANDLE token = nullptr);

std::filesystem::path GetTempFilename();

std::filesystem::path GetTempFolderPath(_In_ HANDLE userToken);

std::string GetWindowsHosts(const std::filesystem::path& Path);

/// <summary>
/// Opens a directory handle with read/execute, optionally also write, & full sharing. The path
/// must exist and be a directory. Throws if the directory cannot be opened.
/// </summary>
wil::unique_hfile OpenDirectoryHandle(_In_ LPCWSTR pPath, _In_ bool forWrite);

/// <summary>
/// Opens a directory handle with read/execute, optionally also write, & full sharing. The path
/// must exist and be a directory.
/// </summary>
wil::unique_hfile OpenDirectoryHandleNoThrow(_In_ LPCWSTR pPath, _In_ bool forWrite);

/// <summary>
/// Opens the null device.
/// </summary>
wil::unique_hfile OpenNulDevice(_In_ DWORD DesiredAccess);

wil::unique_hfile OpenRelativeFile(
    _In_opt_ HANDLE Parent,
    _In_ PUNICODE_STRING RelativePath,
    _In_ ACCESS_MASK DesiredAccess,
    _In_ ULONG Disposition,
    _In_ ULONG CreateOptions,
    _In_opt_ PVOID EaBuffer = nullptr,
    _In_ ULONG EaSize = 0);

std::pair<NTSTATUS, wil::unique_hfile> OpenRelativeFileNoThrow(
    _In_opt_ HANDLE Parent,
    _In_ PUNICODE_STRING RelativePath,
    _In_ ACCESS_MASK DesiredAccess,
    _In_ ULONG Disposition,
    _In_ ULONG CreateOptions,
    _In_opt_ PVOID EaBuffer = nullptr,
    _In_ ULONG EaSize = 0);

wil::unique_hfile ReopenFile(_In_ HANDLE Handle, _In_ ACCESS_MASK DesiredAccess, _In_ ULONG CreateOptions);

void QueryInformationFile(_In_ HANDLE Handle, _Out_writes_bytes_(Length) PVOID Buffer, _In_ ULONG Length, _In_ FILE_INFORMATION_CLASS FileInformationClass);

template <typename T>
void QueryInformationFile(_In_ HANDLE Handle, _Out_ T& Buffer, _In_ FILE_INFORMATION_CLASS FileInformationClass)
{
    QueryInformationFile(Handle, &Buffer, sizeof(Buffer), FileInformationClass);
}

VOID QuerySingleEaFile(_In_ HANDLE Handle, _Out_ PIO_STATUS_BLOCK IoStatus, _In_ std::string_view EaName, _Out_writes_bytes_(Length) PVOID Buffer, _In_ ULONG Length);

std::vector<CHAR> QuerySingleEaFile(_In_ HANDLE Handle, _In_ std::string_view EaName);

NTSTATUS
QuerySingleEaFileNoThrow(
    _In_ HANDLE Handle, _Out_ PIO_STATUS_BLOCK IoStatus, _In_ std::string_view EaName, _Out_writes_bytes_(Length) PVOID Buffer, _In_ ULONG Length);

void SetInformationFile(_In_ HANDLE Handle, _In_reads_bytes_(Length) PVOID Buffer, _In_ ULONG Length, _In_ FILE_INFORMATION_CLASS FileInformationClass);

template <typename T>
void SetInformationFile(_In_ HANDLE Handle, _In_ T& Buffer, _In_ FILE_INFORMATION_CLASS FileInformationClass)
{
    SetInformationFile(Handle, &Buffer, sizeof(Buffer), FileInformationClass);
}

std::optional<std::filesystem::path> TryGetPathFromFileUrl(const std::wstring& Url);

std::wstring UnquotePath(_In_ LPCWSTR Path);

/// <summary>
/// Updates the init binary.
/// </summary>
void UpdateInit(_In_ PCWSTR BasePath, _In_ ULONG DistroVersion);

/// <summary>
/// Wipes out the directory with the given path if it exists, then creates it again and returns
/// an open directory handle onto it.
/// </summary>
wil::unique_hfile WipeAndOpenDirectory(_In_ LPCWSTR pPath);

} // namespace wsl::windows::common::filesystem
