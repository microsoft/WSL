/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    LxssUserSession.h

Abstract:

    This file contains session function declarations.

--*/

#pragma once

#include "LxssPort.h"
#include "LxssMessagePort.h"
#include "LxssCreateProcess.h"
#include "filesystem.hpp"
#include "LxssHttpProxy.h"
#include "WslCoreVm.h"
#include "PluginManager.h"
#include "Lifetime.h"
#include "DistributionRegistration.h"

#define WSL_NEW_DISTRO_LXFS L"NewDistributionLxFs"
#define WSL_DISTRO_CONFIG_DEFAULT_UID L"DefaultUid"

#define LXSS_DELETE_DISTRO_FLAGS_ROOTFS 0x1
#define LXSS_DELETE_DISTRO_FLAGS_VHD 0x2
#define LXSS_DELETE_DISTRO_FLAGS_WSLG_SHORTCUTS 0x4
#define LXSS_DELETE_DISTRO_FLAGS_SHORTCUTS 0x8
#define LXSS_DELETE_DISTRO_FLAGS_UNMOUNT 0x10
#define LXSS_DELETE_DISTRO_FLAGS_ALL \
    (LXSS_DELETE_DISTRO_FLAGS_ROOTFS | LXSS_DELETE_DISTRO_FLAGS_VHD | LXSS_DELETE_DISTRO_FLAGS_WSLG_SHORTCUTS | \
     LXSS_DELETE_DISTRO_FLAGS_SHORTCUTS | LXSS_DELETE_DISTRO_FLAGS_UNMOUNT)

class ConsoleManager;
class LxssRunningInstance;
class WslCoreVm;
class LxssUserSessionImpl;

typedef struct _LXSS_RUN_ELF_CONTEXT
{
    wil::unique_event instanceTerminatedEvent;
    wil::unique_handle instanceHandle;
} LXSS_RUN_ELF_CONTEXT, *PLXSS_RUN_ELF_CONTEXT;

typedef struct _LXSS_VM_MODE_SETUP_CONTEXT
{
    wil::unique_socket tarSocket;
    wil::unique_socket errorSocket;
    std::shared_ptr<LxssRunningInstance> instance;
} LXSS_VM_MODE_SETUP_CONTEXT, *PLXSS_VM_MODE_SETUP_CONTEXT;

enum class ShutdownBehavior
{
    Wait,
    Force,
    ForceAfter30Seconds
};

/// <summary>
/// Each COM client gets a unique LxssUserSession object which contains a std::weak_ptr to a LxssUserSessionImpl for that user.
/// </summary>

