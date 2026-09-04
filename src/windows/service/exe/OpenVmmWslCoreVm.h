// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include "IWslCoreVm.h"
#include "SocketChannel.h"

class DmesgCollector;

namespace wsl::windows::service {
class OpenVmmGrpcClient;
}

class OpenVmmWslCoreVm final : public IWslCoreVm
{
public:
    static std::unique_ptr<OpenVmmWslCoreVm> Create(
        _In_ const wil::shared_handle& UserToken,
        _In_ wsl::core::Config&& VmConfig,
        _In_ const GUID& VmId,
        _In_ InitializeDrvFsCallback InitializeDrvFs);

    static void ForceTerminate(_In_ const GUID& VmId);

    ~OpenVmmWslCoreVm() noexcept override;

    ULONG AttachDisk(
        _In_ PCWSTR Disk,
        _In_ DiskType Type,
        _In_ std::optional<ULONG> Lun,
        _In_ bool IsUserDisk,
        _In_ HANDLE UserToken) override;
    wil::unique_socket ConnectToGuest(_In_ ULONG Port) const override;
    std::shared_ptr<LxssRunningInstance> CreateInstance(
        _In_ const GUID& InstanceId,
        _In_ const LXSS_DISTRO_CONFIGURATION& Configuration,
        _In_ LX_MESSAGE_TYPE MessageType,
        _In_ DWORD ReceiveTimeout,
        _In_ ULONG DefaultUid,
        _In_ ULONG64 ClientLifetimeId,
        _In_ ULONG ExportFlags,
        _Out_opt_ ULONG* ConnectPort) override;
    wil::unique_socket CreateRootNamespaceProcess(_In_ LPCSTR Path, _In_ LPCSTR* Arguments) override;
    std::pair<int, LX_MINI_MOUNT_STEP> DetachDisk(_In_opt_ PCWSTR Disk) override;
    void EjectVhd(_In_ PCWSTR VhdPath) override;
    const wsl::core::Config& GetConfig() const noexcept override;
    GUID GetRuntimeId() const override;
    int GetVmIdleTimeout() const override;
    bool InitializeDrvFs(_In_ HANDLE UserToken) override;
    bool IsVhdAttached(_In_ PCWSTR VhdPath) override;
    DiskMountResult MountDisk(
        _In_ PCWSTR Disk,
        _In_ DiskType MountDiskType,
        _In_ ULONG PartitionIndex,
        _In_opt_ PCWSTR Name,
        _In_opt_ PCWSTR Type,
        _In_opt_ PCWSTR Options) override;
    void MountRootNamespaceFolder(
        _In_ LPCWSTR HostPath, _In_ LPCWSTR GuestPath, _In_ bool ReadOnly, _In_ LPCWSTR Name) override;
    void RegisterCallbacks(
        _In_ const std::function<void(ULONG)>& DistroExitCallback,
        _In_ const std::function<void(GUID)>& TerminationCallback) override;
    void ResizeDistribution(_In_ ULONG Lun, _In_ HANDLE OutputHandle, _In_ ULONG64 NewSize) override;
    void SaveAttachedDisksState() override;
    void TrimDistribution(_In_ ULONG Lun) override;

private:
    struct Mount
    {
        std::wstring Name;
        std::optional<std::wstring> Options;
        std::optional<std::wstring> Type;
    };

    struct AttachedDisk
    {
        DiskType Type;
        std::filesystem::path Path;
        bool ReadOnly;
        bool User;
        std::map<ULONG, Mount> Mounts;
        wil::unique_hfile BackingFile;
    };

    struct VirtioFsShare
    {
        std::filesystem::path Path;
        std::wstring Options;
        bool Admin;
        std::wstring Tag;
        GUID InstanceId;
    };

    OpenVmmWslCoreVm(
        _In_ const wil::shared_handle& UserToken,
        _In_ wsl::core::Config&& VmConfig,
        _In_ const GUID& VmId,
        _In_ InitializeDrvFsCallback InitializeDrvFs);

