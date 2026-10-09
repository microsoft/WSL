/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WslCoreVm.h

Abstract:

    This file contains WSL Core vm function declarations.

--*/

#pragma once

#include "hcs.hpp"
#include "IVirtualMachineBackend.h"
#include "GnsChannel.h"
#include "LxssPort.h"
#include "GnsRpcServer.h"
#include "GnsPortTrackerChannel.h"
#include "Dmesg.h"
#include "GuestTelemetryLogger.h"
#include "WslCoreConfig.h"
#include "LxssCreateProcess.h"
#include "WslCoreNetworkEndpointSettings.h"
#include "WslSecurity.h"
#include "WslCoreFilesystem.h"
#include "INetworkingEngine.h"
#include "SocketChannel.h"
#include "DeviceHostProxy.h"

#define UTILITY_VM_SHUTDOWN_TIMEOUT (30 * 1000)
#define UTILITY_VM_TERMINATE_TIMEOUT (30 * 1000)

inline constexpr auto c_diskValueName = L"Disk";
inline constexpr auto c_disktypeValueName = L"DiskType";
inline constexpr auto c_optionsValueName = L"Options";
inline constexpr auto c_typeValueName = L"Type";
inline constexpr auto c_mountNameValueName = L"Name";

namespace wrl = Microsoft::WRL;

/// <summary>
/// This object tracks a running WSL Core VM.
/// </summary>
class WslCoreVm
{
    WslCoreVm(const WslCoreVm&) = delete;
    void operator=(const WslCoreVm&) = delete;

public:
    using InitializeDrvFsCallback = std::function<LX_INIT_DRVFS_MOUNT(HANDLE)>;

    static std::unique_ptr<WslCoreVm> Create(
        _In_ const wil::shared_handle& UserToken, _In_ wsl::core::Config&& VmConfig, _In_ const GUID& VmId, _In_ InitializeDrvFsCallback InitializeDrvFs);

    ~WslCoreVm() noexcept;

    wil::unique_socket AcceptConnection(_In_ DWORD ReceiveTimeout = 0, _In_ const std::source_location& Location = std::source_location::current()) const;

    enum class DiskType
    {
        Invalid = 0x0,
        VHD = 0x1,
        PassThrough = 0x2
    };

    ULONG AttachDisk(_In_ PCWSTR Disk, _In_ DiskType Type, _In_ std::optional<ULONG> Lun, _In_ bool IsUserDisk, _In_ HANDLE UserToken);

    std::shared_ptr<LxssRunningInstance> CreateInstance(
        _In_ const GUID& InstanceId,
        _In_ const LXSS_DISTRO_CONFIGURATION& Configuration,
        _In_ LX_MESSAGE_TYPE MessageType,
        _In_ DWORD ReceiveTimeout = 0,
        _In_ ULONG DefaultUid = LX_UID_ROOT,
        _In_ ULONG64 ClientLifetimeId = 0,
        _In_ ULONG ExportFlags = 0,
        _Out_opt_ ULONG* ConnectPort = nullptr);

    wil::unique_socket CreateRootNamespaceProcess(_In_ LPCSTR Path, _In_ LPCSTR* Arguments);

    std::pair<int, LX_MINI_MOUNT_STEP> DetachDisk(_In_opt_ PCWSTR Disk);

    struct DiskMountResult
    {
        std::string MountPointName;
        int Result;
        LX_MINI_MOUNT_STEP Step;
    };

    void EjectVhd(_In_ PCWSTR VhdPath);

    const wsl::core::Config& GetConfig() const noexcept;

    GUID GetRuntimeId() const;

    int GetVmIdleTimeout() const;

    bool InitializeDrvFs(_In_ HANDLE UserToken);

    bool IsVhdAttached(_In_ PCWSTR VhdPath);

    DiskMountResult MountDisk(
        _In_ PCWSTR Disk, _In_ DiskType MountDiskType, _In_ ULONG PartitionIndex, _In_opt_ PCWSTR Name, _In_opt_ PCWSTR Type, _In_opt_ PCWSTR Options);

    enum MountFlags
    {
        None = 0x0,
        ReadOnly = 0x1
    };

    ULONG
    MountFileAsPersistentMemory(_In_ PCWSTR FilePath, _In_ bool ReadOnly);

    void MountRootNamespaceFolder(_In_ LPCWSTR HostPath, _In_ LPCWSTR GuestPath, _In_ bool ReadOnly, _In_ LPCWSTR Name);