class DECLSPEC_UUID("a9b7a1b9-0671-405c-95f1-e0612cb4ce7e") LxssUserSession
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, ILxssUserSession, IWslSupport, IFastRundown>
{
public:
    LxssUserSession(_In_ const std::weak_ptr<LxssUserSessionImpl>& Session);
    LxssUserSession(const LxssUserSession&) = delete;
    LxssUserSession& operator=(const LxssUserSession&) = delete;

    /// <summary>
    /// Configures a distribution.
    /// </summary>
    IFACEMETHOD(ConfigureDistribution)(_In_opt_ LPCGUID DistroGuid, _In_ ULONG DefaultUid, _In_ ULONG Flags, _Out_ LXSS_ERROR_INFO* Error) override;

    /// <summary>
    /// Creates an instance of the specified distro. The method blocks until the
    /// new instance is started.
    /// </summary>
    IFACEMETHOD(CreateInstance)(_In_opt_ LPCGUID DistroGuid, _In_ ULONG Flags, _Out_ LXSS_ERROR_INFO* Error) override;

    /// <summary>
    /// Create a Linux process.
    /// </summary>
    IFACEMETHOD(CreateLxProcess)(
        _In_opt_ LPCGUID DistroGuid,
        _In_opt_ LPCSTR Filename,
        _In_ ULONG CommandLineCount,
        _In_reads_opt_(CommandLineCount) LPCSTR* CommandLine,
        _In_opt_ LPCWSTR CurrentWorkingDirectory,
        _In_opt_ LPCWSTR NtPath,
        _In_reads_opt_(NtEnvironmentLength) PWCHAR NtEnvironment,
        _In_ ULONG NtEnvironmentLength,
        _In_opt_ LPCWSTR Username,
        _In_ SHORT Columns,
        _In_ SHORT Rows,
        _In_ ULONG ConsoleHandle,
        _In_ PLXSS_STD_HANDLES StdHandles,
        _In_ ULONG Flags,
        _Out_ GUID* DistributionId,
        _Out_ GUID* InstanceId,
        _Out_ HANDLE* ProcessHandle,
        _Out_ HANDLE* ServerHandle,
        _Out_ HANDLE* StandardIn,
        _Out_ HANDLE* StandardOut,
        _Out_ HANDLE* StandardErr,
        _Out_ HANDLE* CommunicationChannel,
        _Out_ HANDLE* InteropSocket,
        _Out_ LXSS_ERROR_INFO* Error) override;

    /// <summary>
    /// Enumerates all registered distributions.
    /// </summary>
    IFACEMETHOD(EnumerateDistributions)(_Out_ PULONG DistributionCount, _Out_ LXSS_ENUMERATE_INFO** Distributions, _Out_ LXSS_ERROR_INFO* Error) override;

    /// <summary>
    /// Exports a distribution from to tar file.
    /// </summary>
    IFACEMETHOD(ExportDistribution)(
        _In_opt_ LPCGUID DistroGuid, _In_ HANDLE FileHandle, _In_ HANDLE ErrorHandle, _In_ ULONG Flags, _Out_ LXSS_ERROR_INFO* Error) override;

    /// <summary>
    /// Exports a distribution to a pipe.
    /// </summary>
    IFACEMETHOD(ExportDistributionPipe)(
        _In_opt_ LPCGUID DistroGuid, _In_ HANDLE PipeHandle, _In_ HANDLE ErrorHandle, _In_ ULONG Flags, _Out_ LXSS_ERROR_INFO* Error) override;

    /// <summary>
    /// Queries the default distribution.
    /// </summary>
    IFACEMETHOD(GetDefaultDistribution)(_Out_ LXSS_ERROR_INFO* Error, _Out_ LPGUID DefaultDistribution) override;

    /// <summary>
    /// Returns the configuration for the specified distribution.
    /// </summary>
    IFACEMETHOD(GetDistributionConfiguration)(
        _In_opt_ LPCGUID DistroGuid,
        _Out_ LPWSTR* DistributionName,
        _Out_ ULONG* Version,
        _Out_ ULONG* DefaultUid,
        _Out_ ULONG* DefaultEnvironmentCount,
        _Out_ LPSTR** DefaultEnvironment,
        _Out_ ULONG* Flags,
        _Out_ LXSS_ERROR_INFO* Error) override;

    /// <summary>
    /// Returns the GUID of a distribution with the specified name.
    /// </summary>
    IFACEMETHOD(GetDistributionId)(_In_ LPCWSTR DistributionName, _In_ ULONG Flags, _Out_ LXSS_ERROR_INFO* Error, _Out_ GUID* pDistroGuid) override;

    /// <summary>
    /// Registers a distribution from a tar file.
    /// </summary>
    IFACEMETHOD(RegisterDistribution)(
        _In_ LPCWSTR DistributionName,
        _In_ ULONG Version,
        _In_ HANDLE FileHandle,
        _In_ HANDLE ErrorHandle,
        _In_ LPCWSTR TargetDirectory,
        _In_ ULONG Flags,
        _In_ ULONG64 VhdSize,
        _In_opt_ LPCWSTR PackageFamilyName,
        _Out_ LPWSTR* InstalledDistributionName,
        _Out_ LXSS_ERROR_INFO* Error,
        _Out_ GUID* pDistroGuid) override;

    /// <summary>
    /// Registers a distribution from a pipe.
    /// </summary>
    IFACEMETHOD(RegisterDistributionPipe)(
        _In_ LPCWSTR DistributionName,
        _In_ ULONG Version,
        _In_ HANDLE PipeHandle,
        _In_ HANDLE ErrorHandle,
        _In_ LPCWSTR TargetDirectory,
        _In_ ULONG Flags,
        _In_ ULONG64 VhdSize,
        _In_opt_ LPCWSTR PackageFamilyName,
        _Out_ LPWSTR* InstalledDistributionName,
        _Out_ LXSS_ERROR_INFO* Error,
        _Out_ GUID* pDistroGuid) override;

    /// <summary>
    /// Resizes the virtual disk of a distribution.
    /// </summary>
    IFACEMETHOD(ResizeDistribution)(_In_ LPCGUID DistroGuid, _In_ HANDLE OutputHandle, _In_ ULONG64 NewSize, _Out_ LXSS_ERROR_INFO* Error) override;

    /// <summary>
    /// Sets the default distribution.
    /// </summary>
    IFACEMETHOD(SetDefaultDistribution)(_In_ LPCGUID DistroGuid, _Out_ LXSS_ERROR_INFO* Error) override;

    /// <summary>
    /// Sets or unsets the sparse flag for a distribution.
    /// </summary>
    IFACEMETHOD(SetSparse)(_In_ LPCGUID DistroGuid, _In_ BOOLEAN Sparse, _In_ BOOLEAN AllowUnsafe, _Out_ LXSS_ERROR_INFO* Error) override;

    /// <summary>
    /// Sets the version for a distribution.
    /// </summary>
    IFACEMETHOD(SetVersion)(_In_ LPCGUID DistroGuid, _In_ ULONG Version, _In_ HANDLE StdErrHandle, _Out_ LXSS_ERROR_INFO* Error) override;

    /// <summary>
    /// Pass through a disk to the utility VM.
    /// </summary>
    IFACEMETHOD(AttachDisk)(_In_ LPCWSTR Disk, _In_ ULONG Flags, _Out_ LXSS_ERROR_INFO* Error) override;

    /// <summary>
    /// Detach a passthrough disk from the utility VM.
    /// </summary>
    IFACEMETHOD(DetachDisk)(_In_ LPCWSTR Disk, _Out_ int* Result, _Out_ int* Step, _Out_ LXSS_ERROR_INFO* Error) override;

    /// <summary>
    /// Mount a disk.
    /// </summary>
    IFACEMETHOD(MountDisk)(
        _In_ LPCWSTR Disk,
        _In_ ULONG Flags,
        _In_ ULONG PartitionIndex,
        _In_opt_ LPCWSTR Name,
        _In_opt_ LPCWSTR Type,
        _In_opt_ LPCWSTR Options,
        _Out_ int* Result,
        _Out_ int* Step,
        _Out_ LPWSTR* MountName,
        _Out_ LXSS_ERROR_INFO* Error) override;

    /// <summary>
    /// Move a distribution to a new location
    /// </summary>
    IFACEMETHOD(MoveDistribution)(_In_ LPCGUID DistroGuid, _In_ LPCWSTR Location, _Out_ LXSS_ERROR_INFO* Error) override;

    /// <summary>
    /// Terminates all running instances and the Linux utility vm.
    /// </summary>
    IFACEMETHOD(Shutdown)() override;
    IFACEMETHOD(Shutdown)(_In_ BOOL Force);

    /// <summary>
    /// Imports a distribution inplace.
    /// </summary>
    IFACEMETHOD(ImportDistributionInplace)(_In_ LPCWSTR DistributionName, _In_ LPCWSTR VhdPath, _Out_ LXSS_ERROR_INFO* Error, _Out_ GUID* pDistroGuid) override;

    /// <summary>
    /// Terminates a distribution by it's client identifier.
    /// </summary>
    void TerminateByClientId(_In_ ULONG ClientId);

    /// <summary>
    /// Sets the execution state of this instance.
    /// </summary>
    IFACEMETHOD(TerminateDistribution)(_In_opt_ LPCGUID DistroGuid, _Out_ LXSS_ERROR_INFO* Error) override;

    /// <summary>
    /// Unregisters a distribution.
    /// </summary>
    IFACEMETHOD(UnregisterDistribution)(_In_ LPCGUID DistroGuid, _Out_ LXSS_ERROR_INFO* Error) override;

    // IWslSupport methods.

    /// <summary>
    /// Registers a distribution.
    /// </summary>
    HRESULT RegisterDistribution(
        _Inout_ LPCWSTR DistributionName, _In_ ULONG Version, _In_opt_ HANDLE TarGzFile, _In_opt_ HANDLE TarGzPipe, _In_ LPCWSTR TargetDirectory) override;

    /// <summary>
    /// Unregisters a distribution.
    /// </summary>
    HRESULT
    UnregisterDistribution(_In_ LPCWSTR DistributionName) override;

    /// <summary>
    /// Returns the configuration for the specified distribution.
    /// </summary>
    HRESULT GetDistributionConfiguration(
        _In_ LPCWSTR DistributionName,
        _Out_ ULONG* Version,
        _Out_ ULONG* DefaultUid,
        _Out_ ULONG* DefaultEnvironmentCount,
        _Out_ LPSTR** DefaultEnvironment,
        _Out_ ULONG* WslFlags) override;

    /// <summary>
    /// Configures a distribution.
    /// </summary>
    HRESULT SetDistributionConfiguration(_In_ LPCWSTR DistributionName, _In_ ULONG DefaultUid, _In_ ULONG WslFlags) override;

    /// <summary>
    /// Returns a list of runnable distributions.
    /// </summary>
    HRESULT ListDistributions(_Out_ ULONG* Count, _Out_ LPWSTR** Distributions) override;

    /// <summary>
    /// Creates an instance of the specified distro.
    /// </summary>
    HRESULT CreateInstance(_In_ LPCWSTR DistributionName, _In_ ULONG Flags) override;

private:
    std::weak_ptr<LxssUserSessionImpl> m_session;
};

