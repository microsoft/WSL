/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    disk.hpp

Abstract:

    This file contains disk functions declarations.

--*/

#pragma once

#include <windows.h>
#include <wil/resource.h>
#include <chrono>
#include <map>
#include <string>
#include <vector>

namespace wsl::windows::common::disk {

constexpr inline auto c_diskOperationRetry = std::chrono::milliseconds(500);

constexpr inline size_t c_defaultDiskTimeoutMs = 5 * 1000;

/// <summary>
/// Tracks the host state changes that were performed to attach a disk to a VM so they can be undone.
/// </summary>
enum DiskStateFlags
{
    None = 0x0,
    // The disk was taken offline on the host and needs to be brought back online.
    Online = 0x1,
    // The VM was granted access to the disk and access needs to be revoked.
    AccessGranted = 0x2
};

DEFINE_ENUM_FLAG_OPERATORS(DiskStateFlags);

wil::unique_hfile OpenDevice(_In_ LPCWSTR Name, _In_ DWORD Access = GENERIC_READ, _In_ size_t TimeoutMs = c_defaultDiskTimeoutMs);

/// <summary>
/// Opens a VHD file so its backing volume can be tracked. The file is opened without requesting
/// any access so the VM can keep using it.
/// </summary>
wil::unique_hfile OpenVhdBackingFile(_In_ LPCWSTR Path);

/// <summary>
/// Returns whether the volume backing an open file is still mounted. This is used to detect
/// attachments that went stale because their backing volume was detached.
/// </summary>
bool IsBackingVolumeMounted(_In_ HANDLE File);

bool IsDiskOnline(_In_ HANDLE Disk);

void SetOnline(_In_ HANDLE Disk, _In_ bool Online, _In_ size_t TimeoutMs = c_defaultDiskTimeoutMs);

/// <summary>
/// Takes a disk offline if it is currently online. Returns whether the disk state was changed.
/// </summary>
bool TakeOffline(_In_ LPCWSTR Disk, _In_ size_t TimeoutMs = c_defaultDiskTimeoutMs);

/// <summary>
/// Brings a disk back online on the host.
/// </summary>
void BringOnline(_In_ LPCWSTR Disk, _In_ size_t TimeoutMs = c_defaultDiskTimeoutMs);

void LockVolume(_In_ HANDLE Disk);

void Ioctl(_In_ HANDLE Disk, _In_ DWORD Code, _In_opt_ LPVOID InData = nullptr, _In_ DWORD InDataSize = 0, _Out_opt_ LPVOID OutData = nullptr, _In_ DWORD OutDataSize = 0);

std::map<std::wstring, wil::unique_hfile> ListDiskVolumes(_In_ HANDLE Disk);

std::vector<std::wstring> GetVolumeDevices(_In_ HANDLE Volume);

DWORD
GetDiskNumber(_In_ HANDLE Disk);

std::vector<std::wstring> ListDiskPartitions(_In_ HANDLE Disk);

void ValidateDiskVolumesAreReady(_In_ HANDLE Disk);
} // namespace wsl::windows::common::disk
