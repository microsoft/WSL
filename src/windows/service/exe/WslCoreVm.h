/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WslCoreVm.h

Abstract:

    This file contains WSL Core vm function declarations.

--*/

#pragma once

#include "hcs.hpp"
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

inline constexpr auto c_vmOwner = L"WSL";

static constexpr GUID c_virtiofsAdminClassId = {0x7e6ad219, 0xd1b3, 0x42d5, {0xb8, 0xee, 0xd9, 0x63, 0x24, 0xe6, 0x4f, 0xf6}};

// {60285AE6-AAF3-4456-B444-A6C2D0DEDA38}
static constexpr GUID c_virtiofsClassId = {0x60285ae6, 0xaaf3, 0x4456, {0xb4, 0x44, 0xa6, 0xc2, 0xd0, 0xde, 0xda, 0x38}};

namespace wrl = Microsoft::WRL;

/// <summary>
/// This object tracks a running WSL Core VM.
/// </summary>
class WslCoreVm
{
    WslCoreVm(const WslCoreVm&) = delete;
    void operator=(const WslCoreVm&) = delete;

public:
    static std::unique_ptr<WslCoreVm> Create(_In_ const wil::shared_handle& UserToken, _In_ wsl::core::Config&& VmConfig, _In_ const GUID& VmId);

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

    wil::unique_socket CreateListeningSocket() const;

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

    void MountSharedMemoryDevice(_In_ const GUID& ImplementationClsid, _In_ PCWSTR Tag, _In_ PCWSTR Path, _In_ UINT32 SizeMb);

    ULONG
    MountFileAsPersistentMemory(_In_ const GUID& ImplementationClsid, _In_ PCWSTR FilePath, _In_ bool ReadOnly);

    void MountRootNamespaceFolder(_In_ LPCWSTR HostPath, _In_ LPCWSTR GuestPath, _In_ bool ReadOnly, _In_ LPCWSTR Name);

    void RegisterCallbacks(_In_ const std::function<void(ULONG)>& DistroExitCallback = {}, _In_ const std::function<void(GUID)>& TerminationCallback = {});

    void ResizeDistribution(_In_ ULONG Lun, _In_ HANDLE OutputHandle, _In_ ULONG64 NewSize);

    _Requires_lock_not_held_(m_lock)
    void SaveAttachedDisksState();

    _Requires_lock_held_(m_guestDeviceLock)
    void VerifyDrvFsServers();

    enum DiskStateFlags
    {
        Online = 0x1,
        AccessGranted = 0x2
    };

    void TraceLoggingRundown() const noexcept;

    void ValidateNetworkingMode();

private:
    struct AttachedDisk
    {
        DiskType Type;
        std::wstring Path;
        bool User;

        bool operator<(const AttachedDisk& other) const;
        bool operator==(const AttachedDisk& other) const;
    };

    struct Mount
    {
        std::wstring Name;
        std::optional<std::wstring> Options;
        std::optional<std::wstring> Type;
    };

    struct DiskState
    {
        ULONG Lun;
        std::map<ULONG, Mount> Mounts;
        DiskStateFlags Flags;
    };

    struct DirectoryObjectLifetime
    {
        std::wstring Path;
        // Directory objects are temporary, even if they have children, so need to keep
        // any created handles open in order for the directory to remain accessible.
        std::vector<wil::unique_handle> HierarchyLifetimes;
    };

    struct VirtioFsShare
    {
        VirtioFsShare(PCWSTR Path, PCWSTR Options, bool Admin);

        std::wstring Path;
        // Mount options are stored as a map so mounts that specify mount options in different orders can be shared.
        std::map<std::wstring, std::wstring> Options;
        bool Admin;

        std::wstring OptionsString() const;
        bool operator<(const VirtioFsShare& other) const;
        bool operator==(const VirtioFsShare& other) const;
    };

    WslCoreVm(_In_ wsl::core::Config&& VmConfig);

    _Requires_lock_held_(m_guestDeviceLock)
    void AddDrvFsShare(_In_ bool Admin, _In_ HANDLE UserToken);

    _Requires_lock_held_(m_guestDeviceLock)
    void AddPlan9Share(_In_ PCWSTR AccessName, _In_ PCWSTR Path, _In_ UINT32 Port, _In_ wsl::windows::common::hcs::Plan9ShareFlags Flags, _In_ HANDLE UserToken, _In_ PCWSTR VirtIoTag);