/// <summary>
/// Each user gets its own LxssUserSessionImpl object, This object manages the lifetime of running instances.
/// </summary>
class LxssUserSessionImpl
{
public:
    LxssUserSessionImpl(_In_ PSID userSid, _In_ DWORD sessionId, _Inout_ wsl::windows::service::PluginManager& pluginManager);
    virtual ~LxssUserSessionImpl();
    LxssUserSessionImpl(const LxssUserSessionImpl&) = delete;
    LxssUserSessionImpl& operator=(const LxssUserSessionImpl&) = delete;

    /// <summary>
    /// Configures a distribution.
    /// </summary>
    HRESULT
    ConfigureDistribution(_In_opt_ LPCGUID DistroGuid, _In_ ULONG DefaultUid, _In_ ULONG Flags);

    /// <summary>
    /// Creates an instance of the specified distro. The method blocks until the
    /// new instance is started.
    /// </summary>
    HRESULT
    CreateInstance(_In_opt_ LPCGUID DistroGuid, _In_ ULONG Flags);

    /// <summary>
    /// Create a Linux process.
    /// </summary>
    HRESULT
    CreateLxProcess(
        _In_opt_ LPCGUID DistroGuid,
        _In_opt_ LPCSTR Filename,
        _In_ ULONG CommandLineCount,
        _In_reads_opt_(CommandLineCount) LPCSTR* CommandLine,
        _In_opt_ LPCWSTR CurrentWorkingDirectory,
        _In_opt_ LPCWSTR NtPath,
        _In_reads_opt_(NtEnvironmentLength) PWCHAR NtEnvironment,
        _In_ ULONG NtEnvironmentLength,
        _In_opt_ LPCWSTR Username,
        _In_ SHORT Columns,
        _In_ SHORT Rows,
        _In_ HANDLE ConsoleHandle,
        _In_ PLXSS_STD_HANDLES StdHandles,
        _In_ ULONG Flags,
        _Out_ GUID* DistributionId,
        _Out_ GUID* InstanceId,
        _Out_ HANDLE* ProcessHandle,
        _Out_ HANDLE* ServerHandle,
        _Out_ HANDLE* StandardIn,
        _Out_ HANDLE* StandardOut,
        _Out_ HANDLE* StandardErr,
        _Out_ HANDLE* CommunicationChannel,
        _Out_ HANDLE* InteropSocket);

