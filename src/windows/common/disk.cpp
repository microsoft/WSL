/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    disk.cpp

Abstract:

    This file contains disk functions implementations.

--*/

#include "precomp.h"
#include <diskguid.h>

#include "disk.hpp"
#include "wslutil.h"

wil::unique_hfile wsl::windows::common::disk::OpenDevice(_In_ LPCWSTR Name, _In_ DWORD Access, size_t TimeoutMs)
{
    auto openDevice = [&]() {
        wil::unique_hfile handle{
            CreateFileW(Name, Access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr)};

        THROW_LAST_ERROR_IF(!handle);

        return handle;
    };

    // E_ACCESSDENIED is returned if the disk is in use, which can happen when the disk has just been detached and is being
    // attached to the host again. Retry for 5 seconds before failing.

    return wsl::shared::retry::RetryWithTimeout<wil::unique_hfile>(
        openDevice, c_diskOperationRetry, std::chrono::milliseconds(TimeoutMs), []() {
            const auto error = wil::ResultFromCaughtException();
            return error == E_ACCESSDENIED || error == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION);
        });
}

bool wsl::windows::common::disk::IsDiskOnline(_In_ HANDLE Disk)
{
    GET_DISK_ATTRIBUTES attributes = {0};
    Ioctl(Disk, IOCTL_DISK_GET_DISK_ATTRIBUTES, nullptr, 0, &attributes, sizeof(attributes));

    return !WI_IsFlagSet(attributes.Attributes, DISK_ATTRIBUTE_OFFLINE);
}

void wsl::windows::common::disk::Ioctl(
    _In_ HANDLE Disk, _In_ DWORD Code, _In_opt_ LPVOID InData, _In_ DWORD InDataSize, _Out_opt_ LPVOID OutData, _In_ DWORD OutDataSize)
{
    DWORD bytesReturned = 0;
    THROW_LAST_ERROR_IF(!DeviceIoControl(Disk, Code, InData, InDataSize, OutData, OutDataSize, &bytesReturned, nullptr));
}

void wsl::windows::common::disk::LockVolume(_In_ HANDLE Disk)
{
    Ioctl(Disk, FSCTL_LOCK_VOLUME);
}

void wsl::windows::common::disk::SetOnline(_In_ HANDLE Disk, _In_ bool Online, _In_ size_t TimeoutMs)
{
    // Lock and unmount all the volumes contained in the disk.
    // This is done to make sure than the disk is not in use before setting
    // the offline attribute (which doesn't fail if the disk is in use)

    if (!Online)
    {
        const auto volumes = ListDiskVolumes(Disk);

        // Lock all the volumes first so that we we're confident
        // that FSCTL_DISMOUNT_VOLUME won't fail
        // There's no need to unlock the volumes here as this is done when the handle is closed

        for (const auto& e : volumes)
        {
            try
            {
                wsl::shared::retry::RetryWithTimeout<void>(
                    std::bind(&LockVolume, e.second.get()), c_diskOperationRetry, std::chrono::milliseconds(TimeoutMs), []() {
                        return wil::ResultFromCaughtException() == E_ACCESSDENIED;
                    });
            }
            catch (...)
            {
                // FSCTL_LOCK_VOLUME returns access denied if the disk is in use.
                // Let's make the error a bit better for the user

                THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_DRIVE_LOCKED), wil::ResultFromCaughtException() == E_ACCESSDENIED);
                throw;
            }
        }

        for (const auto& e : volumes)
        {
            Ioctl(e.second.get(), FSCTL_DISMOUNT_VOLUME);
        }
    }

    SET_DISK_ATTRIBUTES attributes = {0};
    attributes.Version = sizeof(attributes);
    attributes.AttributesMask = DISK_ATTRIBUTE_OFFLINE;
    attributes.Attributes = Online ? 0 : DISK_ATTRIBUTE_OFFLINE;
    Ioctl(Disk, IOCTL_DISK_SET_DISK_ATTRIBUTES, &attributes, sizeof(attributes));
}

DWORD
wsl::windows::common::disk::GetDiskNumber(_In_ HANDLE Disk)
{
    STORAGE_DEVICE_NUMBER DiskNumber;
    Ioctl(Disk, IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr, 0, &DiskNumber, sizeof(DiskNumber));

    return DiskNumber.DeviceNumber;
}

std::map<std::wstring, wil::unique_hfile> wsl::windows::common::disk::ListDiskVolumes(_In_ HANDLE Disk)
{
    ValidateDiskVolumesAreReady(Disk);

    size_t partitionCount = 16;
    std::vector<char> buffer;

    for (;;)
    {
        buffer.resize(offsetof(DRIVE_LAYOUT_INFORMATION_EX, PartitionEntry) + partitionCount * sizeof(PARTITION_INFORMATION_EX));

        if (!DeviceIoControl(Disk, IOCTL_DISK_GET_DRIVE_LAYOUT_EX, nullptr, 0, buffer.data(), gsl::narrow_cast<DWORD>(buffer.size()), nullptr, nullptr))
        {
            THROW_LAST_ERROR_IF(GetLastError() != ERROR_INSUFFICIENT_BUFFER);
            THROW_IF_FAILED(SizeTMult(partitionCount, 2, &partitionCount));
        }
        else
        {
            break;
        }
    }

    const auto* partitions = reinterpret_cast<PDRIVE_LAYOUT_INFORMATION_EX>(buffer.data());
    std::vector<DWORD> partitionNumbers;
    for (const auto& partition : gsl::make_span(partitions->PartitionEntry, partitions->PartitionCount))
    {
        if (partition.PartitionStyle == PARTITION_STYLE_MBR && partition.Mbr.PartitionType != PARTITION_ENTRY_UNUSED &&
            partition.Mbr.PartitionType != PARTITION_SPACES && partition.Mbr.PartitionType != PARTITION_EXTENDED &&
            partition.Mbr.PartitionType != PARTITION_XINT13_EXTENDED)
        {
            partitionNumbers.emplace_back(partition.PartitionNumber);
        }
        else if (
            partition.PartitionStyle == PARTITION_STYLE_GPT && partition.Gpt.PartitionType != PARTITION_ENTRY_UNUSED_GUID &&
            partition.Gpt.PartitionType != PARTITION_SPACES_GUID)
        {
            partitionNumbers.emplace_back(partition.PartitionNumber);
        }

        // If the partition scheme is neither MBR nor GPT, then it's RAW, which means
        // that Windows doesn't recognize it.
    }

    auto diskNumber = GetDiskNumber(Disk);

    auto transform = [diskNumber](DWORD partitionNumber) {
        auto path = std::format(L"\\\\?\\Harddisk{}Partition{}", diskNumber, partitionNumber);
        return std::make_pair(std::move(path), OpenDevice(path.c_str(), GENERIC_ALL));
    };

    std::map<std::wstring, wil::unique_hfile> output;
    std::transform(partitionNumbers.begin(), partitionNumbers.end(), std::inserter(output, output.begin()), transform);

    return output;
}

void wsl::windows::common::disk::ValidateDiskVolumesAreReady(_In_ HANDLE Disk)
{
    // Will throw if the disk is not ready
    Ioctl(Disk, IOCTL_DISK_ARE_VOLUMES_READY);
}
