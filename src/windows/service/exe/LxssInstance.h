/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    LxssInstance.h

Abstract:

    This file contains lxss instance declarations.

--*/

#pragma once

#include "LxssIpTables.h"
#include "LxssConsoleManager.h"
#include "helpers.hpp"
#include "Lifetime.h"
#include "LxssCreateProcess.h"
#include "LxssServerPort.h"
#include "filesystem.hpp"
#include "WslCoreHostDnsInfo.h"

class LxssInstance;

/// <summary>
/// Represents an instance.  Instances may or may not be running. This object is
/// created via LxssUserSession::CreateInstance.
/// </summary>
class LxssInstance : public LxssRunningInstance
{
public:
    /// <summary>
    /// Sets up a new LxssInstance.
    /// </summary>
    LxssInstance(
        _In_ const GUID& InstanceId,
        _In_ const LXSS_DISTRO_CONFIGURATION& Configuration,
        _In_ ULONG DefaultUid,
        _In_ ULONG64 ClientLifetimeId,
        _In_ const std::function<void()>& TerminationCallback,
        _In_ const std::function<void()>& UpdateInitCallback,
        _In_ ULONG Flags,
        _In_ int IdleTimeout);

    virtual ~LxssInstance();

    void CreateLxProcess(
        _In_ const CreateLxProcessData& CreateProcessData,
        _In_ const CreateLxProcessContext& CreateProcessContext,
        _In_ const CreateLxProcessConsoleData& ConsoleData,
        _In_ SHORT Columns,
        _In_ SHORT Rows,
        _In_ PLXSS_STD_HANDLES StdHandles,
        _Out_ GUID* InstanceId,
        _Out_ HANDLE* ProcessHandle,
        _Out_ HANDLE* ServerHandle,
        _Out_ HANDLE* StandardIn,
        _Out_ HANDLE* StandardOut,
        _Out_ HANDLE* StandardErr,
        _Out_ HANDLE* CommunicationChannel,
        _Out_ HANDLE* InteropSocket) override;

    /// <returns>
    /// The instance's client identifier.
    /// </returns>
    ULONG GetClientId() const override;

    /// <returns>
    /// The distribution id.
    /// </returns>
    GUID GetDistributionId() const override;

    /// <returns>
    /// The message port to the init daemon.
    /// </returns>
    std::shared_ptr<LxssPort> GetInitPort() override;

    /// <returns>
    /// Informs the instance that the timezone has changed.
    /// </returns>
    void UpdateTimezone() override;

    /// <returns>
    /// The unique lifetime manager identifier for the instance.
    /// </returns>
    ULONG64 GetLifetimeManagerId() const override;

    /// <summary>
    /// This routine initializes an instance.
    /// </summary>
    void Initialize() override;

    /// <returns>
    /// Calls the termination callback that was registered when the instance starts.
    /// </returns>
    void OnTerminated() const;

    /// <summary>
    /// Requests for the instance to stop.
    /// </summary>
    bool RequestStop(_In_ bool Force) override;

    /// <summary>
    /// Stops the instance. Terminates the LXSS instance, and potentially
    /// deletes the directories created for runtime of the instance.
    /// This method can be called at any time (started or stopped) and will do the right cleanup.
    /// </summary>
    void Stop() override;

    /// <summary>
    /// Registers connection targets with the Plan 9 Redirector for the calling user, if they're
    /// not already registered.
    /// </summary>
    void RegisterPlan9ConnectionTarget(_In_ HANDLE userToken) override;

    /// <summary>
    /// Returns information about the distribution.
    /// </summary>
    const WSLDistributionInformation* DistributionInformation() const noexcept override;

protected:
    /// <summary>
    /// Configures the filesystem for this instance. Creates both "RootFS"
    /// and a "temp" directory that LXSS requires to function.
    /// </summary>
    void _ConfigureFilesystem(_In_ ULONG Flags);

    /// <summary>
    /// Creates the backing LXSS instance and starts it.
    /// </summary>
    void _StartInstance(_In_ ULONG DistributionFlags);

    /// <summary>
    /// Initializes mount points to be passed during instance creation.
    /// </summary>
    std::vector<wsl::windows::common::filesystem::unique_lxss_addmount> _InitializeMounts() const;

    /// <summary>
    /// Server port for listening and accepting of new connections.
    /// </summary>
    std::shared_ptr<LxssServerPort> m_ServerPort;

    /// <summary>
    /// Message port to the init process.
    /// </summary>
    std::shared_ptr<LxssMessagePort> m_InitMessagePort;

    /// <summary>
    /// Creates a process in the instance.
    /// </summary>
    wil::unique_handle _CreateLxProcess(
        _In_ const std::shared_ptr<LxssMessagePort>& MessagePort,
        _In_ const CreateLxProcessData& CreateProcessData,
        _In_ const std::vector<wil::unique_handle>& StdHandles,
        _In_ const wil::unique_handle& Token,
        _In_ ULONG DefaultUid,
        _Out_opt_ PHANDLE ServerPortHandle);

