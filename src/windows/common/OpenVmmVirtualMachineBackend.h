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
void ApplyCrashDumpRetention(const std::filesystem::path& DumpPath, std::uint32_t MaxCount, HANDLE UserToken);

} // namespace wsl::windows::common::vm::openvmm

class OpenVmmVirtualMachineBackend : public IVirtualMachineBackend
{
public:
    ~OpenVmmVirtualMachineBackend() noexcept override;

    // Networking supports creation-time Consomme NICs, automatic guest IPv6 and host DNS. Hot-add is unsupported.
    // Backend failures include OpenVMM in the contextualized error code.
    static std::unique_ptr<OpenVmmVirtualMachineBackend> Create(const VmCreateRequest& Request);

    static VmPlatformCapabilities QueryCapabilities();

    VmPlatformCapabilities GetCapabilities() const override;
    VmDescription GetDescription() const override;
    VmState GetState() const override;
    VmTerminationInformation GetTerminationReason() const override;
    wil::shared_event GetTerminationEvent() const override;
    wil::shared_event GetCrashEvent() const override;
    std::optional<std::filesystem::path> GetCrashLogPath() const override;
    void Start() override;
    void Terminate() noexcept override;

    VmGuestListener CreateGuestListener(GuestServicePort Port) override;
    wil::unique_socket ConnectGuest(GuestServicePort Port, _In_opt_ HANDLE ExitHandle = nullptr) override;
    wsl::windows::common::GuestConnector GetGuestConnector() const override
    {
        return wsl::windows::common::GuestConnector::CreateOpenVmm(m_fileSystemResources.VsockPath);
    }
    std::optional<VmHostSocket> CreateHostSocket(GuestServicePort Port) override;
    void CloseHostSocket(VmHostSocketId Socket) override;

    VmDiskAttachment AttachDisk(const VmDiskRequest& Request) override;
    std::vector<VmDiskAttachment> GetAttachedDisks() const override;
    void DetachDisk(VmDiskId Disk) override;

    VmPersistentMemoryDevice AddPersistentMemory(const VmPersistentMemoryRequest& Request) override;
    VmGpuAttachment AddGpu(const VmGpuRequest& Request) override;

    VmFileSystemDevice CreateFileSystemDevice(const VmFileSystemDeviceRequest& Request) override;
    std::optional<VmFileSystemDevice> GetFileSystemDevice(const VmFileSystemDevicePredicate& Predicate) const override;
    VmFileSystemDevice GetFileSystemDeviceStatus(VmDeviceId Device) override;
    VmFileSystemShare AddFileSystemShare(VmDeviceId Device, const VmFileSystemShareRequest& Request) override;
    std::optional<VmFileSystemShare> GetFileSystemShare(const VmFileSystemSharePredicate& Predicate) const override;
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

    static void CALLBACK OnProcessSignaled(PTP_CALLBACK_INSTANCE Instance, void* Context, PTP_WAIT Wait, TP_WAIT_RESULT Result) noexcept;
    void OnProcessExit(std::optional<DWORD> ExitCode) noexcept;
    void ReadProcessLog(wil::unique_hfile Pipe) noexcept;
    void Initialize(const VmCreateRequest& Request);

    static void DestroyVm(WslOpenVmmVm* Vm) noexcept;
    using UniqueVm = wil::unique_any<WslOpenVmmVm*, decltype(&DestroyVm), DestroyVm>;

    using AttachedDisk = VmResource<VmDiskAttachment, GUID>;

    struct OpenVmmFileSystemDevice
    {
        std::optional<std::uint64_t> Share;
        wil::shared_handle UserToken;
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

    _Requires_lock_held_(m_lock)
    void AddFileSystemShareLocked(const VmFileSystemShare& Share, const wil::shared_handle& Token);
    _Requires_lock_held_(m_lock)
    void RemoveFileSystemShareLocked(VmShareId Share);

    VmDescription m_description{};
    _Guarded_by_(m_lock) std::map<std::uint64_t, AttachedDisk> m_attachedDisks;
    _Guarded_by_(m_lock) std::uint64_t m_nextDiskId = 1;
    _Guarded_by_(m_lock) std::map<std::uint64_t, VmPersistentMemoryDevice> m_persistentMemoryDevices;
    _Guarded_by_(m_lock) std::map<std::uint64_t, FileSystemDevice> m_fileSystemDevices;
    _Guarded_by_(m_lock) std::map<std::uint64_t, VmFileSystemShare> m_fileSystemShares;
    _Guarded_by_(m_lock) std::map<std::uint64_t, NetworkAdapter> m_networkAdapters;
    _Guarded_by_(m_lock) std::map<std::uint64_t, PortBinding> m_portBindings;
    _Guarded_by_(m_lock) std::uint64_t m_nextDeviceId = 1;
    _Guarded_by_(m_lock) std::uint64_t m_nextShareId = 1;
    _Guarded_by_(m_lock) std::uint64_t m_nextPortBindingId = 1;
    _Guarded_by_(m_lock) std::map<std::uint64_t, VmHostSocket> m_hostSockets;
    _Guarded_by_(m_lock) std::uint64_t m_nextHostSocketId = 1;
    _Guarded_by_(m_lock) std::uint32_t m_nextPersistentMemoryIndex = 0;
    wil::srwlock m_terminationLock;
    UniqueVm m_vm;
    wil::unique_handle m_process;
    wil::unique_handle m_job;
    std::thread m_processLogThread;
    wil::unique_threadpool_wait m_processExitWait;
    SessionFileSystemResources m_fileSystemResources;
    std::optional<std::filesystem::path> m_crashDumpPath;
    std::uint32_t m_maxSavedStateCount = 0;
    wil::shared_event m_exitEvent{wil::EventOptions::ManualReset};
    wil::shared_event m_crashEvent{wil::EventOptions::ManualReset};
    _Guarded_by_(m_lock) VmState m_state = VmState::Unknown;
    _Guarded_by_(m_lock) VmTerminationInformation m_terminationInformation;
};