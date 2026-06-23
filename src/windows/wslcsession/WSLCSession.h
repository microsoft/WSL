/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCSession.h

Abstract:

    TODO

--*/

#pragma once

#include "wslc.h"
#include "WSLCCompat.h"
#include "WSLCVirtualMachine.h"
#include "WSLCContainer.h"
#include "WSLCIdleState.h"
#include "WSLCVolumes.h"
#include "WSLCNetworkMetadata.h"
#include "DockerEventTracker.h"
#include "DockerHTTPClient.h"
#include "IORelay.h"
#include <atomic>
#include <list>
#include <optional>
#include <unordered_map>

namespace wsl::windows::service::wslc {

class WSLCSession;

class UserHandle
{
    NON_COPYABLE(UserHandle);

public:
    UserHandle(WSLCSession& Session, HANDLE handle);
    UserHandle(UserHandle&& Other);

    ~UserHandle();

    UserHandle& operator=(UserHandle&& Other);

    HANDLE Get() const noexcept;
    void Reset();

private:
    WSLCSession* m_session{};
    HANDLE m_handle{};
};

class UserCOMCallback
{
    NON_COPYABLE(UserCOMCallback);

public:
    UserCOMCallback(WSLCSession& Session) noexcept;
    UserCOMCallback(UserCOMCallback&& Other) noexcept;

    ~UserCOMCallback() noexcept;