    void RegisterCallbacks(_In_ const std::function<void(ULONG)>& DistroExitCallback = {}, _In_ const std::function<void(GUID)>& TerminationCallback = {});

    void ResizeDistribution(_In_ ULONG Lun, _In_ HANDLE OutputHandle, _In_ ULONG64 NewSize);

    void TrimDistribution(_In_ ULONG Lun);

    _Requires_lock_not_held_(m_lock)
    void SaveAttachedDisksState();

    _Requires_lock_held_(m_guestDeviceLock)
    void VerifyPlan9Servers();

    void TraceLoggingRundown() const noexcept;

    void ValidateNetworkingMode();

private:
    struct Mount
    {
        std::wstring Name;
        std::optional<std::wstring> Options;
        std::optional<std::wstring> Type;
    };

    struct DiskMountState
    {
        std::map<ULONG, Mount> Mounts;
    };

    WslCoreVm(_In_ wsl::core::Config&& VmConfig, _In_ InitializeDrvFsCallback InitializeDrvFs);

    _Requires_lock_held_(m_guestDeviceLock)
    void AddDrvFsShare(_In_ bool Admin, _In_ HANDLE UserToken);

    _Requires_lock_held_(m_guestDeviceLock)
    void AddPlan9Share(_In_ PCWSTR AccessName, _In_ PCWSTR Path, _In_ UINT32 Port, _In_ wsl::windows::common::hcs::Plan9ShareFlags Flags, _In_ HANDLE UserToken, _In_ PCWSTR VirtIoTag);

    _Requires_lock_held_(m_guestDeviceLock)
    std::tuple<std::wstring, std::wstring, std::wstring> AddVirtioFsShare(
        _In_ bool Admin, _In_ PCWSTR Path, _In_ PCWSTR Options, _In_opt_ HANDLE UserToken = nullptr);

    _Requires_lock_held_(m_lock)
    VmDiskAttachment AttachDiskLockHeld(
        _In_ PCWSTR Disk,
        _In_ DiskType Type,
        _In_ MountFlags Flags,
        _In_ std::optional<ULONG> Lun,
        _In_ bool IsUserDisk,
        _In_ HANDLE UserToken,
        _In_ bool BootCritical = false);

    void CollectCrashDumps(VmGuestListener Listener) const;

    std::shared_ptr<LxssRunningInstance> CreateInstanceInternal(
        _In_ const GUID& InstanceId,
        _In_ const LXSS_DISTRO_CONFIGURATION& Configuration,
        _In_ DWORD ReceiveTimeout = 0,
        _In_ ULONG DefaultUid = LX_UID_ROOT,
        _In_ ULONG64 ClientLifetimeId = 0,
        _In_ bool LaunchSystemDistro = false,
        _Out_opt_ ULONG* ConnectPort = nullptr);

    _Requires_lock_held_(m_lock)
    void EjectVhdLockHeld(_In_ PCWSTR VhdPath);

    _Requires_lock_held_(m_guestDeviceLock)
    std::optional<VmFileSystemShare> FindVirtioFsShare(_In_ PCWSTR Tag, _In_ std::optional<bool> Admin = {}) const;

    VmCreateRequest GenerateBackendRequest(const GUID& VmId, BackendKind Backend);

    static std::pair<int, LX_MINI_MOUNT_STEP> GetMountResult(_In_ wsl::shared::SocketChannel& Channel);

    void Initialize(const GUID& VmId, const wil::shared_handle& UserToken);

    void InitializeGuest();

    void ValidateBackendConfiguration(BackendKind Backend) const;

    _Requires_lock_held_(m_guestDeviceLock)
    bool InitializeDrvFsLockHeld(_In_ HANDLE UserToken);

    bool IsDnsTunnelingSupported() const;

    _Requires_lock_held_(m_lock)
    DiskMountResult MountDiskLockHeld(
        _In_ PCWSTR Disk, _In_ DiskType MountDiskType, _In_ ULONG PartitionIndex, _In_opt_ PCWSTR Name, _In_opt_ PCWSTR Type, _In_opt_ PCWSTR Options);

    void WaitForPmemDeviceInVm(_In_ ULONG PmemId);

    void OnExit(const VmTerminationInformation& Termination);

    void ReadGuestCapabilities();

    _Requires_lock_held_(m_lock)
    static void SaveDiskState(_In_ HKEY Key, _In_ const VmDiskAttachment& Disk, _In_ const DiskMountState& State);