    /// <summary>
    /// Creates a create process message.
    /// </summary>
    std::vector<gsl::byte> _CreateLxProcessMarshalMessage(
        _In_ const std::shared_ptr<LxssMessagePort>& MessagePort,
        _In_ const CreateLxProcessData& CreateProcessData,
        _In_ const std::vector<wil::unique_handle>& StdHandles,
        _In_ const wil::unique_handle& Token,
        _In_ ULONG DefaultUid) const;

    /// <summary>
    /// Cleans up orphaned handles from a create process message.
    /// </summary>
    static void _ReleaseHandlesFromLxProcessMarshalMessage(_In_ const std::shared_ptr<LxssMessagePort>& MessagePort, _In_ PLX_INIT_CREATE_PROCESS MessageLocal);

    /// <summary>
    /// Initializes networking information and registers for WNF network state change notifications.
    /// </summary>
    void _InitializeNetworking();

    /// <summary>
    /// Updates networking information for the instance.
    /// </summary>
    void _UpdateNetworkInformation();

    /// <summary>
    /// Initializes communication channel to the init process.
    /// </summary>
    void _InitiateConnectionToInitProcess();

    /// <summary>
    /// Initializes configuration information for the instance. Updates host name
    /// information for the instance and supplies the list of DrvFs volumes to mount.
    /// </summary>
    void _InitializeConfiguration(_In_ const std::filesystem::path& Plan9SocketPath);

    /// <summary>
    /// Writes out configuration files used by a running Lx instance.
    /// </summary>
    void _UpdateNetworkConfigurationFiles(_In_ bool UpdateAlways);

private:
    /// <summary>
    /// Basic state of this object - instance identifier, instance handle,
    /// instance terminated event, and termination callback state.
    /// </summary>
    GUID m_instanceId;
    wil::unique_handle m_instanceHandle;
    wil::unique_event m_instanceTerminatedEvent;
    std::function<void()> m_terminationCallback;
    wil::unique_threadpool_wait m_terminationWait;
    wil::unique_handle m_userToken;

    /// <summary>
    /// Lock to protect instance state.
    /// </summary>
    std::mutex m_stateLock;
    _Guarded_by_(m_stateLock) bool m_initialized;
    _Guarded_by_(m_stateLock) bool m_running;

    /// <summary>
    /// Provides iptables emulation support.
    /// </summary>
    LxssIpTables m_ipTables;

    /// <summary>
    /// Class for querying host dns information.
    /// </summary>
    wsl::core::networking::HostDnsInfo m_dnsInfo;

    /// <summary>
    /// Settings for updating /etc/resolv.conf.
    /// </summary>
    bool m_enableVpnDetection;
    std::mutex m_resolvConfLock;
    _Guarded_by_(m_resolvConfLock) std::string m_lastResolvConfContents;

    /// <summary>
    /// Path and directory handles for this instance.  The base path is always available, the
    /// handles are only valid while the instance is running.
    /// </summary>
    std::filesystem::path m_tempPath;
    wil::unique_hfile m_tempDirectory;
    wil::unique_hfile m_rootDirectory;

    /// <summary>
    /// Specifies the immutable settings for the instance. These are read from
    /// the registry when the instance is created.
    /// </summary>
    ULONG m_defaultUid;
    LXSS_DISTRO_CONFIGURATION m_configuration;

    /// <summary>
    /// Specifies information about the distro.
    /// </summary>
    WSLDistributionInformation m_distributionInfo{};

    /// <summary>
    /// This job object contains all pico processes within the instance.
    /// </summary>
    wil::unique_handle m_instanceJob;

    /// <summary>
    /// Handle for network state change notifications.
    /// </summary>
    wsl::core::networking::unique_notify_handle m_networkNotificationHandle;

    /// <summary>
    /// Lifetime manager ID for registering NT client termination callbacks.
    /// </summary>
    ULONG64 m_ntClientLifetimeId;

    /// <summary>
    /// Class to keep track of client process and consoles.
    /// </summary>
    std::shared_ptr<ConsoleManager> m_consoleManager;

    /// <summary>
    /// Stores if the basic integrity level check is enabled.
    /// </summary>
    bool m_instanceBasicIntegrityLevelCheckEnabled;

    /// <summary>
    /// The basic integrity level of the caller that created the instance.
    /// </summary>
    DWORD m_instanceBasicIntegrityLevel;

    /// <summary>
    /// The authentication IDs used for the Plan 9 redirector connection target.
    /// </summary>
    wsl::windows::common::redirector::ConnectionTargetManager m_redirectorConnectionTargets;

    std::thread m_oobeThread;
    wil::unique_event m_destroyingEvent{wil::EventOptions::ManualReset};
    wil::unique_event m_oobeCompleteEvent;
};
