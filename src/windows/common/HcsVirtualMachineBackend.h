/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    HcsVirtualMachineBackend.h

Abstract:

    Implementation of IVirtualMachineBackend - represents a single HCS-based VM instance.

--*/

#pragma once

#include "IVirtualMachineBackend.h"
#include "GuestDeviceManager.h"
#include "hcs.hpp"

class HcsVirtualMachineBackend : public IVirtualMachineBackend
{
public:
    ~HcsVirtualMachineBackend() noexcept override;

    static std::unique_ptr<HcsVirtualMachineBackend> Create(const VmCreateRequest& Request);
    static VmPlatformCapabilities QueryCapabilities();

    VmPlatformCapabilities GetCapabilities() const override;
    VmDescription GetDescription() const override;
    wil::unique_handle GetTerminationEvent() const override;
    void Start() override;
    void Terminate() override;

    VmGuestListener CreateGuestListener(GuestServicePort Port) override;
    wil::unique_socket ConnectGuest(GuestServicePort Port) override;
    void CloseGuestListener(VmListenerId Listener) override;

    VmDiskAttachment AttachDisk(const VmDiskRequest& Request) override;
    void DetachDisk(VmDiskId Disk) override;

    VmFileSystemDevice CreateFileSystemDevice(const VmFileSystemDeviceRequest& Request) override;
    VmFileSystemShare AddFileSystemShare(VmDeviceId Device, const VmFileSystemShareRequest& Request) override;
    void RemoveFileSystemShare(VmShareId Share) override;

    VmNetworkAttachment AddNetworkAdapter(const VmNetworkAdapterRequest& Request) override;
    VmPortBinding BindPort(VmDeviceId Device, const VmPortBindingRequest& Request) override;
    void UnbindPort(VmPortBindingId Binding) override;

private:
    struct VmConfiguration
    {
        VmDescription Description;
        wsl::windows::common::hcs::ComputeSystem Settings;
    };

    struct AttachedDisk
    {
        VmDiskAttachment Attachment;
        // Set for pass-through disks, cleared for virtual disks.
        bool PassThrough = false;
        std::wstring Path;
        wsl::windows::common::disk::DiskStateFlags Flags{};
        // Timeout applied when the host disk state changes performed to attach the disk are undone.
        std::chrono::milliseconds DeviceTimeout{wsl::windows::common::disk::c_defaultDiskTimeoutMs};
        wil::unique_hfile BackingFile;
    };

    struct FileSystemDevice
    {
        VmFileSystemDevice Device;
        VmFileSystemDeviceTransport Transport;
        // Mount options applied when the virtio-fs device is created.
        std::wstring MountOptions;
        wil::com_ptr<IPlan9FileSystem> Plan9Server;
        bool Plan9Socket = false;
    };

    HcsVirtualMachineBackend();
    void Initialize(const VmCreateRequest& Request);
    VmConfiguration BuildConfiguration(const VmCreateRequest& Request);
    static void CALLBACK OnSystemEvent(HCS_EVENT* Event, void* Context) noexcept;
    void OnCrash(PCWSTR Details);
    void OnExit(PCWSTR ExitDetails);
    void CleanupAttachedDisks(std::map<std::uint64_t, AttachedDisk>&& Disks) noexcept;
    std::shared_ptr<VmGuestListenerState> ConfigureGuestListener(const VmGuestListener& Listener) override;

    _Requires_lock_held_(m_lock)
    std::uint32_t ReserveLunLocked(const std::optional<VmScsiPlacement>& Placement) const;

    _Requires_lock_held_(m_lock)
    std::map<std::uint64_t, AttachedDisk>::iterator FindAttachedDiskLocked(bool PassThrough, const std::wstring& Path);

    NON_COPYABLE(HcsVirtualMachineBackend);
    NON_MOVABLE(HcsVirtualMachineBackend);

    VmConfiguration m_configuration;
    std::wstring m_vmIdString;
    wil::unique_event m_terminatingEvent{wil::EventOptions::ManualReset};
    wil::unique_event m_exitEvent{wil::EventOptions::ManualReset};
    wil::unique_event m_vmCrashEvent{wil::EventOptions::ManualReset};
    std::optional<VmCrashCaptureRequest> m_crashCapture;
    std::optional<std::filesystem::path> m_vmCrashLogFile;
    wil::srwlock m_exitDetailsLock;
    _Guarded_by_(m_exitDetailsLock) std::wstring m_exitDetails;
    // Closing the system drains callbacks before their event and context are destroyed.
    _Guarded_by_(m_lock) wsl::windows::common::hcs::unique_hcs_system m_system;
    _Guarded_by_(m_lock) std::map<std::uint64_t, AttachedDisk> m_attachedDisks;
    _Guarded_by_(m_lock) std::uint64_t m_nextDiskId = 1;
    _Guarded_by_(m_lock) std::map<std::uint64_t, FileSystemDevice> m_fileSystemDevices;
    _Guarded_by_(m_lock) std::uint64_t m_nextDeviceId = 1;
    wil::unique_handle m_restrictedToken;

    _Guarded_by_(m_lock) std::shared_ptr<GuestDeviceManager> m_guestDeviceManager;
    _Guarded_by_(m_lock) GUID m_runtimeId {};
};
