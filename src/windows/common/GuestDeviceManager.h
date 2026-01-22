// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include "DeviceHostProxy.h"

// Flags for virtiofs vdev device creation.
#define VIRTIO_FS_FLAGS_TYPE_FILES 0x8000
#define VIRTIO_FS_FLAGS_TYPE_SECTIONS 0x4000

inline const std::wstring c_defaultDeviceTag = L"default";

// These device types and class IDs are implemented by the external wsldevicehost vdev.
DEFINE_GUID(VIRTIO_FS_DEVICE_ID, 0x872270E1, 0xA899, 0x4AF6, 0xB4, 0x54, 0x71, 0x93, 0x63, 0x44, 0x35, 0xAD); // {872270E1-A899-4AF6-B454-7193634435AD}
DEFINE_GUID(VIRTIO_FS_ADMIN_CLASS_ID, 0x7E6AD219, 0xD1B3, 0x42D5, 0xB8, 0xEE, 0xD9, 0x63, 0x24, 0xE6, 0x4F, 0xF6); // {7E6AD219-D1B3-42D5-B8EE-D96324E64FF6}
DEFINE_GUID(VIRTIO_FS_CLASS_ID, 0x60285AE6, 0xAAF3, 0x4456, 0xB4, 0x44, 0xA6, 0xC2, 0xD0, 0xDE, 0xDA, 0x38); // {60285AE6-AAF3-4456-B444-A6C2D0DEDA38}

DEFINE_GUID(VIRTIO_NET_DEVICE_ID, 0xF07010D0, 0x0EA9, 0x447F, 0x88, 0xEF, 0xBD, 0x95, 0x2A, 0x4D, 0x2F, 0x14); // {F07010D0-0EA9-447F-88EF-BD952A4D2F14}
DEFINE_GUID(VIRTIO_NET_CLASS_ID, 0x16479D2E, 0xF0C3, 0x4DBA, 0xBF, 0x7A, 0x04, 0xFF, 0xF0, 0x89, 0x2B, 0x07); // {16479D2E-F0C3-4DBA-BF7A-04FFF0892B07}

DEFINE_GUID(VIRTIO_PMEM_DEVICE_ID, 0xEDBB24BB, 0x5E19, 0x40F4, 0x8A, 0x0F, 0x82, 0x24, 0x31, 0x30, 0x64, 0xFD); // {EDBB24BB-5E19-40F4-8A0F-8224313064FD}
DEFINE_GUID(VIRTIO_PMEM_CLASS_ID, 0xABB755FC, 0x1B86, 0x4255, 0x83, 0xE2, 0xE5, 0x78, 0x7A, 0xBC, 0xF6, 0xC2); // {ABB755FC-1B86-4255-83E2-E5787ABCF6C2}

//
// Provides synchronized access to guest device operations.
//
class GuestDeviceManager
{
public:
    GuestDeviceManager(_In_ const std::wstring& machineId, _In_ const GUID& runtimeId);

    _Requires_lock_not_held_(m_lock)
    GUID AddGuestDevice(
        _In_ const GUID& DeviceId,
        _In_ const GUID& ImplementationClsid,
        _In_ PCWSTR AccessName,
        _In_opt_ PCWSTR Options,
        _In_ PCWSTR Path,
        _In_ UINT32 Flags,
        _In_ HANDLE UserToken);

    GUID AddNewDevice(_In_ const GUID& deviceId, _In_ const wil::com_ptr<IPlan9FileSystem>& server, _In_ PCWSTR tag);

    void AddRemoteFileSystem(_In_ REFCLSID clsid, _In_ PCWSTR tag, _In_ const wil::com_ptr<IPlan9FileSystem>& server);

    void AddSharedMemoryDevice(_In_ const GUID& ImplementationClsid, _In_ PCWSTR Tag, _In_ PCWSTR Path, _In_ UINT32 SizeMb, _In_ HANDLE UserToken);

    wil::com_ptr<IPlan9FileSystem> GetRemoteFileSystem(_In_ REFCLSID clsid, _In_ std::wstring_view tag);

    void Shutdown();

private:
    _Requires_lock_held_(m_lock)
    GUID AddHdvShareWithOptions(
        _In_ const GUID& DeviceId,
        _In_ const GUID& ImplementationClsid,
        _In_ PCWSTR AccessName,
        _In_opt_ PCWSTR Options,
        _In_ PCWSTR Path,
        _In_ UINT32 Flags,
        _In_ HANDLE UserToken);

    struct DirectoryObjectLifetime
    {
        std::wstring Path;
        // Directory objects are temporary, even if they have children, so need to keep
        // any created handles open in order for the directory to remain accessible.
        std::vector<wil::unique_handle> HierarchyLifetimes;
    };

    DirectoryObjectLifetime CreateSectionObjectRoot(_In_ std::wstring_view RelativeRootPath, _In_ HANDLE UserToken) const;

    wil::srwlock m_lock;
    std::wstring m_machineId;
    wil::com_ptr<DeviceHostProxy> m_deviceHostSupport;
    _Guarded_by_(m_lock) std::vector<DirectoryObjectLifetime> m_objectDirectories;
};
