/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    HcsVirtualMachineBackend.h

Abstract:

    Implementation of IVirtualMachineBackend - represents a single HCS-based VM instance.

--*/

#pragma once

#include "IVirtualMachineBackend.h"
#include "hcs.hpp"

class HcsVirtualMachineBackend : public IVirtualMachineBackend
{
public:
    ~HcsVirtualMachineBackend() noexcept override;

    static std::unique_ptr<HcsVirtualMachineBackend> Create(const VmCreateRequest& Request);

    VmPlatformCapabilities GetCapabilities() const override;
    VmDescription GetDescription() const override;
    wil::unique_handle GetTerminationEvent() const override;
    void Start() override;
    void Terminate() override;

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
    struct VmConfiguration
    {
        VmDescription Description;
        wsl::windows::common::hcs::ComputeSystem Settings;
    };

    HcsVirtualMachineBackend();
    void Initialize(const VmCreateRequest& Request);
    VmConfiguration BuildConfiguration(const VmCreateRequest& Request);
    static void CALLBACK OnSystemEvent(HCS_EVENT* Event, void* Context) noexcept;
    void OnCrash(PCWSTR Details);
    void OnExit(PCWSTR ExitDetails);
    NON_COPYABLE(HcsVirtualMachineBackend);
    NON_MOVABLE(HcsVirtualMachineBackend);

    wil::srwlock m_lock;
    VmConfiguration m_configuration;
    wil::unique_event m_terminatingEvent{wil::EventOptions::ManualReset};
    wil::unique_event m_exitEvent{wil::EventOptions::ManualReset};
    wil::unique_event m_vmCrashEvent{wil::EventOptions::ManualReset};
    std::optional<VmCrashCaptureRequest> m_crashCapture;
    std::optional<std::filesystem::path> m_vmCrashLogFile;
    wil::srwlock m_exitDetailsLock;
    _Guarded_by_(m_exitDetailsLock) std::wstring m_exitDetails;
    // Closing the system drains callbacks before their event and context are destroyed.
    _Guarded_by_(m_lock) wsl::windows::common::hcs::unique_hcs_system m_system;
    wil::unique_handle m_restrictedToken;
};