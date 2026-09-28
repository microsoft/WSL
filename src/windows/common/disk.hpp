/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    disk.hpp

Abstract:

    This file contains disk functions declarations.

--*/

#pragma once

namespace wsl::windows::common::disk {

constexpr inline auto c_diskOperationRetry = std::chrono::milliseconds(500);

wil::unique_hfile OpenDevice(_In_ LPCWSTR Name, _In_ DWORD Access = GENERIC_READ, _In_ size_t TimeoutMs = 5 * 1000);

bool IsDiskOnline(_In_ HANDLE Disk);

void SetOnline(_In_ HANDLE Disk, _In_ bool Online, _In_ size_t TimeoutMs = 5 * 1000);

void LockVolume(_In_ HANDLE Disk);

void Ioctl(_In_ HANDLE Disk, _In_ DWORD Code, _In_opt_ LPVOID InData = nullptr, _In_ DWORD InDataSize = 0, _Out_opt_ LPVOID OutData = nullptr, _In_ DWORD OutDataSize = 0);

std::map<std::wstring, wil::unique_hfile> ListDiskVolumes(_In_ HANDLE Disk);

std::vector<std::wstring> GetVolumeDevices(_In_ HANDLE Volume);

DWORD
GetDiskNumber(_In_ HANDLE Disk);

std::vector<std::wstring> ListDiskPartitions(_In_ HANDLE Disk);

void ValidateDiskVolumesAreReady(_In_ HANDLE Disk);
} // namespace wsl::windows::common::disk
