// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include "DeviceHostProxy.h"

inline const std::wstring c_defaultDeviceTag = L"default";

struct VirtioFsShareOptions
{
    VirtiofsShareKind Kind = VirtiofsShareKind_FileBacked;
    UINT32 SharedMemorySizeMb = 0;
};

//
// Provides synchronized access to guest device operations.
//
class GuestDeviceManager
{
public:
    GuestDeviceManager(_In_ const std::wstring& machineId, _In_ const GUID& runtimeId, bool EnableTelemetry = true);
    ~GuestDeviceManager();

    _Requires_lock_not_held_(m_lock)
    GUID AddVirtiofsDevice(_In_ PCWSTR Label, _In_opt_ PCWSTR MountOptions, _In_ PCWSTR RootPath, _In_ HANDLE UserToken, VirtioFsShareOptions Options = {});

    _Requires_lock_not_held_(m_lock)
    GUID AddVirtioPmemDevice(_In_ PCWSTR Path, bool ReadOnly, _In_ HANDLE UserToken);

    _Requires_lock_not_held_(m_lock)
    GUID AddNewDevice(_In_ const GUID& deviceId, _In_ const wil::com_ptr<IPlan9FileSystem>& server, _In_ PCWSTR tag);

    _Requires_lock_not_held_(m_lock)
    GUID AddVirtioNetDevice(_In_ PCWSTR Tag, const WslVirtioNetConfig& Config, const std::vector<IpAddress>& Nameservers, _In_ HANDLE UserToken);

    wil::com_ptr<IWslVirtioNetDevice> GetVirtioNetDevice(_In_ PCWSTR Tag);

    void AddRemoteFileSystem(_In_ REFCLSID clsid, _In_ PCWSTR tag, _In_ const wil::com_ptr<IPlan9FileSystem>& server);

    void AddSharedMemoryDevice(_In_ PCWSTR Tag, _In_ PCWSTR Path, _In_ UINT32 SizeMb, _In_ HANDLE UserToken);

    wil::com_ptr<IPlan9FileSystem> GetRemoteFileSystem(_In_ REFCLSID clsid, _In_ std::wstring_view tag);

    void SetSwiotlb(UINT64 GpaBase, UINT64 SizeBytes);

    _Requires_lock_not_held_(m_lock)
    void RemoveGuestDevice(_In_ const GUID& InstanceId);

private:
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
    _Guarded_by_(m_lock) std::map<std::wstring, GUID> m_virtioNetDevices;
};
