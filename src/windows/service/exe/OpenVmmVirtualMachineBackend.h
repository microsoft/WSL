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

    static std::unique_ptr<OpenVmmVirtualMachineBackend> Create(const VmCreateRequest& Request);

    static VmPlatformCapabilities QueryCapabilities();

    VmPlatformCapabilities GetCapabilities() const override;
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

    struct State
    {
        struct AttachedDisk
        {
            VmDiskAttachment Attachment;
            wil::unique_hfile BackingFile;
        };

        wil::srwlock m_lock;
        VmDescription m_description;
        _Guarded_by_(m_lock) std::map<std::uint64_t, AttachedDisk> m_attachedDisks;
        _Guarded_by_(m_lock) std::uint64_t m_nextDiskId = 1;
        UniqueVm m_vm;
        wil::unique_handle m_process;
        wil::unique_handle m_job;
        std::vector<wil::unique_hfile> m_backingFiles;
        std::filesystem::path m_socketDirectory;
        std::filesystem::path m_rpcSocketPath;
        std::filesystem::path m_vsockPath;
        bool m_directoryCreated = false;
        wil::unique_event m_exitEvent{wil::EventOptions::ManualReset};
        wil::unique_threadpool_wait m_processWait;
    };

    std::unique_ptr<State> m_state;
};