    wil::unique_socket AcceptConnection(_In_ DWORD ReceiveTimeout = 0) const;
    wil::unique_socket AcceptConnection(
        _In_ SOCKET ListenSocket, _In_ DWORD AcceptTimeout, _In_ DWORD ReceiveTimeout = 0) const;
    _Requires_lock_held_(m_guestDeviceLock)
    std::tuple<std::wstring, std::wstring, std::wstring> AddVirtioFsShare(
        _In_ bool Admin, _In_ const std::wstring& Path, _In_ const std::wstring& Options);
    _Requires_lock_held_(m_lock)
    ULONG AttachDiskLockHeld(
        _In_ PCWSTR Disk,
        _In_ DiskType Type,
        _In_ std::optional<ULONG> Lun,
        _In_ bool IsUserDisk,
        _In_ bool ReadOnly);
    std::wstring BuildCommandLine() const;
    std::wstring BuildKernelCommandLine() const;
    void ConfigureVmService() const;
    std::pair<wil::unique_socket, std::filesystem::path> CreateVsockListener(_In_ ULONG Port) const;
    void Initialize();
    void InitializeConfiguration();
    void InitializeGuest();
    void LaunchOpenVmm();
    std::vector<char> ProcessVirtioFsRequest(_In_ gsl::span<gsl::byte> Request);
    void ReadGuestCapabilities();
    static std::string GetMountTargetName(_In_ PCWSTR Disk, _In_opt_ PCWSTR Name, _In_ int PartitionIndex);
    static std::pair<int, LX_MINI_MOUNT_STEP> GetMountResult(_In_ wsl::shared::SocketChannel& Channel);
    _Requires_lock_held_(m_lock)
    void FreeLun(_In_ ULONG Lun);
    _Requires_lock_held_(m_lock)
    ULONG ReserveLun(_In_ std::optional<ULONG> Lun = {});
    _Requires_lock_held_(m_lock)
    std::pair<int, LX_MINI_MOUNT_STEP> UnmountDisk(_In_ ULONG Lun, _Inout_ AttachedDisk& Disk);
    std::pair<int, LX_MINI_MOUNT_STEP> UnmountVolume(_In_ PCWSTR Name);
    void UnregisterProcess() noexcept;
    void VirtioFsWorker() noexcept;

    static void CALLBACK OnProcessExit(_Inout_ PTP_CALLBACK_INSTANCE, _In_opt_ void* Context, _Inout_ PTP_WAIT, _In_ TP_WAIT_RESULT) noexcept;

    static constexpr ULONG c_maxVhdCount = 254;
    static constexpr int c_pageReportingOrder = 5;
    static constexpr DWORD c_shutdownTimeoutMs = 30 * 1000;
    static constexpr DWORD c_processTerminationTimeoutMs = 5 * 1000;
    static constexpr uint32_t c_nicGuidXorMask = 0x4E494300;
    static constexpr wchar_t c_defaultConsommeMacAddress[] = L"00-15-5D-00-00-01";

    wil::shared_handle m_userToken;
    wil::unique_handle m_restrictedToken;
    wsl::core::Config m_vmConfig;
    GUID m_vmId{};
    std::wstring m_machineId;
    std::filesystem::path m_installPath;
    std::filesystem::path m_rootFsPath;
    std::filesystem::path m_initrdPath;
    std::filesystem::path m_openVmmPath;
    std::filesystem::path m_vsockPath;
    std::filesystem::path m_grpcSocketPath;
    std::filesystem::path m_listenPath;
    std::wstring m_userProfile;
    std::wstring m_comPipe0;
    std::wstring m_kernelVersionString;
    std::tuple<std::uint32_t, std::uint32_t, std::uint32_t> m_kernelVersion{};
    SE_SID m_userSid{};
    ULONG m_systemDistroDeviceId = ULONG_MAX;
    ULONG m_kernelModulesDeviceId = ULONG_MAX;
    bool m_defaultKernel = true;
    bool m_creatorElevated = false;
    bool m_processRegistered = false;
    bool m_seccompAvailable = false;
    uint64_t m_hvPciSwiotlbBase = 0;
    uint64_t m_hvPciSwiotlbSize = 0;
    InitializeDrvFsCallback m_initializeDrvFs;
    wil::srwlock m_guestDeviceLock;
    _Guarded_by_(m_guestDeviceLock) std::vector<VirtioFsShare> m_virtioFsShares;
    wil::srwlock m_exitCallbackLock;
    _Guarded_by_(m_exitCallbackLock) std::function<void(GUID)> m_terminationCallback;
    wil::srwlock m_lock;
    _Guarded_by_(m_lock) std::bitset<c_maxVhdCount> m_lunBitmap;
    _Guarded_by_(m_lock) std::map<ULONG, AttachedDisk> m_attachedDisks;
    wil::unique_event m_terminatingEvent{wil::EventOptions::ManualReset};
    wil::unique_event m_vmExitEvent{wil::EventOptions::ManualReset};
    wil::unique_handle m_processJobObject;
    wil::unique_handle m_processHandle;
    std::unique_ptr<wsl::windows::service::OpenVmmGrpcClient> m_vmService;
    wil::unique_socket m_listenSocket;
    wsl::shared::SocketChannel m_miniInitChannel;
    wil::unique_socket m_notifyChannel;
    wil::unique_socket m_gnsSocket;
    wil::unique_socket m_virtioFsListenSocket;
    std::filesystem::path m_virtioFsListenPath;
    std::thread m_distroExitThread;
    std::thread m_virtioFsThread;
    std::shared_ptr<DmesgCollector> m_dmesgCollector;
    wil::unique_threadpool_wait m_processWait;
};