    /// <summary>
    /// Clears the state of an attached disk in the registry
    /// </summary>
    void ClearDiskStateInRegistry(_In_opt_ LPCWSTR Disk);

    /// <summary>
    /// Start a process in the root namespace or in a user distribution.
    /// </summary>
    HRESULT CreateLinuxProcess(_In_opt_ const GUID* Distro, _In_ LPCSTR Path, _In_ LPCSTR* Arguments, _Out_ SOCKET* socket);

    /// <summary>
    /// Enumerates registered distributions, optionally including ones that are
    /// currently being registered, unregistered, or converted.
    /// </summary>
    HRESULT
    EnumerateDistributions(_Out_ PULONG DistributionCount, _Out_ LXSS_ENUMERATE_INFO** Distributions);

    /// <summary>
    /// Exports a distribution.
    /// </summary>
    HRESULT
    ExportDistribution(_In_opt_ LPCGUID DistroGuid, _In_ HANDLE FileHandle, _In_ HANDLE ErrorHandle, _In_ ULONG Flags);

    /// <summary>
    /// Queries the default distribution.
    /// </summary>
    HRESULT
    GetDefaultDistribution(_Out_ LPGUID DefaultDistribution);

    /// <summary>
    /// Returns the configuration for the specified distribution.
    /// </summary>
    HRESULT
    GetDistributionConfiguration(
        _In_opt_ LPCGUID DistroGuid,
        _Out_ LPWSTR* DistributionName,
        _Out_ ULONG* Version,
        _Out_ ULONG* DefaultUid,
        _Out_ ULONG* DefaultEnvironmentCount,
        _Out_ LPSTR** DefaultEnvironment,
        _Out_ ULONG* Flags);

    /// <summary>
    /// Returns the GUID of a distribution with the specified name.
    /// </summary>
    HRESULT
    GetDistributionId(_In_ LPCWSTR DistributionName, _In_ ULONG Flags, _Out_ GUID* pDistroGuid);

    /// <summary>
    /// Returns the session cookie
    /// </summary>
    DWORD GetSessionCookie() const;

    /// <summary>
    /// Returns the session ID of the user.
    /// </summary>
    DWORD GetSessionId() const;

    /// <summary>
    /// Returns the sid for the user session.
    /// </summary>
    PSID GetUserSid();

    /// <summary>
    /// Imports a distribution inplace.
    /// </summary>
    HRESULT
    ImportDistributionInplace(_In_ LPCWSTR DistributionName, _In_ LPCWSTR VhdPath, _Out_ GUID* pDistroGuid);