    _Requires_lock_held_(m_guestDeviceLock)
    GUID AddHdvShare(_In_ const GUID& DeviceId, _In_ const GUID& ImplementationClsid, _In_ PCWSTR AccessName, _In_opt_ PCWSTR Path, _In_ UINT32 Flags, _In_ HANDLE UserToken);

    _Requires_lock_held_(m_guestDeviceLock)
    GUID AddHdvShareWithOptions(
        _In_ const GUID& DeviceId,
        _In_ const GUID& ImplementationClsid,
        _In_ std::wstring_view AccessName,
        _In_ std::wstring_view Options,
        _In_ std::wstring_view Path,
        _In_ UINT32 Flags,
        _In_ HANDLE UserToken);

    _Requires_lock_held_(m_guestDeviceLock)
    std::wstring AddVirtioFsShare(_In_ bool Admin, _In_ PCWSTR Path, _In_ PCWSTR Options, _In_opt_ HANDLE UserToken = nullptr);

    _Requires_lock_held_(m_lock)
    ULONG AttachDiskLockHeld(_In_ PCWSTR Disk, _In_ DiskType Type, _In_ MountFlags Flags, _In_ std::optional<ULONG> Lun, _In_ bool IsUserDisk, _In_ HANDLE UserToken);

    void CollectCrashDumps(wil::unique_socket&& socket) const;

    template <typename Interface>
    wil::com_ptr_t<Interface> CreateComServerAsUser(_In_ REFCLSID RefClsId, _In_ HANDLE UserToken)
    {
        auto revert = wil::impersonate_token(UserToken);
        return wil::CoCreateInstance<Interface>(RefClsId, (CLSCTX_LOCAL_SERVER | CLSCTX_ENABLE_CLOAKING | CLSCTX_ENABLE_AAA));
    }

    template <typename Class, typename Interface>
    wil::com_ptr_t<Interface> CreateComServerAsUser(_In_ HANDLE UserToken)
    {
        return CreateComServerAsUser<Interface>(__uuidof(Class), UserToken);
    }

    std::shared_ptr<LxssRunningInstance> CreateInstanceInternal(
        _In_ const GUID& InstanceId,
        _In_ const LXSS_DISTRO_CONFIGURATION& Configuration,
        _In_ DWORD ReceiveTimeout = 0,
        _In_ ULONG DefaultUid = LX_UID_ROOT,
        _In_ ULONG64 ClientLifetimeId = 0,
        _In_ bool LaunchSystemDistro = false,
        _Out_opt_ ULONG* ConnectPort = nullptr);

    DirectoryObjectLifetime CreateSectionObjectRoot(_In_ std::wstring_view RelativeRootPath, _In_ HANDLE UserToken) const;

    _Requires_lock_held_(m_lock)
    void EjectVhdLockHeld(_In_ PCWSTR VhdPath);

    _Requires_lock_held_(m_guestDeviceLock)
    std::optional<VirtioFsShare> FindVirtioFsShare(_In_ PCWSTR tag, _In_ std::optional<bool> Admin = {}) const;

    _Requires_lock_held_(m_lock)
    void FreeLun(_In_ ULONG Lun);

    std::wstring GenerateConfigJson();

    static std::pair<int, LX_MINI_MOUNT_STEP> GetMountResult(_In_ wsl::shared::SocketChannel& Channel);

    void GrantVmWorkerProcessAccessToDisk(_In_ PCWSTR Disk, _In_opt_ HANDLE UserToken) const;

    void Initialize(const GUID& VmId, const wil::shared_handle& UserToken);

    void InitializeGuest();

    _Requires_lock_held_(m_guestDeviceLock)
    bool InitializeDrvFsLockHeld(_In_ HANDLE UserToken);

    bool IsDnsTunnelingSupported() const;

    bool IsDisableVgpuSettingsSupported() const;

    bool IsVirtioSerialConsoleSupported() const;

    bool IsVmemmSuffixSupported() const;

    _Requires_lock_held_(m_lock)
    DiskMountResult MountDiskLockHeld(
        _In_ PCWSTR Disk, _In_ DiskType MountDiskType, _In_ ULONG PartitionIndex, _In_opt_ PCWSTR Name, _In_opt_ PCWSTR Type, _In_opt_ PCWSTR Options);

    _Requires_lock_held_(m_guestDeviceLock)
    void MountSharedMemoryDeviceLockHeld(_In_ const GUID& ImplementationClsid, _In_ PCWSTR Tag, _In_ PCWSTR Path, _In_ UINT32 SizeMb);

    void WaitForPmemDeviceInVm(_In_ ULONG PmemId);

    void OnCrash(_In_ LPCWSTR Details);

    void OnExit(_In_opt_ PCWSTR ExitDetails);

