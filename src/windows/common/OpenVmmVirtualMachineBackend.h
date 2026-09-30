/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    OpenVmmVirtualMachineBackend.h

Abstract:

    Implementation of IVirtualMachineBackend - represents a single OpenVMM-based VM instance.

--*/

#pragma once

#include <thread>
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
    VmState GetState() const override;
    VmTerminationInformation GetTerminationReason() const override;
    wil::unique_handle GetTerminationEvent() const override;
    void Start() override;
    void Terminate() override;

    VmGuestListener CreateGuestListener(GuestServicePort Port) override;
    wil::unique_socket ConnectGuest(GuestServicePort Port) override;
    void CloseGuestListener(VmListenerId Listener) override;

    VmDiskAttachment AttachDisk(const VmDiskRequest& Request) override;
    void DetachDisk(VmDiskId Disk) override;

    VmPersistentMemoryDevice AddPersistentMemory(const VmPersistentMemoryRequest& Request) override;
    VmGpuAttachment AddGpu(const VmGpuRequest& Request) override;

    VmFileSystemDevice CreateFileSystemDevice(const VmFileSystemDeviceRequest& Request) override;
    VmFileSystemDevice GetFileSystemDeviceStatus(VmDeviceId Device) override;
    VmFileSystemShare AddFileSystemShare(VmDeviceId Device, const VmFileSystemShareRequest& Request) override;
    void RemoveFileSystemShare(VmShareId Share) override;
    VmSharedMemoryDevice AddSharedMemory(const VmSharedMemoryRequest& Request) override;
    void ConfigureGuestDma(const VmGuestDmaRequest& Request) override;
    void RemoveDevice(VmDeviceId Device) override;

    VmNetworkAttachment AddNetworkAdapter(const VmNetworkAdapterRequest& Request) override;
    void UpdateNetworkAdapter(VmDeviceId Device, const VmNetworkConfiguration& Configuration) override;
    void RemoveNetworkAdapter(VmDeviceId Device) override;
    VmPortBinding BindPort(VmDeviceId Device, const VmPortBindingRequest& Request) override;
    void UnbindPort(VmPortBindingId Binding) override;
    IpAddress CreateVirtualAddress(VmDeviceId Device, const IpAddress& Destination) override;
    void CreateDnsRecord(VmDeviceId Device, const VmDnsRecord& Record) override;

private:
    OpenVmmVirtualMachineBackend();
    NON_COPYABLE(OpenVmmVirtualMachineBackend);
    NON_MOVABLE(OpenVmmVirtualMachineBackend);

    void OnProcessExit(DWORD ExitCode) noexcept;
    void ReadProcessLog(wil::unique_hfile Pipe) noexcept;
    void Initialize(const VmCreateRequest& Request);

    static void DestroyVm(WslOpenVmmVm* Vm) noexcept;
    using UniqueVm = wil::unique_any<WslOpenVmmVm*, decltype(&DestroyVm), DestroyVm>;

    std::shared_ptr<VmGuestListenerState> ConfigureGuestListener(const VmGuestListener& Listener) override;

    struct GuestListener : VmGuestListenerState
    {
        ~GuestListener() noexcept;
        std::optional<wil::unique_socket> Accept();

        wil::unique_hfile SocketFile;
    };

    struct OpenVmmFileSystemDevice
    {
        std::optional<std::uint64_t> Share;
    };

    using FileSystemDevice = VmResource<VmFileSystemDevice, OpenVmmFileSystemDevice>;

    struct OpenVmmNetworkAdapter
    {
        std::wstring NicId;
    };

    using NetworkAdapter = VmResource<VmNetworkAttachment, OpenVmmNetworkAdapter>;

    struct OpenVmmPortBinding
    {
        std::wstring NicId;
        std::wstring HostAddress;
    };

    using PortBinding = VmResource<VmPortBinding, OpenVmmPortBinding>;

    struct SessionFileSystemResources
    {
        std::filesystem::path SocketDirectory;
        std::filesystem::path RpcSocketPath;
        std::filesystem::path VsockPath;
        bool DirectoryCreated = false;
    };

    VmDescription m_description{};
    _Guarded_by_(m_lock) std::map<std::uint64_t, VmDiskAttachment> m_attachedDisks;
    _Guarded_by_(m_lock) std::uint64_t m_nextDiskId = 1;
    _Guarded_by_(m_lock) std::map<std::uint64_t, FileSystemDevice> m_fileSystemDevices;
    _Guarded_by_(m_lock) std::map<std::uint64_t, VmFileSystemShare> m_fileSystemShares;
    _Guarded_by_(m_lock) std::map<std::uint64_t, NetworkAdapter> m_networkAdapters;
    _Guarded_by_(m_lock) std::map<std::uint64_t, PortBinding> m_portBindings;
    _Guarded_by_(m_lock) std::uint64_t m_nextDeviceId = 1;
    _Guarded_by_(m_lock) std::uint64_t m_nextShareId = 1;
    _Guarded_by_(m_lock) std::uint64_t m_nextPortBindingId = 1;
    UniqueVm m_vm;
    wil::unique_handle m_process;
    wil::unique_handle m_job;
    std::thread m_processLogThread;
    SessionFileSystemResources m_fileSystemResources;
    wil::unique_event m_exitEvent{wil::EventOptions::ManualReset};
    _Guarded_by_(m_lock) VmState m_state = VmState::Unknown;
    _Guarded_by_(m_lock) VmTerminationInformation m_terminationInformation;
};