    /// <summary>
    /// Mount a disk.
    /// </summary>
    HRESULT MountDisk(
        _In_ LPCWSTR Disk,
        _In_ ULONG Flags,
        _In_ ULONG PartitionIndex,
        _In_opt_ LPCWSTR Name,
        _In_opt_ LPCWSTR Type,
        _In_opt_ LPCWSTR Options,
        _Out_ int* Result,
        _Out_ int* Step,
        _Out_ LPWSTR* MountName);

    HRESULT MoveDistribution(_In_ LPCGUID DistroGuid, _In_ LPCWSTR Location);

    HRESULT MountRootNamespaceFolder(_In_ LPCWSTR HostPath, _In_ LPCWSTR GuestPath, _In_ bool ReadOnly, _In_ LPCWSTR Name);

    /// <summary>
    /// Registers a distribution.
    /// </summary>
    HRESULT
    RegisterDistribution(
        _In_ LPCWSTR DistributionName,
        _In_ ULONG Version,
        _In_ HANDLE FileHandle,
        _In_ HANDLE ErrorHandle,
        _In_ LPCWSTR TargetDirectory,
        _In_ ULONG Flags,
        _In_ ULONG64 VhdSize,
        _In_opt_ LPCWSTR PackageFamilyName,
        _Out_opt_ LPWSTR* InstalledDistributionName,
        _Out_ GUID* pDistroGuid);

    /// <summary>
    /// Resizes the disk of a distribution.
    /// </summary>
    HRESULT
    ResizeDistribution(_In_ LPCGUID DistroGuid, _In_ HANDLE OutputHandle, _In_ ULONG64 NewSize);

    /// <summary>
    /// Sets the default distribution.
    /// </summary>
    HRESULT
    SetDefaultDistribution(_In_ LPCGUID DistroGuid);

    /// <summary>
    /// Marks/unmarks the backing vhdx as sparse.
    /// </summary>
    HRESULT
    SetSparse(_In_ LPCGUID DistroGuid, _In_ BOOLEAN Sparse, _In_ BOOLEAN AllowUnsafe);

    /// <summary>
    /// Sets the version for a distribution.
    /// </summary>
    HRESULT
    SetVersion(_In_ LPCGUID DistroGuid, _In_ ULONG Version, _In_ HANDLE StdErrHandle);

    /// <summary>
    /// Pass through a disk to the utility VM.
    /// </summary>
    HRESULT AttachDisk(_In_ LPCWSTR Disk, _In_ ULONG Flags);

    /// <summary>
    /// Detach a passthrough disk from the utility VM.
    /// </summary>
    HRESULT DetachDisk(_In_ LPCWSTR Disk, _Out_ int* Result, _Out_ int* Step);

    /// <summary>
    /// Terminates all running instances and the Linux utility vm.
    /// </summary>
    HRESULT Shutdown(_In_ bool PreventNewInstances = false, ShutdownBehavior Behavior = ShutdownBehavior::Wait);

    /// <summary>
    /// Worker thread for logging telemetry about processes running inside of WSL.
    /// </summary>
    void TelemetryWorker(_In_ wil::unique_socket&& socket) const;

    /// <summary>
    /// Terminates a distribution by it's client identifier.
    /// </summary>
    void TerminateByClientId(_In_ ULONG ClientId);

    /// <summary>
    /// Terminates a distribution by it's client identifier (assumes lock is held).
    /// </summary>
    void TerminateByClientIdLockHeld(_In_ ULONG ClientId);

    /// <summary>
    /// Sets the execution state of this instance.
    /// </summary>
    HRESULT
    TerminateDistribution(_In_opt_ LPCGUID DistroGuid);

    /// <summary>
    /// Unregisters a distribution.
    /// </summary>
    HRESULT
    UnregisterDistribution(_In_ LPCGUID DistroGuid);

    /// <summary>
    /// Queries a distribution's default UID, default environment, and flags.
    /// </summary>
    static CreateLxProcessContext s_GetCreateProcessContext(_In_ const GUID& DistroGuid, _In_ bool SystemDistro);

private:
    /// <summary>
    /// Adds a distro to the list of converting distros.
    /// </summary>
    _Requires_lock_held_(m_instanceLock)
    void _ConversionBegin(_In_ GUID DistroGuid, _In_ LxssDistributionState State);

    /// <summary>
    /// Removes a distro from the list of converting distros and checks if the
    /// Linux utility VM is idle.
    /// </summary>
    _Requires_lock_not_held_(m_instanceLock)
    void _ConversionComplete(_In_ GUID DistroGuid);