    void ReadGuestCapabilities();

    _Requires_lock_held_(m_lock)
    ULONG ReserveLun(_In_ std::optional<ULONG> Lun);

    void RestorePassthroughDiskState(_In_ LPCWSTR Disk) const;

    _Requires_lock_held_(m_lock)
    static void SaveDiskState(_In_ HKEY Key, _In_ const AttachedDisk& Disk, _In_ const DiskState& State, _In_ const DiskType& DiskType);

    _Requires_lock_held_(m_lock)
    std::pair<int, LX_MINI_MOUNT_STEP> UnmountDisk(_In_ const AttachedDisk& Disk, _Inout_ DiskState& State);

    _Requires_lock_held_(m_lock)
    std::pair<int, LX_MINI_MOUNT_STEP> UnmountVolume(_In_ const AttachedDisk& Disk, _In_ ULONG PartitionIndex, _In_ PCWSTR Name);

    void VirtioFsWorker(_In_ const wil::unique_socket& socket);

    static std::string s_GetMountTargetName(_In_ PCWSTR Disk, _In_opt_ PCWSTR Name, _In_ int PartitionIndex);

    static LX_INIT_DRVFS_MOUNT s_InitializeDrvFs(_Inout_ WslCoreVm* VmContext, _In_ HANDLE UserToken);

    static void CALLBACK s_OnExit(_In_ HCS_EVENT* Event, _In_opt_ void* Context);

    wil::srwlock m_lock;
    wil::srwlock m_guestDeviceLock;
    _Guarded_by_(m_guestDeviceLock) std::future<bool> m_drvfsInitialResult;
    _Guarded_by_(m_guestDeviceLock) wil::unique_handle m_drvfsToken;
    _Guarded_by_(m_guestDeviceLock) wil::unique_handle m_adminDrvfsToken;
    _Guarded_by_(m_guestDeviceLock) std::map<VirtioFsShare, std::wstring> m_virtioFsShares;
    _Guarded_by_(m_lock) wil::unique_event m_terminatingEvent { wil::EventOptions::ManualReset };
    _Guarded_by_(m_lock) wil::unique_event m_vmExitEvent { wil::EventOptions::ManualReset };
    wil::unique_event m_vmCrashEvent{wil::EventOptions::ManualReset};
    std::optional<std::filesystem::path> m_vmCrashLogFile;

    wil::srwlock m_exitCallbackLock;
    std::wstring m_exitDetails;
    std::wstring m_machineId;
    GUID m_runtimeId;
    wsl::core::Config m_vmConfig;
    std::wstring m_comPipe0;
    std::wstring m_comPipe1;
    int m_coldDiscardShiftSize;
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
    LX_MINI_INIT_MOUNT_DEVICE_TYPE m_systemDistroDeviceType = LxMiniInitMountDeviceTypeInvalid;
    ULONG m_systemDistroDeviceId = ULONG_MAX;
    wsl::windows::common::hcs::unique_hcs_system m_system;
    wil::unique_socket m_listenSocket;
    std::function<void(GUID)> m_onExit;
    wsl::shared::SocketChannel m_miniInitChannel;
    wil::unique_socket m_notifyChannel;
    SE_SID m_userSid;
    Microsoft::WRL::ComPtr<DeviceHostProxy> m_deviceHostSupport;
    std::shared_ptr<LxssRunningInstance> m_systemDistro;
    _Guarded_by_(m_lock) std::bitset<MAX_VHD_COUNT> m_lunBitmap;
    _Guarded_by_(m_lock) std::map<AttachedDisk, DiskState> m_attachedDisks;
    _Guarded_by_(m_guestDeviceLock) std::map<UINT32, wil::com_ptr<IPlan9FileSystem>> m_plan9Servers;
    _Guarded_by_(m_guestDeviceLock) std::vector<DirectoryObjectLifetime> m_objectDirectories;
    std::tuple<std::uint32_t, std::uint32_t, std::uint32_t> m_kernelVersion;
    std::wstring m_kernelVersionString;
    bool m_seccompAvailable;
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

    wil::srwlock m_persistentMemoryLock;
    _Guarded_by_(m_persistentMemoryLock) ULONG m_nextPersistentMemoryId = 0;

    std::unique_ptr<wsl::core::INetworkingEngine> m_networkingEngine;

    static const std::wstring c_defaultTag;
};

DEFINE_ENUM_FLAG_OPERATORS(WslCoreVm::DiskStateFlags);

DEFINE_ENUM_FLAG_OPERATORS(WslCoreVm::MountFlags);
