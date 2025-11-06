/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WslCoreFilesystem.cpp

Abstract:

    This file contains filesystem helper function definitions.

--*/

#include "precomp.h"
#include "WslCoreFilesystem.h"
#include "WslSecurity.h"

wil::unique_hfile wsl::core::filesystem::CreateFile(
    _In_ LPCWSTR fileName, _In_ DWORD desiredAccess, _In_ DWORD shareMode, _In_ DWORD creationDisposition, _In_ DWORD flagsAndAttributes, _In_ PSID userSid)
{
    auto sd = windows::common::security::CreateSecurityDescriptor(userSid);
    SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), &sd, false};
    wil::unique_hfile file{CreateFileW(fileName, desiredAccess, shareMode, &sa, creationDisposition, flagsAndAttributes, nullptr)};
    THROW_LAST_ERROR_IF(!file);

    return file;
}

void wsl::core::filesystem::CreateVhd(_In_ LPCWSTR target, _In_ ULONGLONG maximumSize, _In_ PSID userSid, _In_ BOOL sparse, _In_ BOOL fixed)
{
    WI_ASSERT(wsl::windows::common::string::IsPathComponentEqual(
        std::filesystem::path{target}.extension().native(), windows::common::wslutil::c_vhdxFileExtension));

    // Disable creation of sparse VHDs while data corruption is being debugged.
    if (sparse)
    {
        sparse = false;
        EMIT_USER_WARNING(wsl::shared::Localization::MessageSparseVhdDisabled());
    }

    VIRTUAL_STORAGE_TYPE storageType{};
    storageType.DeviceId = VIRTUAL_STORAGE_TYPE_DEVICE_VHDX;
    storageType.VendorId = VIRTUAL_STORAGE_TYPE_VENDOR_MICROSOFT;

    // Create a VHDX with the specified maximum size.
    //
    // N.B. The block size was chosen based on the best practices for Linux VHDs:
    //      https://docs.microsoft.com/en-us/windows-server/virtualization/hyper-v/best-practices-for-running-linux-on-hyper-v
    CREATE_VIRTUAL_DISK_PARAMETERS createVhdParameters{};
    createVhdParameters.Version = CREATE_VIRTUAL_DISK_VERSION_2;
    createVhdParameters.Version2.BlockSizeInBytes = _1MB;
    createVhdParameters.Version2.MaximumSize = maximumSize;

    CREATE_VIRTUAL_DISK_FLAG flags = CREATE_VIRTUAL_DISK_FLAG_SUPPORT_COMPRESSED_VOLUMES;
    WI_SetFlagIf(flags, CREATE_VIRTUAL_DISK_FLAG_FULL_PHYSICAL_ALLOCATION, fixed);
    if (sparse)
    {
        WI_SetAllFlags(flags, CREATE_VIRTUAL_DISK_FLAG_SPARSE_FILE | CREATE_VIRTUAL_DISK_FLAG_SUPPORT_SPARSE_FILES_ANY_FS);
    }

    // Explicitly set the owner of the file so the default is not used.
    //
    // N.B. This ensures that HcsGrantVmAccess is able to add the required ACL
    //      to the VHD because the operation is done while impersonating the user.
    auto sd = windows::common::security::CreateSecurityDescriptor(userSid);
    wil::unique_hfile vhd{};
    THROW_IF_WIN32_ERROR(
        ::CreateVirtualDisk(&storageType, target, VIRTUAL_DISK_ACCESS_NONE, &sd, flags, 0, &createVhdParameters, nullptr, &vhd));
}

wil::unique_handle wsl::core::filesystem::OpenVhd(_In_ LPCWSTR Path, _In_ VIRTUAL_DISK_ACCESS_MASK Mask)
{
    WI_ASSERT(wsl::windows::common::wslutil::IsVhdFile(std::filesystem::path{Path}));

    // N.B. Specifying unknown for device and vendor means the system will determine the type of VHD.
    VIRTUAL_STORAGE_TYPE storageType{};
    storageType.DeviceId = VIRTUAL_STORAGE_TYPE_DEVICE_UNKNOWN;
    storageType.VendorId = VIRTUAL_STORAGE_TYPE_VENDOR_UNKNOWN;

    wil::unique_handle disk;
    THROW_IF_WIN32_ERROR(OpenVirtualDisk(&storageType, Path, Mask, OPEN_VIRTUAL_DISK_FLAG_NONE, nullptr, &disk));

    return disk;
}

void wsl::core::filesystem::ResizeExistingVhd(_In_ HANDLE diskHandle, _In_ ULONGLONG maximumSize, _In_ RESIZE_VIRTUAL_DISK_FLAG resizeFlag)
{
    RESIZE_VIRTUAL_DISK_PARAMETERS resize{};
    resize.Version1.NewSize = maximumSize;
    resize.Version = RESIZE_VIRTUAL_DISK_VERSION_1;
    THROW_IF_WIN32_ERROR(ResizeVirtualDisk(diskHandle, resizeFlag, &resize, nullptr));
}

ULONGLONG wsl::core::filesystem::GetDiskSize(_In_ HANDLE diskHandle)
{
    GET_VIRTUAL_DISK_INFO virtualDiskInfo{};
    virtualDiskInfo.Version = GET_VIRTUAL_DISK_INFO_SIZE;
    ULONG size = sizeof(virtualDiskInfo);

    THROW_IF_WIN32_ERROR(GetVirtualDiskInformation(diskHandle, &size, &virtualDiskInfo, nullptr));

    return virtualDiskInfo.Size.VirtualSize;
}