    /// <summary>
    /// Creates a distribution registration for legacy installs.
    /// </summary>
    _Requires_exclusive_lock_held_(m_instanceLock)
    void _CreateLegacyRegistration(_In_ HKEY LxssKey, _In_ HANDLE UserToken);

    /// <summary>
    /// Creates the set of WSL mounts required for setup and ext4 conversion.
    /// </summary>
    static std::vector<wsl::windows::common::filesystem::unique_lxss_addmount> _CreateSetupMounts(_In_ const LXSS_DISTRO_CONFIGURATION& Configuration);

    /// <summary>
    /// Creates and initializes a utility VM. If a VM is already running, the
    /// running VM is returned.
    /// </summary>
    _Requires_lock_not_held_(m_instanceLock)
    std::shared_ptr<LxssRunningInstance> _CreateInstance(_In_opt_ LPCGUID DistroGuid, _In_ ULONG Flags = LXSS_CREATE_INSTANCE_FLAGS_ALLOW_FS_UPGRADE);

    static void _CreateDistributionShortcut(
        _In_ LPCWSTR DistributionName, LPCWSTR Shortcut, LPCWSTR ExecutablePath, wsl::windows::service::DistributionRegistration& registration);

    static void _CreateTerminalProfile(
        _In_ const std::string_view& Template,
        _In_ const std::filesystem::path& IconPath,
        _In_ const LXSS_DISTRO_CONFIGURATION& Configuration,
        wsl::windows::service::DistributionRegistration& Registration);

    /// <summary>
    /// Ensures that the utility VM has been created.
    /// </summary>
    _Requires_exclusive_lock_held_(m_instanceLock)
    void _CreateVm();

    /// <summary>
    /// Deletes distribution filesystem.
    /// </summary>
    void _DeleteDistribution(_In_ const LXSS_DISTRO_CONFIGURATION& Configuration, _In_ ULONG Flags = LXSS_DELETE_DISTRO_FLAGS_ALL);

    /// <summary>
    /// Deletes distribution filesystem.
    /// </summary>
    _Requires_exclusive_lock_held_(m_instanceLock)
    void _DeleteDistributionLockHeld(_In_ const LXSS_DISTRO_CONFIGURATION& Configuration, _In_ ULONG Flags = LXSS_DELETE_DISTRO_FLAGS_ALL) const;

    /// <summary>
    /// Enumerates and validates all registered distributions for the calling
    /// process. If ListAll is true this will return all distributions including
    /// those that are in the progress of being installed or uninstalled.
    /// </summary>
    _Requires_exclusive_lock_held_(m_instanceLock)
    std::vector<wsl::windows::service::DistributionRegistration> _EnumerateDistributions(
        _In_ HKEY LxssKey, _In_ bool ListAll = false, _In_ const std::optional<GUID>& Exclude = {});

    /// <summary>
    /// Validates that the specified distribution is not currently performing
    /// a filesystem conversion.
    /// </summary>
    _Requires_lock_held_(m_instanceLock)
    void _EnsureNotLocked(_In_ LPCGUID DistroGuid, const std::source_location& location = std::source_location::current());

    /// <summary>
    /// Queries the GUID of the default distribution for the calling process.
    /// </summary>
    _Requires_exclusive_lock_held_(m_instanceLock)
    GUID _GetDefaultDistro(_In_ HKEY LxssKey);

    /// <summary>
    /// Waits for the elf binary to exit and returns the exit status.
    /// </summary>
    static LONG _GetElfExitStatus(_In_ const LXSS_RUN_ELF_CONTEXT& Context);

    /// <summary>
    /// Return a new config after policies have been applied.
    /// </summary>
    wsl::core::Config _GetResultantConfig(_In_ const HANDLE userToken);

    _Requires_exclusive_lock_held_(m_instanceLock)
    void _LoadDiskMount(_In_ HKEY Key, _In_ const std::wstring& LunStr) const;

    _Requires_exclusive_lock_held_(m_instanceLock)
    void _LoadDiskMounts();

    _Requires_exclusive_lock_held_(m_instanceLock)
    void _LoadNetworkingSettings(_Inout_ wsl::core::Config& config, _In_ HANDLE userToken);

    void _ProcessImportResultMessage(
        const LX_MINI_INIT_IMPORT_RESULT& message,
        const gsl::span<gsl::byte> span,
        HKEY LxssKey,
        LXSS_DISTRO_CONFIGURATION& configuration,
        wsl::windows::service::DistributionRegistration& registration);