    UserCOMCallback& operator=(UserCOMCallback&& Other) noexcept;
    void Reset() noexcept;

private:
    WSLCSession* m_session{};
    DWORD m_threadId{};
};

//
// WSLCSession - Implements IWSLCSession for container management.
// Runs in a per-user COM server process for security isolation.
// The SYSTEM service passes an IWSLCVirtualMachineFactory to Initialize(); the VM is created
// lazily on first use and may be torn down when idle and recreated on demand.
//
class DECLSPEC_UUID("4877FEFC-4977-4929-A958-9F36AA1892A4") WSLCSession
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::WinRtClassicComMix>, IWSLCSession, IWSLCCompatSession, IFastRundown, ISupportErrorInfo>
{
    // WSLCContainer::Delete acquires a VmLease to keep the VM alive (and block idle
    // teardown) for the duration of a container deletion.
    friend class WSLCContainer;

public:
    WSLCSession() = default;

    ~WSLCSession();

    // Sets a callback invoked when this object is destroyed.
    // Used by the COM server host to signal process exit.
    void SetDestructionCallback(std::function<void()>&& callback);

    // Type of m_crashDumpCallbacks. Exposed so CrashDumpSubscription can hold an iterator into
    // it as an O(1) registration handle.
    using CrashDumpCallbackList = std::list<wil::com_ptr<ICrashDumpCallback>>;

    // IWSLCSession - initialization methods
    IFACEMETHOD(GetProcessHandle)(_Out_ HANDLE* ProcessHandle) override;
    IFACEMETHOD(Initialize)(
        _In_ const WSLCSessionInitSettings* Settings,
        _In_ IWSLCVirtualMachineFactory* VmFactory,
        _In_ IWSLCPluginNotifier* PluginNotifier,
        _In_opt_ IWarningCallback* WarningCallback) override;

    IFACEMETHOD(GetId)(_Out_ ULONG* Id) override;
    IFACEMETHOD(GetDisplayName)(_Out_ LPWSTR* DisplayName) override;
    IFACEMETHOD(GetState)(_Out_ WSLCSessionState* State) override;
    IFACEMETHOD(GetTerminationEvent)(_Out_ HANDLE* Event) override;
    IFACEMETHOD(GetTerminationReason)(_Out_ WSLCVirtualMachineTerminationReason* Reason, _Out_ LPWSTR* Details) override;

    // Image management.
    IFACEMETHOD(PullImage)(
        _In_ LPCSTR Image,
        _In_opt_ LPCSTR RegistryAuthenticationInformation,
        _In_opt_ IProgressCallback* ProgressCallback,
        _In_opt_ IWarningCallback* WarningCallback) override;
    IFACEMETHOD(BuildImage)(_In_ const WSLCBuildImageOptions* Options, _In_opt_ IProgressCallback* ProgressCallback, _In_opt_ HANDLE CancelEvent) override;
    IFACEMETHOD(LoadImage)(
        _In_ const WSLCHandle ImageHandle,
        _In_ ULONGLONG ContentLength,
        _In_opt_ IWarningCallback* WarningCallback,
        _In_opt_ IImageLoadCallback* LoadCallback) override;
    IFACEMETHOD(ImportImage)(
        _In_ const WSLCHandle ImageHandle,
        _In_opt_ LPCSTR ImageName,
        _In_ ULONGLONG ContentLength,
        _In_opt_ IWarningCallback* WarningCallback,
        _Out_ LPSTR* ImageId) override;
    IFACEMETHOD(SaveImage)(_In_ WSLCHandle OutputHandle, _In_ LPCSTR ImageNameOrID, _In_ IProgressCallback* ProgressCallback, _In_opt_ HANDLE CancelEvent) override;
    IFACEMETHOD(SaveImages)(_In_ WSLCHandle OutputHandle, _In_ const WSLCStringArray* ImageNames, _In_ IProgressCallback* ProgressCallback, _In_opt_ HANDLE CancelEvent) override;
    IFACEMETHOD(ListImages)(_In_opt_ const WSLCListImagesOptions* Options, _Out_ WSLCImageInformation** Images, _Out_ ULONG* Count) override;
    IFACEMETHOD(DeleteImage)(_In_ const WSLCDeleteImageOptions* Options, _Out_ WSLCDeletedImageInformation** DeletedImages, _Out_ ULONG* Count) override;
    IFACEMETHOD(TagImage)(_In_ const WSLCTagImageOptions* Options) override;
    IFACEMETHOD(PushImage)(
        _In_ LPCSTR Image,
        _In_ LPCSTR RegistryAuthenticationInformation,
        _In_opt_ IProgressCallback* ProgressCallback,
        _In_opt_ IWarningCallback* WarningCallback) override;
    IFACEMETHOD(InspectImage)(_In_ LPCSTR ImageNameOrId, _Out_ LPSTR* Output) override;
    IFACEMETHOD(Authenticate)(_In_ LPCSTR ServerAddress, _In_ LPCSTR Username, _In_ LPCSTR Password, _Out_ LPSTR* IdentityToken) override;
    IFACEMETHOD(PruneImages)(
        _In_opt_ const WSLCFilter* Filters,
        _In_ ULONG FiltersCount,
        _Out_ WSLCDeletedImageInformation** DeletedImages,
        _Out_ ULONG* DeletedImagesCount,
        _Out_ ULONGLONG* SpaceReclaimed) override;

    // Container management.
    IFACEMETHOD(CreateContainer)(_In_ const WSLCContainerOptions* Options, _In_opt_ IWarningCallback* WarningCallback, _Out_ IWSLCContainer** Container) override;
    IFACEMETHOD(OpenContainer)(_In_ LPCSTR Id, _In_ IWSLCContainer** Container) override;
    IFACEMETHOD(BeginContainerOperation)(_Outptr_ IUnknown** Operation) override;
    IFACEMETHOD(ListContainers)(
        _In_opt_ const WSLCListContainersOptions* Options,
        _Out_ WSLCContainerEntry** Containers,
        _Out_ ULONG* Count,
        _Out_ WSLCContainerPortMapping** Ports,
        _Out_ ULONG* PortsCount) override;
    IFACEMETHOD(PruneContainers)(_In_opt_ const WSLCFilter* Filters, _In_ ULONG FiltersCount, _Out_ WSLCPruneContainersResults* Result) override;

    // VM management.
    IFACEMETHOD(CreateRootNamespaceProcess)(
        _In_ LPCSTR Executable,
        _In_ const WSLCProcessOptions* Options,
        _In_ ULONG TtyRows,
        _In_ ULONG TtyColumns,
        _Out_ IWSLCProcess** VirtualMachine,
        _Out_ int* Errno) override;

    // Disk management.
    IFACEMETHOD(FormatVirtualDisk)(_In_ LPCWSTR Path) override;

    // Volume management.
    IFACEMETHOD(CreateVolume)(_In_ const WSLCVolumeOptions* Options, _Out_ WSLCVolumeInformation* VolumeInfo) override;
    IFACEMETHOD(DeleteVolume)(_In_ LPCSTR Name) override;
    IFACEMETHOD(ListVolumes)
    (_In_reads_opt_(FiltersCount) const WSLCFilter* Filters, _In_ ULONG FiltersCount, _Out_ WSLCVolumeInformation** Volumes, _Out_ ULONG* Count)
        override;
    IFACEMETHOD(InspectVolume)(_In_ LPCSTR Name, _Out_ LPSTR* Output) override;
    IFACEMETHOD(PruneVolumes)
    (_In_reads_opt_(FiltersCount) const WSLCFilter* Filters,
     _In_ ULONG FiltersCount,
     _In_opt_ IWarningCallback* WarningCallback,
     _Out_ WSLCVolumeName** Volumes,
     _Out_ ULONG* VolumesCount,
     _Out_ ULONGLONG* SpaceReclaimed) override;

    // Network management.
    IFACEMETHOD(CreateNetwork)(_In_ const WSLCNetworkOptions* Options, _In_opt_ IWarningCallback* WarningCallback) override;
    IFACEMETHOD(DeleteNetwork)(_In_ LPCSTR Name) override;
    IFACEMETHOD(ListNetworks)(_Out_ WSLCNetworkInformation** Networks, _Out_ ULONG* Count) override;
    IFACEMETHOD(InspectNetwork)(_In_ LPCSTR Name, _Out_ LPSTR* Output) override;
    IFACEMETHOD(PruneNetworks)
    (_In_reads_opt_(FiltersCount) const WSLCFilter* Filters, _In_ ULONG FiltersCount, _Out_ WSLCNetworkName** Networks, _Out_ ULONG* NetworksCount)
        override;

    IFACEMETHOD(Terminate()) override;

    IFACEMETHOD(RegisterCrashDumpCallback)(_In_ ICrashDumpCallback* Callback, _Out_ IUnknown** Subscription) override;

    // Called by CrashDumpSubscription when its last reference is released. The iterator must
    // have been returned by RegisterCrashDumpCallback against this session.
    void RemoveCrashDumpCallback(CrashDumpCallbackList::iterator It) noexcept;

    // ISupportErrorInfo
    IFACEMETHOD(InterfaceSupportsErrorInfo)(_In_ REFIID riid) override;

    // Testing.
    IFACEMETHOD(MountWindowsFolder)(_In_ LPCWSTR WindowsPath, _In_ LPCSTR LinuxPath, _In_ BOOL ReadOnly) override;
    IFACEMETHOD(UnmountWindowsFolder)(_In_ LPCSTR LinuxPath) override;
    IFACEMETHOD(MapVmPort)(_In_ int Family, _In_ unsigned short WindowsPort, _In_ unsigned short LinuxPort) override;
    IFACEMETHOD(UnmapVmPort)(_In_ int Family, _In_ unsigned short WindowsPort, _In_ unsigned short LinuxPort) override;

    // IWSLCCompatSession - converts the WSLCCompat types to the wslc.idl types and forwards to the methods above.
    // Methods that have an identical signature in both interfaces (Terminate, DeleteVolume, Authenticate,
    // GetTerminationReason) are served by the single existing override and require no additional code here.
    IFACEMETHOD(PullImage)(
        _In_ LPCSTR Image,
        _In_opt_ LPCSTR RegistryAuthenticationInformation,
        _In_opt_ IWSLCCompatProgressCallback* ProgressCallback,
        _In_opt_ IWSLCCompatWarningCallback* WarningCallback) override;
    IFACEMETHOD(LoadImage)(
        _In_ WSLCCompatHandle ImageHandle,
        _In_opt_ IWSLCCompatProgressCallback* ProgressCallback,
        _In_ ULONGLONG ContentLength,
        _In_opt_ IWSLCCompatWarningCallback* WarningCallback) override;
    IFACEMETHOD(ImportImage)(
        _In_ WSLCCompatHandle ImageHandle,
        _In_opt_ LPCSTR ImageName,
        _In_opt_ IWSLCCompatProgressCallback* ProgressCallback,
        _In_ ULONGLONG ContentLength,
        _In_opt_ IWSLCCompatWarningCallback* WarningCallback,
        _Out_ LPSTR* ImageId) override;
    IFACEMETHOD(ListImages)(_In_opt_ const WSLCCompatListImagesOptions* Options, _Out_ WSLCCompatImageInformation** Images, _Out_ ULONG* Count) override;
    IFACEMETHOD(DeleteImage)(_In_ const WSLCCompatDeleteImageOptions* Options, _Out_ WSLCCompatDeletedImageInformation** DeletedImages, _Out_ ULONG* Count) override;
    IFACEMETHOD(TagImage)(_In_ const WSLCCompatTagImageOptions* Options) override;
    IFACEMETHOD(PushImage)(
        _In_ LPCSTR Image,
        _In_ LPCSTR RegistryAuthenticationInformation,
        _In_opt_ IWSLCCompatProgressCallback* ProgressCallback,
        _In_opt_ IWSLCCompatWarningCallback* WarningCallback) override;
    IFACEMETHOD(CreateContainer)(
        _In_ const WSLCCompatContainerOptions* Options,
        _In_opt_ IWSLCCompatWarningCallback* WarningCallback,
        _Out_ IWSLCCompatContainer** Container) override;
    IFACEMETHOD(CreateVolume)(_In_ const WSLCCompatVolumeOptions* Options, _Out_ WSLCCompatVolumeInformation* VolumeInfo) override;
    IFACEMETHOD(RegisterCrashDumpCallback)(_In_ IWSLCCompatCrashDumpCallback* Callback, _Out_ IUnknown** Subscription) override;

    common::io::MultiHandleWait CreateIOContext(HANDLE CancelHandle = nullptr);

    UserHandle OpenUserHandle(WSLCHandle Handle);
    void ReleaseUserHandle(HANDLE Handle);

    UserCOMCallback RegisterUserCOMCallback();
    void UnregisterUserCOMCallback(DWORD ThreadId);

    // Returns the warning callback supplied when the session was created/entered, re-marshalled
    // into the calling apartment. Used as a fallback by WSLCExecutionContext so that warnings
    // emitted by operations that carry no explicit callback (e.g. resource recovery during the
    // lazy VM start) still reach the session creator. Returns null if no callback was supplied
    // or the creating client's proxy is no longer reachable.
    wil::com_ptr<IWarningCallback> AcquireWarningCallback() const;

    HANDLE SessionTerminatingEvent() const noexcept
    {
        return m_sessionTerminatingEvent.get();
    }

    ULONG Id() const noexcept
    {
        return m_id;
    }

    bool WaitForEventOrSessionTerminating(HANDLE Event, std::chrono::milliseconds Timeout) const;

    // Shared idle-termination state. Exposed so VM-scoped objects (e.g. running containers via
    // WSLCContainerImpl's ActivityRef) can hold an activity reference for their lifetime without
    // keeping the session object itself alive.
    std::shared_ptr<IdleState> IdleStateShared() const noexcept
    {
        return m_idleState;
    }

    // Creates an opaque activity token that holds a reference on this session's activity count for
    // its lifetime, deferring idle teardown of the VM until every outstanding token is released.
    // Used both for transient client operations (BeginContainerOperation) and to keep the VM alive
    // for the lifetime of a process whose wrapper a client may keep (root-namespace and exec'd
    // processes).
    Microsoft::WRL::ComPtr<IUnknown> CreateActivityToken();

private:
    ULONG m_id = 0;

    // VM lifecycle state for on-demand creation / idle termination.
    enum class VmState
    {
        None,
        Starting,
        Running,
        Stopping,
    };

    // Single-owner arbitration for a VM exit: exactly one side tears a given VM instance down,
    // resolved atomically. An expected stop (idle teardown or bring-up cleanup, on a lock-holding
    // thread) and a spontaneous exit (OnVmExited, lock-free on the relay thread) each try to claim
    // it; the loser declines. This avoids both a missed teardown and a deadlock (a lock-holder
    // joining the relay thread while OnVmExited spins for the lock in Terminate()). A fresh VM
    // starts Active. Claim via TryClaim*(), not by touching the atomic directly.
    enum class VmExitDisposition
    {
        Active,        // Running normally; a VM exit is unexpected and triggers a permanent Terminate().
        StopRequested, // An expected (soft) stop is in progress; OnVmExited treats the exit as expected.
        ExitClaimed,   // OnVmExited owns the permanent teardown of a spontaneous exit.
    };

    // Claims an expected (soft) stop of the current VM from a lock-holding thread. On success a
    // racing OnVmExited() declines, so the caller may tear down (joining the relay thread is safe).
    // On failure OnVmExited() already owns a spontaneous-exit teardown and is spinning for the lock
    // in Terminate(); the caller must not tear down or it deadlocks joining the relay.
    [[nodiscard]] bool TryClaimExpectedStop() noexcept;

    // Claims the teardown of a spontaneous VM exit from OnVmExited(). Fails if an expected stop is
    // already in progress, in which case the exit was wanted and the caller declines.
    [[nodiscard]] bool TryClaimSpontaneousExit() noexcept;

    _Requires_exclusive_lock_held_(m_lock)
    void StartVmLockHeld();
    _Requires_exclusive_lock_held_(m_lock)
    void StopVmLockHeld();
    _Requires_exclusive_lock_held_(m_lock)
    void TearDownVmLockHeld(bool CaptureTerminationReason = false);
    void EnsureVmRunning();

    // Idle-teardown callback invoked by IdleState's timer once the VM has been continuously idle
    // (activity count zero) for the grace period. Runs on a threadpool thread.
    void OnIdleTimer();
    bool IdleTerminationEnabled() const noexcept;
    void PersistSettings(const WSLCSessionInitSettings& Settings, PSID UserSid);

    // RAII lease taken at the top of every VM-requiring operation. On construction it
    // ensures the VM is running (lazily restarting it if it was idle-terminated) and records
    // an in-flight operation so idle teardown is deferred; it then holds the shared session
    // lock for the operation's duration. On destruction it releases the lock and triggers an
    // idle check.
    class VmLease
    {
    public:
        VmLease() = default;
        explicit VmLease(WSLCSession& Session);
        VmLease(VmLease&& Other) noexcept;
        VmLease& operator=(VmLease&& Other) noexcept;
        ~VmLease();

        VmLease(const VmLease&) = delete;
        VmLease& operator=(const VmLease&) = delete;

    private:
        WSLCSession* m_session{};
        wil::rwlock_release_shared_scope_exit m_lock;
    };

    [[nodiscard]] VmLease AcquireVmLease();

    __requires_lock_held(m_userHandlesLock) void CancelUserHandleIO();
    __requires_lock_held(m_userCOMCallbacksLock) void CancelUserCOMCallbacks();

    _Requires_shared_lock_held_(m_lock)
    void CreateContainerImpl(const WSLCContainerOptions* Options, IWSLCContainer** Container);

    void ConfigureStorage(const WSLCSessionInitSettings& Settings, PSID UserSid);
    void Ext4Format(const std::string& Device);
    _Requires_shared_lock_held_(m_lock)
    std::string InspectImageLockHeld(const std::string& Id);
    void OnContainerDeleted(const WSLCContainerImpl* Container);

    void OnCrashDumpWritten(const std::wstring& DumpPath, const std::string& ProcessName, ULONG Pid, ULONG Signal, ULONGLONG Timestamp);

    _Requires_shared_lock_held_(m_lock)
    void OnImageCreated(const std::string& ImageNameOrId) noexcept;

    _Requires_shared_lock_held_(m_lock)
    void OnImageDeleted(const std::string& ImageId) noexcept;

    void OnProcessLog(const gsl::span<char>& Data, PCSTR Source);
    void OnContainerdExited();
    void OnDockerdExited();
    void OnVmExited();
    ServiceRunningProcess StartProcess(
        const std::string& Executable, const std::vector<std::string>& Args, PCSTR LogSource, std::function<void()>&& ExitCallback);
    void InstallTrustedRootCertificates();
    void StartContainerd();
    void StartDockerd();
    int StopProcess(ServiceRunningProcess& Process, DWORD TerminateTimeoutMs, DWORD KillTimeoutMs);
    std::optional<std::string> ImportImageImpl(
        DockerHTTPClient::HTTPRequestContext& Request, const WSLCHandle ImageHandle, IImageLoadCallback* LoadCallback = nullptr);
    void RecoverExistingContainers();
    void RecoverExistingNetworks();

    void SaveImageImpl(std::pair<uint32_t, wil::unique_socket>& RequestCodePair, WSLCHandle OutputHandle, HANDLE CancelEvent);
    void StreamImageOperation(DockerHTTPClient::HTTPRequestContext& requestContext, LPCSTR Image, LPCSTR OperationName, IProgressCallback* ProgressCallback);

    std::optional<DockerHTTPClient> m_dockerClient;

    // The VM factory is a cross-process proxy supplied by the SYSTEM service at Initialize() time
    // but first used later (on demand) from a different thread/apartment. A directly stored proxy
    // would fail with RPC_E_WRONG_THREAD, so it is parked in the process Global Interface Table and
    // re-fetched (re-marshalled into the calling apartment) each time a VM is created.
    wil::com_ptr<IGlobalInterfaceTable> m_git;
    DWORD m_vmFactoryGitCookie{};

    // The warning callback supplied at Initialize() is parked in the GIT for the same reason as
    // the VM factory: it is used later, on demand, from other threads/apartments (a directly
    // stored proxy would fail with RPC_E_WRONG_THREAD). Zero if no callback was supplied.
    DWORD m_warningCallbackGitCookie{};

    std::optional<WSLCVirtualMachine> m_virtualMachine;
    std::optional<DockerEventTracker> m_eventTracker;
    wil::unique_event m_dockerdReadyEvent{wil::EventOptions::ManualReset};
    std::wstring m_displayName;
    std::wstring m_creatorProcessName;
    std::filesystem::path m_storageVhdPath;
    std::filesystem::path m_swapVhdPath;
    bool m_storageMounted = false;

    // N.B. m_lock must be acquired before acquiring m_containersLock or m_networksLock.
    // These locks protect m_containers without requiring an exclusive m_lock.
    // This allows independent operations to proceed while container bookkeeping remains synchronized.
    // WSLCVolumes has its own internal srwlock and does not require m_lock.
    std::mutex m_containersLock;
    std::unordered_map<std::string, std::unique_ptr<WSLCContainerImpl>> m_containers;
    std::optional<WSLCVolumes> m_volumes;
    std::mutex m_networksLock;
    std::unordered_map<std::string, NetworkEntry> m_networks;
    wil::unique_event m_sessionTerminatingEvent{wil::EventOptions::ManualReset};
    wil::unique_event m_sessionTerminatedEvent{wil::EventOptions::ManualReset};
    wil::unique_event m_vmExitedEvent;

    WSLCVirtualMachineTerminationReason m_terminationReason{WSLCVirtualMachineTerminationReasonUnknown};
    std::wstring m_terminationDetails;
    wil::srwlock m_lock;
    std::optional<IORelay> m_ioRelay;

    // VM lifecycle / idle-termination state.
    std::atomic<VmState> m_vmState{VmState::None};
    std::atomic<VmExitDisposition> m_vmExitDisposition{VmExitDisposition::Active};
    // In-flight activity count, idle timer and teardown callback, decoupled from this object's
    // lifetime (see IdleState in WSLCIdleState.h) so activity tokens and container COM wrappers can
    // safely manage activity without keeping the session alive. See WSLCContainerImpl's ActivityRef
    // (m_activityHold), WSLCSession::VmLease and CreateActivityToken().
    std::shared_ptr<IdleState> m_idleState{std::make_shared<IdleState>()};

    // Persisted settings required to (re)create the VM on demand. The string fields point
    // into the owned storage members below (or m_displayName) so they remain valid for the
    // lifetime of the session.
    WSLCSessionInitSettings m_settings{};
    std::optional<std::wstring> m_settingsCreatorProcessName;
    std::optional<std::wstring> m_settingsStoragePath;
    std::optional<std::string> m_settingsRootVhdTypeOverride;
    std::vector<BYTE> m_userSid;

    std::optional<ServiceRunningProcess> m_containerdProcess;
    std::optional<ServiceRunningProcess> m_dockerdProcess;
    WSLCFeatureFlags m_featureFlags{};
    std::function<void()> m_destructionCallback;
    std::atomic<bool> m_terminating{false};

    wil::com_ptr<IWSLCPluginNotifier> m_pluginNotifier;

    // User-provided handles that the session is currently doing IO on.
    std::mutex m_userHandlesLock;
    __guarded_by(m_userHandlesLock) std::vector<HANDLE> m_userHandles;

    // Threads currently inside an outgoing COM callback (e.g. IProgressCallback::OnProgress).
    std::recursive_mutex m_userCOMCallbacksLock;
    __guarded_by(m_userCOMCallbacksLock) std::map<DWORD, int> m_userCOMCallbackThreads;

    // Callbacks registered via RegisterCrashDumpCallback. std::list gives stable iterators that
    // survive insertions and unrelated erasures, so each CrashDumpSubscription stashes its own
    // iterator and uses it as an O(1) removal handle when the last reference is released.
    // The session's lifetime extends past Terminate() (the COM object outlives the VM), so this
    // list may outlive m_virtualMachine; that's fine because dispatch only runs while the VM
    // thread is alive.
    mutable wil::srwlock m_crashDumpLock;
    _Guarded_by_(m_crashDumpLock) CrashDumpCallbackList m_crashDumpCallbacks;

    // Used for testing only.
    std::mutex m_allocatedPortsLock;
    __guarded_by(m_allocatedPortsLock) std::map<uint16_t, std::pair<std::shared_ptr<VmPortAllocation>, size_t>> m_allocatedPorts;
};

} // namespace wsl::windows::service::wslc
