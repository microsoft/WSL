/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WslCoreFilesystem.h

Abstract:

    This file contains WSL Core filesystem helper function declarations.

--*/

#pragma once
#include "precomp.h"

#define MAX_VHD_COUNT 254

// Each virtiofs device uses the DAX cache, which is controlled by wslcore's caller, plus a couple
// of extra pages for configuration. MMIO space needs to be large page aligned (2MB), so request an
// additional 2MB to cover the couple of extra pages needed.
#define EXTRA_MMIO_SIZE_PER_VIRTIOFS_DEVICE_IN_MB 2

namespace wsl::core::filesystem {

/// <summary>
/// Create a file owned by the specified user.
/// </summary>
wil::unique_hfile CreateFile(
    _In_ LPCWSTR fileName, _In_ DWORD desiredAccess, _In_ DWORD shareMode, _In_ DWORD creationDisposition, _In_ DWORD flagsAndAttributes, _In_ PSID userSid);

/// <summary>
/// Create a VHD of the specified size.
/// </summary>
void CreateVhd(_In_ LPCWSTR target, _In_ ULONGLONG maximumSize, _In_ PSID userSid, _In_ BOOL sparse, _In_ BOOL fixed);

wil::unique_handle OpenVhd(_In_ LPCWSTR Path, _In_ VIRTUAL_DISK_ACCESS_MASK Mask);

void ResizeExistingVhd(_In_ HANDLE diskHandle, _In_ ULONGLONG maximumSize, _In_ RESIZE_VIRTUAL_DISK_FLAG resizeFlag);

ULONGLONG GetDiskSize(_In_ HANDLE diskHandle);

} // namespace wsl::core::filesystem