    /// <summary>
    /// Runs a single ELF binary without using the init daemon.
    /// </summary>
    static LXSS_RUN_ELF_CONTEXT _RunElfBinary(
        _In_ LPCSTR CommandLine,
        _In_ LPCWSTR TargetDirectory,
        _In_ HANDLE ClientProcess,
        _In_opt_ HANDLE StdIn = nullptr,
        _In_opt_ HANDLE StdOut = nullptr,
        _In_opt_ HANDLE StdErr = nullptr,
        _In_opt_count_(NumMounts) PLX_KMAPPATHS_ADDMOUNT Mounts = nullptr,
        _In_opt_ ULONG NumMounts = 0);

    /// <summary>
    /// Returns the currently running utility vm, if one exists.
    /// </summary>
    _Requires_lock_held_(m_instanceLock)
    std::shared_ptr<LxssRunningInstance> _RunningInstance(_In_ LPCGUID DistroGuid);

    /// <summary>
    /// Creates a utility VM to perform a setup operation.
    /// </summary>
    LXSS_VM_MODE_SETUP_CONTEXT
    _RunUtilityVmSetup(_In_ const LXSS_DISTRO_CONFIGURATION& Configuration, _In_ LX_MESSAGE_TYPE MessageType, _In_ ULONG ExportFlags = 0, _In_ bool SetVersion = false);

    void _SendDistributionRegisteredEvent(_In_ const LXSS_DISTRO_CONFIGURATION& Configuration) const;

    /// <summary>
    /// Set the specified distribution as installed. If there is no default distribution
    /// this routine also marks this distribution as the default.
    /// </summary>
    _Requires_lock_held_(m_instanceLock)
    static void _SetDistributionInstalled(_In_ HKEY LxssKey, _In_ const GUID& DistroGuid);

    /// <summary>
    /// Removes a utility vm from the list.
    /// </summary>
    _Requires_lock_not_held_(m_instanceLock)
    bool _TerminateInstance(_In_ LPCGUID DistroGuid, _In_ bool CheckForClients);

    _Requires_exclusive_lock_held_(m_instanceLock)
    bool _TerminateInstanceInternal(_In_ LPCGUID DistroGuid, _In_ bool CheckForClients = false);

    /// <summary>
    /// Ensures the WSL1 init binary is up-to-date.
    /// </summary>
    wil::srwlock m_initUpdateLock;
    _Guarded_by_(m_initUpdateLock) std::vector<GUID> m_updatedInitDistros;
    void _UpdateInit(_In_ const LXSS_DISTRO_CONFIGURATION& Configuration);

    /// <summary>
    /// Unregisters the specified distribution.
    /// </summary>
    _Requires_exclusive_lock_held_(m_instanceLock)
    void _UnregisterDistributionLockHeld(_In_ HKEY LxssKey, _In_ const LXSS_DISTRO_CONFIGURATION& Configuration);

    /// <summary>
    /// Updates timezone information for each running instance and utility VM.
    /// </summary>
    void _TimezoneUpdated();

    /// <summary>
    /// Validates if the package for a specified distribution is still installed.
    /// </summary>
    _Requires_lock_held_(m_instanceLock)
    static bool _ValidateDistro(_In_ HKEY LxssKey, _In_ LPCGUID DistroGuid);

    /// <summary>
    /// Validates that the given path or name is not already in use by a registered distribution.
    /// </summary>
    _Requires_lock_held_(m_instanceLock)
    void _ValidateDistributionNameAndPathNotInUse(
        _In_ HKEY LxssKey, _In_opt_ LPCWSTR Path, _In_opt_ LPCWSTR Name, const std::optional<GUID>& Exclude = {});

    /// <summary>
    /// Queues a threadpool timer to terminate an idle utility VM.
    /// </summary>
    _Requires_exclusive_lock_held_(m_instanceLock)
    void _VmCheckIdle();

    /// <summary>
    /// Terminate the Linux utility VM if there are no running distros.
    /// </summary>
    _Requires_lock_not_held_(m_instanceLock)
    void _VmIdleTerminate();

    /// <summary>
    /// Queries if the Linux utility VM has any running distros.
    /// </summary>
    _Requires_exclusive_lock_held_(m_instanceLock)
    bool _VmIsIdle();

    /// <summary>
    /// Terminates the Linux utility VM.
    /// </summary>
    _Requires_exclusive_lock_held_(m_instanceLock)
    void _VmTerminate();

    /// <summary>
    /// Configures HttpProxy info for the process
    /// </summary>
    void _SetHttpProxyInfo(_Inout_ std::vector<std::string>& environment) const noexcept;

    /// <summary>
    /// Launch OOBE for the first time or skip.
    /// </summary>
    void _LaunchOOBEIfNeeded() noexcept;

    /// <summary>
    /// Inputs proxy environment values into an environment if they're not already present.
    /// </summary>
    static void s_AddHttpProxyToEnvironment(_In_ const HttpProxySettings& proxySettings, _Inout_ std::vector<std::string>& environment) noexcept;

