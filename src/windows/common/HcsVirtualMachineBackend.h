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
    void UpdateNetworkAdapter(VmDeviceId Device, const VmNetworkConfiguration& Configuration) override;
    void RemoveNetworkAdapter(VmDeviceId Device) override;
    VmPortBinding BindPort(VmDeviceId Device, const VmPortBindingRequest& Request) override;
    void UnbindPort(VmPortBindingId Binding) override;
    IpAddress CreateVirtualAddress(VmDeviceId Device, const IpAddress& Destination) override;
    void CreateDnsRecord(VmDeviceId Device, const VmDnsRecord& Record) override;

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
    };

    using FileSystemDeviceMap = std::map<std::uint64_t, FileSystemDevice>;

    struct FileSystemShare
    {
        VmFileSystemShare Share;
        // Options the share was created with. Together with the host path these identify a virtio-fs
        // share, so a repeated request reuses the existing share instead of creating a second one.
        std::wstring MountOptions;
        // Token the share was created with, if any. A Plan 9 share is removed under the same identity
        // that added it. Unset shares fall back to the VM identity token.
        wil::shared_handle UserToken;
    };

    struct NetworkAdapter
    {
        VmNetworkAttachment Attachment;
        // Resource path the adapter was added at. Set only for host endpoint networks, which are
        // removed from the compute system at the same path that added them.
        std::wstring ResourcePath;
    };

    struct PortBinding
    {
        VmPortBinding Binding;
        // Tag of the user-mode NAT device that owns the binding.
        std::wstring Tag;
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
    void CloseFileSystemDevicesLocked() noexcept;

    _Requires_lock_held_(m_lock)
    std::uint32_t ReserveLunLocked(const std::optional<VmScsiPlacement>& Placement) const;

    _Requires_lock_held_(m_lock)
    std::map<std::uint64_t, AttachedDisk>::iterator FindAttachedDiskLocked(bool PassThrough, const std::wstring& Path);

    _Requires_lock_held_(m_lock)
    FileSystemDeviceMap::iterator FindFileSystemDeviceLocked(VmDeviceId Device);

    /// <summary>
    /// Returns the share of Device that already serves HostPath with MountOptions, if there is one.
    /// </summary>
    _Requires_lock_held_(m_lock)
    const FileSystemShare* FindFileSystemShareLocked(VmDeviceId Device, const std::wstring& HostPath, const std::wstring& MountOptions) const;

    /// <summary>
    /// Resolves the token used to reach the host path of a share, preferring the one on the request.
    /// </summary>
    HANDLE ResolveShareUserToken(const VmFileSystemShareRequest& Request) const;

    /// <summary>
    /// Adds a share to a Plan 9 device and returns the name the guest uses to reach it.
    /// </summary>
    _Requires_lock_held_(m_lock)
    std::wstring AddPlan9ShareLocked(
        const FileSystemDevice& Device, const VmFileSystemShareRequest& Request, HANDLE UserToken, const std::wstring& HostPath) const;

    /// <summary>
    /// Removes a share from a Plan 9 device by the name the guest uses to reach it.
    /// </summary>
    _Requires_lock_held_(m_lock)
    void RemovePlan9ShareLocked(const FileSystemDevice& Device, const std::wstring& AccessName, HANDLE UserToken) const;

    _Requires_lock_held_(m_lock)
    std::map<std::uint64_t, NetworkAdapter>::iterator FindNetworkAdapterLocked(VmDeviceId Device);

    /// <summary>
    /// Returns the virtio-net device that backs a user-mode NAT adapter, failing the call when the
    /// adapter is served by a host endpoint instead.
    /// </summary>
    _Requires_lock_held_(m_lock)
    wil::com_ptr<IWslVirtioNetDevice> GetUserModeNatDeviceLocked(VmDeviceId Device) const;

    /// <summary>
    /// Adds or removes a host endpoint adapter at ResourcePath. HCS reports transient failures while
    /// the host network stack settles, so the modification is retried.
    /// </summary>
    _Requires_lock_held_(m_lock)
    void ModifyHostEndpointLocked(
        const VmHostEndpointNetwork& Configuration, const std::wstring& ResourcePath, wsl::windows::common::hcs::ModifyRequestType RequestType) const;

    /// <summary>
    /// Removes the adapter's tracked state, tearing down the resource that serves it. Port bindings
    /// on the adapter are dropped because the device that tracked them is gone.
    /// </summary>
    _Requires_lock_held_(m_lock)
    void RemoveNetworkAdapterLocked(std::map<std::uint64_t, NetworkAdapter>::iterator Adapter);

    _Requires_lock_held_(m_lock)
    void CloseNetworkAdaptersLocked() noexcept;

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
    _Guarded_by_(m_lock) FileSystemDeviceMap m_fileSystemDevices;
    _Guarded_by_(m_lock) std::uint64_t m_nextDeviceId = 1;
    _Guarded_by_(m_lock) std::map<std::uint64_t, FileSystemShare> m_fileSystemShares;
    _Guarded_by_(m_lock) std::uint64_t m_nextShareId = 1;
    _Guarded_by_(m_lock) std::vector<VmNetworkAdapterRequest> m_pendingNetworkAdapters;
    _Guarded_by_(m_lock) std::map<std::uint64_t, NetworkAdapter> m_networkAdapters;
    _Guarded_by_(m_lock) std::map<std::uint64_t, PortBinding> m_portBindings;
    _Guarded_by_(m_lock) std::uint64_t m_nextPortBindingId = 1;
    wil::unique_handle m_restrictedToken;

    _Guarded_by_(m_lock) std::shared_ptr<GuestDeviceManager> m_guestDeviceManager;
    _Guarded_by_(m_lock) GUID m_runtimeId {};
};
