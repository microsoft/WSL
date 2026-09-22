/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    OpenVmmVirtualMachineBackend.h

Abstract:

    Implementation of IVirtualMachineBackend - represents a single OpenVMM-based VM instance.

--*/

#pragma once

#include "IVirtualMachineBackend.h"

struct WslOpenVmmVm;

namespace wsl::windows::common::vm::openvmm {

VmDescription ValidateCreateRequest(const VmCreateRequest& Request);

}

class OpenVmmVirtualMachineBackend : public IVirtualMachineBackend
{
public:
    ~OpenVmmVirtualMachineBackend() noexcept override;

    // Networking supports one creation-time Consomme NIC: 10.0.0.2/24, gateway 10.0.0.1,
    // default gateway MACs, automatic guest IPv6 and host DNS. Custom settings and hot-add are unsupported.
    static std::unique_ptr<OpenVmmVirtualMachineBackend> Create(const VmCreateRequest& Request);

    static VmPlatformCapabilities QueryCapabilities();

    VmPlatformCapabilities GetCapabilities() const override;
    VmDescription GetDescription() const override;
    wil::unique_handle GetTerminationEvent() const override;
    void Start() override;
    void Terminate() override;
    void CancelPendingOperations() noexcept override;

    VmGuestListener CreateGuestListener(GuestServicePort Port) override;
    wil::unique_socket AcceptGuestConnection(VmListenerId Listener) override;
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
    OpenVmmVirtualMachineBackend();
    NON_COPYABLE(OpenVmmVirtualMachineBackend);
    NON_MOVABLE(OpenVmmVirtualMachineBackend);

    static void CALLBACK OnProcessExit(PTP_CALLBACK_INSTANCE, void* Context, PTP_WAIT, TP_WAIT_RESULT) noexcept;
    void Initialize(const VmCreateRequest& Request);

    static void DestroyVm(WslOpenVmmVm* Vm) noexcept;
    using UniqueVm = wil::unique_any<WslOpenVmmVm*, decltype(&DestroyVm), DestroyVm>;

    struct AttachedDisk
    {
        VmDiskAttachment Attachment;
        wil::unique_hfile BackingFile;
    };

    struct GuestListener
    {
        ~GuestListener() noexcept;

        VmGuestListener Listener;
        wil::unique_socket Socket;
        std::filesystem::path Path;
        wil::unique_event CancellationEvent{wil::EventOptions::ManualReset};
    };

    struct FileSystemDevice
    {
        VmFileSystemDevice Device;
        VmVirtioFsDevice Transport;
        std::optional<std::uint64_t> Share;
    };

    struct FileSystemShare
    {
        VmFileSystemShare Share;
    };

    struct NetworkAdapter
    {
        VmNetworkAttachment Attachment;
        std::wstring NicId;
    };

    struct PortBinding
    {
        VmPortBinding Binding;
        std::wstring NicId;
        std::wstring HostAddress;
    };

    wil::srwlock m_lock;
    _Requires_lock_held_(m_lock)
    void CloseGuestListeners() noexcept;

    VmDescription m_description{};
    _Guarded_by_(m_lock) std::map<std::uint64_t, AttachedDisk> m_attachedDisks;
    _Guarded_by_(m_lock) std::uint64_t m_nextDiskId = 1;
    _Guarded_by_(m_lock) std::map<std::uint64_t, std::shared_ptr<GuestListener>> m_guestListeners;
    _Guarded_by_(m_lock) std::uint64_t m_nextListenerId = 1;
    _Guarded_by_(m_lock) std::map<std::uint64_t, FileSystemDevice> m_fileSystemDevices;
    _Guarded_by_(m_lock) std::map<std::uint64_t, FileSystemShare> m_fileSystemShares;
    _Guarded_by_(m_lock) std::map<std::uint64_t, NetworkAdapter> m_networkAdapters;
    _Guarded_by_(m_lock) std::map<std::uint64_t, PortBinding> m_portBindings;
    _Guarded_by_(m_lock) std::uint64_t m_nextDeviceId = 1;
    _Guarded_by_(m_lock) std::uint64_t m_nextShareId = 1;
    _Guarded_by_(m_lock) std::uint64_t m_nextPortBindingId = 1;
    UniqueVm m_vm;
    wil::unique_handle m_process;
    wil::unique_handle m_job;
    std::vector<wil::unique_hfile> m_backingFiles;
    std::filesystem::path m_socketDirectory;
    std::filesystem::path m_rpcSocketPath;
    std::filesystem::path m_vsockPath;
    bool m_directoryCreated = false;
    wil::unique_event m_exitEvent{wil::EventOptions::ManualReset};
    wil::unique_event m_operationCancellationEvent{wil::EventOptions::ManualReset};
    wil::unique_threadpool_wait m_processWait;
};