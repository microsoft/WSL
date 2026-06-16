/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCSession.h

Abstract:

    TODO

--*/

#pragma once

#include "wslc.h"
#include "WSLCVirtualMachine.h"
#include "WSLCContainer.h"
#include "WSLCVolumes.h"
#include "WSLCNetworkMetadata.h"
#include "DockerEventTracker.h"
#include "DockerHTTPClient.h"
#include "IORelay.h"
#include <list>
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
// The SYSTEM service creates the VM and passes IWSLCVirtualMachine to Initialize().
//
class DECLSPEC_UUID("4877FEFC-4977-4929-A958-9F36AA1892A4") WSLCSession
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::WinRtClassicComMix>, IWSLCSession, IFastRundown, ISupportErrorInfo>
{
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
    IFACEMETHOD(LoadImage)(_In_ const WSLCHandle ImageHandle, _In_ IProgressCallback* ProgressCallback, _In_ ULONGLONG ContentLength, _In_opt_ IWarningCallback* WarningCallback) override;
    IFACEMETHOD(ImportImage)(
        _In_ const WSLCHandle ImageHandle,
        _In_ LPCSTR ImageName,
        _In_ IProgressCallback* ProgressCallback,
        _In_ ULONGLONG ContentLength,
        _In_opt_ IWarningCallback* WarningCallback) override;
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

    common::io::MultiHandleWait CreateIOContext(HANDLE CancelHandle = nullptr);

    UserHandle OpenUserHandle(WSLCHandle Handle);
    void ReleaseUserHandle(HANDLE Handle);

    UserCOMCallback RegisterUserCOMCallback();
    void UnregisterUserCOMCallback(DWORD ThreadId);

    HANDLE SessionTerminatingEvent() const noexcept
    {
        return m_sessionTerminatingEvent.get();
    }

    ULONG Id() const noexcept
    {
        return m_id;
    }

    bool WaitForEventOrSessionTerminating(HANDLE Event, std::chrono::milliseconds Timeout) const;

private:
    ULONG m_id = 0;

    __requires_lock_held(m_userHandlesLock) void CancelUserHandleIO();
    __requires_lock_held(m_userCOMCallbacksLock) void CancelUserCOMCallbacks();

    _Requires_shared_lock_held_(m_lock)
    void CreateContainerImpl(const WSLCContainerOptions* Options, IWSLCContainer** Container);

    void ConfigureStorage(const WSLCSessionInitSettings& Settings, PSID UserSid);
    void Ext4Format(const std::string& Device);
    _Requires_shared_lock_held_(m_lock)
    std::string InspectImageLockHeld(const std::string& Id);
    void OnContainerDeleted(const WSLCContainerImpl* Container);

    void OnCrashDumpWritten(const std::wstring& DumpPath, const std::string& ProcessName, ULONGLONG Pid, ULONG Signal, ULONGLONG Timestamp);

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
    void ImportImageImpl(DockerHTTPClient::HTTPRequestContext& Request, const WSLCHandle ImageHandle);
    void RecoverExistingContainers();
    void RecoverExistingNetworks();

    void SaveImageImpl(std::pair<uint32_t, wil::unique_socket>& RequestCodePair, WSLCHandle OutputHandle, HANDLE CancelEvent);
    void StreamImageOperation(DockerHTTPClient::HTTPRequestContext& requestContext, LPCSTR Image, LPCSTR OperationName, IProgressCallback* ProgressCallback);

    std::optional<DockerHTTPClient> m_dockerClient;
    std::optional<WSLCVirtualMachine> m_virtualMachine;
    std::optional<DockerEventTracker> m_eventTracker;
    wil::unique_event m_dockerdReadyEvent{wil::EventOptions::ManualReset};
    std::wstring m_displayName;
    std::wstring m_creatorProcessName;
    std::filesystem::path m_storageVhdPath;
    std::filesystem::path m_swapVhdPath;

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
    IORelay m_ioRelay;
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
