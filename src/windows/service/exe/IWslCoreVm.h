// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include "LxssCreateProcess.h"
#include "WslCoreConfig.h"

class IWslCoreVm
{
public:
    using InitializeDrvFsCallback = std::function<LX_INIT_DRVFS_MOUNT(HANDLE)>;

    enum class DiskType
    {
        Invalid = 0x0,
        VHD = 0x1,
        PassThrough = 0x2
    };

    struct DiskMountResult
    {
        std::string MountPointName;
        int Result;
        LX_MINI_MOUNT_STEP Step;
    };

    virtual ~IWslCoreVm() = default;

    virtual ULONG AttachDisk(
        _In_ PCWSTR Disk,
        _In_ DiskType Type,
        _In_ std::optional<ULONG> Lun,
        _In_ bool IsUserDisk,
        _In_ HANDLE UserToken) = 0;

    // Connects to a guest endpoint returned by another operation on this VM, such as CreateInstance().
    virtual wil::unique_socket ConnectToGuest(_In_ ULONG Port) const = 0;

    virtual std::shared_ptr<LxssRunningInstance> CreateInstance(
        _In_ const GUID& InstanceId,
        _In_ const LXSS_DISTRO_CONFIGURATION& Configuration,
        _In_ LX_MESSAGE_TYPE MessageType,
        _In_ DWORD ReceiveTimeout = 0,
        _In_ ULONG DefaultUid = LX_UID_ROOT,
        _In_ ULONG64 ClientLifetimeId = 0,
        _In_ ULONG ExportFlags = 0,
        _Out_opt_ ULONG* ConnectPort = nullptr) = 0;

    virtual wil::unique_socket CreateRootNamespaceProcess(_In_ LPCSTR Path, _In_ LPCSTR* Arguments) = 0;

    virtual std::pair<int, LX_MINI_MOUNT_STEP> DetachDisk(_In_opt_ PCWSTR Disk) = 0;

    virtual void EjectVhd(_In_ PCWSTR VhdPath) = 0;

    // Returns the effective WSL configuration after backend capability checks and runtime fallbacks.
    virtual const wsl::core::Config& GetConfig() const noexcept = 0;

    // Returns the logical VM identity supplied when this VM was created.
    virtual GUID GetRuntimeId() const = 0;

    virtual int GetVmIdleTimeout() const = 0;

    virtual bool InitializeDrvFs(_In_ HANDLE UserToken) = 0;

    virtual bool IsVhdAttached(_In_ PCWSTR VhdPath) = 0;

    virtual DiskMountResult MountDisk(
        _In_ PCWSTR Disk,
        _In_ DiskType MountDiskType,
        _In_ ULONG PartitionIndex,
        _In_opt_ PCWSTR Name,
        _In_opt_ PCWSTR Type,
        _In_opt_ PCWSTR Options) = 0;

    virtual void MountRootNamespaceFolder(
        _In_ LPCWSTR HostPath, _In_ LPCWSTR GuestPath, _In_ bool ReadOnly, _In_ LPCWSTR Name) = 0;

    // Registered callbacks may run on implementation-owned threads.
    virtual void RegisterCallbacks(
        _In_ const std::function<void(ULONG)>& DistroExitCallback = {},
        _In_ const std::function<void(GUID)>& TerminationCallback = {}) = 0;

    virtual void ResizeDistribution(_In_ ULONG Lun, _In_ HANDLE OutputHandle, _In_ ULONG64 NewSize) = 0;

    virtual void SaveAttachedDisksState() = 0;

    virtual void TrimDistribution(_In_ ULONG Lun) = 0;
};