    _Requires_lock_held_(m_lock)
    std::pair<int, LX_MINI_MOUNT_STEP> UnmountDisk(_In_ const VmDiskAttachment& Disk, _Inout_ DiskMountState& State);

    _Requires_lock_held_(m_lock)
    std::pair<int, LX_MINI_MOUNT_STEP> UnmountVolume(_In_ PCWSTR Name);

    void VirtioFsWorker(VmGuestListener Listener);

    std::vector<char> ProcessVirtioFsRequest(_In_ gsl::span<gsl::byte> Request);

    static std::string s_GetMountTargetName(_In_ PCWSTR Disk, _In_opt_ PCWSTR Name, _In_ int PartitionIndex);

    wil::srwlock m_guestDeviceLock;
    _Guarded_by_(m_guestDeviceLock) std::future<bool> m_drvfsInitialResult;
    _Guarded_by_(m_guestDeviceLock) wil::unique_handle m_drvfsToken;
    _Guarded_by_(m_guestDeviceLock) wil::unique_handle m_adminDrvfsToken;
    wil::srwlock m_lock;
    _Guarded_by_(m_lock) wil::com_ptr<IPlan9FileSystem> m_pluginPlan9Server;
    _Guarded_by_(m_lock) wil::unique_event m_terminatingEvent { wil::EventOptions::ManualReset };
    _Guarded_by_(m_lock) wil::unique_event m_vmExitEvent { wil::EventOptions::ManualReset };
    wil::unique_event m_vmCrashEvent{wil::EventOptions::ManualReset};

    wil::srwlock m_exitCallbackLock;
    _Guarded_by_(m_exitCallbackLock) std::optional<VmTerminationInformation> m_terminationInformation;
    std::wstring m_machineId;
    GUID m_runtimeId;
    wsl::core::Config m_vmConfig;
    InitializeDrvFsCallback m_initializeDrvFs;
    std::wstring m_comPipe0;
    std::wstring m_comPipe1;
    int m_pageReportingOrder;
    WslTraceLoggingClient m_traceClient;
    std::filesystem::path m_rootFsPath;
    std::filesystem::path m_tempPath;
    wil::shared_handle m_userToken;
    wil::unique_handle m_restrictedToken;
    bool m_swapFileCreated;
    bool m_localDevicesKeyCreated;
    bool m_tempDirectoryCreated;
    bool m_enableInboxGpuLibs;
    bool m_defaultKernel = true;
    bool m_privateKernelModules = false;
    LX_MINI_INIT_MOUNT_DEVICE_TYPE m_systemDistroDeviceType = LxMiniInitMountDeviceTypeInvalid;
    ULONG m_systemDistroDeviceId = ULONG_MAX;
    ULONG m_kernelModulesDeviceId = ULONG_MAX;
    std::unique_ptr<IVirtualMachineBackend> m_backend;
    VmGuestListener m_guestListener;
    std::optional<VmGuestListener> m_crashDumpListener;
    std::optional<VmGuestListener> m_virtioFsListener;
    std::function<void(GUID)> m_onExit;
    wsl::shared::SocketChannel m_miniInitChannel;
    wil::unique_socket m_notifyChannel;
    SE_SID m_userSid;
    std::shared_ptr<LxssRunningInstance> m_systemDistro;
    _Guarded_by_(m_lock) std::map<std::uint64_t, DiskMountState> m_diskMounts;
    std::tuple<std::uint32_t, std::uint32_t, std::uint32_t> m_kernelVersion;
    std::wstring m_kernelVersionString;
    bool m_seccompAvailable;
    uint64_t m_hvPciSwiotlbBase = 0;
    uint64_t m_hvPciSwiotlbSize = 0;
    std::wstring m_sharedMemoryRoot;
    std::filesystem::path m_installPath;
    std::wstring m_userProfile;
    wsl::windows::common::helpers::WindowsVersion m_windowsVersion;
    std::shared_ptr<DmesgCollector> m_dmesgCollector;
    std::shared_ptr<GuestTelemetryLogger> m_gnsTelemetryLogger;
    std::wstring m_debugShellPipe;
    std::thread m_distroExitThread;
    std::thread m_virtioFsThread;
    std::thread m_crashDumpCollectionThread;

    std::unique_ptr<wsl::core::INetworkingEngine> m_networkingEngine;

    // Job object that terminates child processes (wslhost.exe, wslrelay.exe)
    // when the VM shuts down.
    wil::unique_handle m_processJobObject;
};

DEFINE_ENUM_FLAG_OPERATORS(WslCoreVm::MountFlags);