    /// <summary>
    /// Query information about a distribution config.
    /// </summary>
    static LXSS_DISTRO_CONFIGURATION s_GetDistributionConfiguration(const wsl::windows::service::DistributionRegistration& Distro, bool skipName = false);

    /// <summary>
    /// Impersonate the user and open the lxss registry key
    /// </summary>
    static wil::unique_hkey s_OpenLxssUserKey();

    /// <summary>
    /// Ensures the distribution name is valid.
    /// </summary>
    static void s_ValidateDistroName(_In_ LPCWSTR Name);

    /// <summary>
    /// Callback for when a VM unexpectedly exits (crashes).
    /// </summary>
    static void s_VmTerminated(_Inout_ LxssUserSessionImpl* UserSession, _In_ const GUID& VmId);

    /// <summary>
    /// Callback to terminate a utility VM.
    /// </summary>
    static bool s_TerminateInstance(_Inout_ LxssUserSessionImpl* UserSession, _In_ GUID DistroGuid, _In_ bool CheckForClients);

    /// <summary>
    /// Ensures that the init binary for the specified distribution is up-to-date.
    /// </summary>
    static void s_UpdateInit(_Inout_ LxssUserSessionImpl* UserSession, _In_ const LXSS_DISTRO_CONFIGURATION& Configuration);

    /// <summary>
    /// Callback to determine if the Linux VM can terminate.
    /// </summary>
    static VOID CALLBACK s_VmIdleTerminate(_Inout_ PTP_CALLBACK_INSTANCE, _Inout_opt_ PVOID Context, _Inout_ PTP_TIMER Timer);

    static LRESULT CALLBACK s_TimezoneWindowProc(HWND windowHandle, UINT messageCode, WPARAM wParameter, LPARAM lParameter);

    /// <summary>
    /// Lock for protecting various lists.
    /// </summary>
    std::recursive_timed_mutex m_instanceLock;

    /// <summary>
    /// Contains the currently running utility VM's.
    /// </summary>
    _Guarded_by_(m_instanceLock) std::map<GUID, std::shared_ptr<LxssRunningInstance>, wsl::windows::common::helpers::GuidLess> m_runningInstances;

    /// <summary>
    /// Contains a list of instances that have been terminated.
    /// </summary>
    wil::srwlock m_terminatedInstanceLock;
    _Guarded_by_(m_terminatedInstanceLock) std::list<std::shared_ptr<LxssRunningInstance>> m_terminatedInstances;

    /// <summary>
    /// Contains a list of distribution are toggling VM mode.
    /// </summary>
    _Guarded_by_(m_instanceLock) std::list<std::pair<GUID, LxssDistributionState>> m_lockedDistributions;

    /// <summary>
    /// The running utility vm for WSL2 distributions.
    ///
    _Guarded_by_(m_instanceLock) std::unique_ptr<WslCoreVm> m_utilityVm;

    std::atomic<GUID> m_vmId{GUID_NULL};

    /// <summary>
    /// Contains the user sid for the session.
    /// </summary>
    SE_SID m_userSid;

    /// <summary>
    /// Contains the session ID.
    /// </summary>
    DWORD m_sessionId;

    /// <summary>
    /// Class to keep track of any client processes.
    /// </summary>
    LifetimeManager m_lifetimeManager;

    /// <summary>
    /// Timer to control terminating the Linux utility VM.
    /// </summary>
    wil::unique_threadpool_timer m_vmTerminationTimer;

    /// <summary>
    /// Signaled when the utility vm is terminating.
    /// </summary>
    wil::unique_event m_vmTerminating{wil::EventOptions::ManualReset};

    /// <summary>
    /// Thread for logging usage telemetry from the WSL VM.
    /// </summary>
    std::thread m_telemetryThread;

    /// <summary>
    /// True when this session shouldn't allow creating new instances.
    /// (Used when the session is being deleted).
    /// </summary>
    bool m_disableNewInstanceCreation = false;

    /// <summary>
    /// The user's token. Shared with WslCoreVm and plugins.
    /// </summary>
    wil::shared_handle m_userToken;

    WSLSessionInformation m_session{};

    wsl::windows::service::PluginManager& m_pluginManager;

    /// <summary>
    /// Listens to host http proxy setting changes, tracks ongoing proxy queries, and stores current settings.
    /// Information used to configure Linux proxy environment variables.
    /// </summary>
    std::shared_ptr<HttpProxyStateTracker> m_httpProxyStateTracker{};

    std::thread m_timezoneThread;
};

#define LXSS_CLIENT_ID_WILDCARD ((ULONG)0)
#define LXSS_CLIENT_ID_INVALID ((ULONG) - 1)
