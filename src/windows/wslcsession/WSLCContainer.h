/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCContainer.h

Abstract:

    Contains the definition for WSLCContainer.

--*/

#pragma once

#include "ServiceProcessLauncher.h"
#include "WSLCSession.h"
#include "WSLCIdleState.h"
#include "DockerEventTracker.h"
#include "DockerHTTPClient.h"
#include "WSLCProcessControl.h"
#include "IORelay.h"
#include "COMImplClass.h"
#include "wslc_schema.h"
#include "WSLCCompat.h"
#include "WSLCContainerMetadata.h"
#include "WSLCNetworkMetadata.h"
#include "WSLCVhdVolume.h"
#include <unordered_map>

namespace wsl::windows::service::wslc {

class WSLCContainer;
class WSLCSession;
class WSLCSessionRuntime;
class WSLCVolumes;

class unique_com_disconnect
{
public:
    NON_COPYABLE(unique_com_disconnect);
    DEFAULT_MOVABLE(unique_com_disconnect);

    unique_com_disconnect() = default;
    unique_com_disconnect(Microsoft::WRL::ComPtr<WSLCContainer>&& wrapper) noexcept;
    ~unique_com_disconnect() noexcept;

private:
    Microsoft::WRL::ComPtr<WSLCContainer> m_wrapper;
};

struct ContainerPortMapping
{
    NON_COPYABLE(ContainerPortMapping);

    ContainerPortMapping(VMPortMapping&& VmMapping, uint16_t ContainerPort);
    ContainerPortMapping(ContainerPortMapping&& Other);

    ContainerPortMapping& operator=(ContainerPortMapping&& Other);
    const char* ProtocolString() const;

    WSLCPortMapping Serialize() const;

    VMPortMapping VmMapping;
    uint16_t ContainerPort{};
};

class WSLCContainerImpl : public std::enable_shared_from_this<WSLCContainerImpl>
{
public:
    NON_COPYABLE(WSLCContainerImpl);
    NON_MOVABLE(WSLCContainerImpl);

    WSLCContainerImpl(
        WSLCSession& wslcSession,
        WSLCSessionRuntime& runtime,
        IWSLCPluginNotifier* pluginNotifier,
        std::string&& Id,
        std::string&& Name,
        std::string&& Image,
        std::string NetworkMode,
        std::vector<WSLCVolumeMount>&& volumes,
        std::vector<std::string>&& namedVolumes,
        std::vector<ContainerPortMapping>&& ports,
        std::map<std::string, std::string>&& labels,
        std::function<void(const WSLCContainerImpl*)>&& OnDeleted,
        WSLCContainerState InitialState,
        std::uint64_t CreatedAt,
        WSLCProcessFlags InitProcessFlags,
        WSLCContainerFlags ContainerFlags);

    ~WSLCContainerImpl();

    void Initialize();

    void Start(WSLCContainerStartFlags Flags, const WSLCProcessStartOptions* StartOptions);
    void Attach(LPCSTR DetachKeys, WSLCHandle* Stdin, WSLCHandle* Stdout, WSLCHandle* Stderr) const;
    void Stop(_In_ WSLCSignal Signal, _In_ LONG TimeoutSeconds, bool Kill);
    void Delete(WSLCDeleteFlags Flags);
    void Export(WSLCHandle TarHandle) const;
    void UploadArchive(WSLCHandle TarHandle, LPCSTR DestPath, ULONGLONG ContentSize) const;
    void DownloadArchive(LPCSTR SrcPath, WSLCHandle OutHandle) const;
    void GetStateChangedAt(_Out_ ULONGLONG* StateChangedAt);
    void GetCreatedAt(_Out_ ULONGLONG* CreatedAt);
    void GetState(_Out_ WSLCContainerState* State);
    void GetInitProcess(_Out_ IWSLCProcess** process) const;
    void Exec(_In_ const WSLCProcessOptions* Options, const WSLCProcessStartOptions* StartOptions, _Out_ IWSLCProcess** Process);
    void Inspect(LPSTR* Output) const;
    void Logs(WSLCLogsFlags Flags, WSLCHandle* Stdout, WSLCHandle* Stderr, ULONGLONG Since, ULONGLONG Until, ULONGLONG Tail) const;
    void Stats(LPSTR* Output) const;
    void GetLabels(WSLCLabelInformation** Labels, ULONG* Count) const;
    void ConnectToNetwork(const WSLCNetworkConnectionOptions* Options);
    void DisconnectFromNetwork(LPCSTR NetworkName);

    void CopyTo(IWSLCContainer** Container) const;

    const std::string& Image() const noexcept;
    const std::string& Name() const noexcept;
    WSLCContainerState State() const noexcept;
    std::vector<WSLCPortMapping> GetPorts() const;

    // Reconciles a surviving wrapper after its VM was torn down (idle-termination or crash) while the
    // container was running: records a synthetic init-process exit, releases VM-scoped resources and
    // drops to Exited (releasing the VM activity hold). Keeps the wrapper connected so client COM
    // references stay valid across the VM restart.
    void OnVmTornDown() noexcept;

    // Re-registers a survivor's VM-scoped port allocations against the restarted VM (see OnVmTornDown).
    void RecoverPorts(const common::docker_schema::ContainerInfo& dockerContainer);

    // Honors --rm for a survivor that was running when the VM was torn down: OnVmTornDown forced it to
    // Exited but deferred the auto-remove delete while dockerd was down. Removes it now that the VM is
    // back, mirroring OnStopped's Running->Exited delete. Sets Removed and returns the disconnect
    // wrapper (destroy after dropping the container from tracking) when it deletes; otherwise a no-op.
    [[nodiscard]] unique_com_disconnect RemoveExitedAutoRemoveSurvivor(bool& Removed);

    __requires_lock_held(m_lock) void Transition(WSLCContainerState State, std::optional<std::uint64_t> stateChangedAt = std::nullopt) noexcept;

    const std::string& ID() const noexcept;

    // Returns the container flags used to decide whether to
    // auto-delete the container on stop.
    WSLCContainerFlags Flags() const noexcept
    {
        return m_containerFlags;
    }

    static std::shared_ptr<WSLCContainerImpl> Create(
        const WSLCContainerOptions& Options,
        const std::string& Name,
        WSLCSession& wslcSession,
        WSLCSessionRuntime& runtime,
        IWSLCPluginNotifier* pluginNotifier,
        const std::unordered_map<std::string, NetworkEntry>& SessionNetworks,
        std::function<void(const WSLCContainerImpl*)>&& OnDeleted);

    static std::shared_ptr<WSLCContainerImpl> Open(
        const common::docker_schema::ContainerInfo& DockerContainer,
        WSLCSession& wslcSession,
        WSLCSessionRuntime& runtime,
        IWSLCPluginNotifier* pluginNotifier,
        std::function<void(const WSLCContainerImpl*)>&& OnDeleted);

private:
    __requires_exclusive_lock_held(m_lock) [[nodiscard]] unique_com_disconnect DeleteExclusiveLockHeld(WSLCDeleteFlags Flags);

    void AllocateBridgedModePorts();
    void OnEvent(ContainerEvent event, std::optional<int> exitCode, std::uint64_t eventTime);

    __requires_exclusive_lock_held(m_lock) [[nodiscard]] unique_com_disconnect ReleaseResources();
    __requires_exclusive_lock_held(m_lock) void ReleaseRuntimeResources();
    __requires_exclusive_lock_held(m_lock) void ReleaseProcesses();
    __requires_exclusive_lock_held(m_lock) [[nodiscard]] unique_com_disconnect PrepareDisconnectComWrapper();

    __requires_exclusive_lock_held(m_lock) [[nodiscard]] unique_com_disconnect OnStopped(std::optional<std::uint64_t> stopTimestamp);

    void SetExitCode(int ExitCode) noexcept;
    void SignalInitProcessExit() noexcept;

    std::unique_ptr<RelayedProcessIO> CreateRelayedProcessIO(wil::unique_handle&& stream, WSLCProcessFlags flags);

    wsl::windows::common::wslc_schema::InspectContainer BuildInspectContainer(const wsl::windows::common::docker_schema::InspectContainer& dockerInspect) const;

    void MapPorts();
    void UnmapPorts();

    // Acquires or releases the activity hold so it is held exactly while the container is Running,
    // keeping the session's VM alive across idle teardown.
    __requires_lock_held(m_lock) void UpdateActivityHoldLockHeld() noexcept;

    __requires_shared_lock_held(m_lock) std::string InspectLockHeld() const;

    // Accessors for the session's VM-scoped resources. The container outlives any single VM: it
    // survives idle-termination and is reused when the VM restarts. These fetch the current VM's
    // objects from the (stable) runtime rather than caching references that would dangle across a
    // restart. They are only valid while a VM lease is held (i.e. the VM is running).
    WSLCVirtualMachine& Vm() const;
    DockerHTTPClient& Docker() const;
    WSLCVolumes& Volumes() const;
    DockerEventTracker& Events() const;
    IORelay& Relay() const;

    mutable wil::srwlock m_lock;
    std::string m_name;
    std::string m_image;
    std::string m_id;
    WSLCProcessFlags m_initProcessFlags{};
    WSLCContainerFlags m_containerFlags{};
    mutable std::mutex m_processesLock;
    __guarded_by(m_processesLock) std::vector<std::weak_ptr<DockerExecProcessControl>> m_processes;
    __guarded_by(m_processesLock) Microsoft::WRL::ComPtr<IWSLCProcess> m_initProcess;
    __guarded_by(m_processesLock) DockerContainerProcessControl* m_initProcessControl = nullptr;

    struct StopNotification
    {
        std::atomic<std::uint64_t> EventTime{0};
        wil::unique_event Event{wil::EventOptions::None};
    } m_stopNotification;

    wil::unique_event m_destroyEvent{wil::EventOptions::ManualReset};

    // Serializes Stop() callers and signals OnEvent that a Stop is in flight.
    // Must be acquired before m_lock when both are needed.
    std::mutex m_stopLock;

    WSLCSessionRuntime& m_runtime;
    std::uint64_t m_stateChangedAt{static_cast<std::uint64_t>(std::time(nullptr))};
    std::uint64_t m_createdAt{};
    WSLCContainerState m_state = WslcContainerStateInvalid;
    WSLCSession& m_wslcSession;
    IWSLCPluginNotifier* m_pluginNotifier;
    std::vector<ContainerPortMapping> m_mappedPorts;
    std::vector<WSLCVolumeMount> m_mountedVolumes;

    std::vector<std::string> m_namedVolumes;

    std::map<std::string, std::string> m_labels;
    Microsoft::WRL::ComPtr<WSLCContainer> m_comWrapper;
    DockerEventTracker::EventTrackingReference m_containerEvents;
    std::string m_networkMode;

    // Held (non-empty) exactly while the container is Running so the session's VM stays alive even
    // when no client holds the wrapper (e.g. a detached `run -d` container). Maintained by
    // UpdateActivityHoldLockHeld(); released automatically when the container is destroyed.
    ActivityRef m_activityHold;
};

class DECLSPEC_UUID("B1F1C4E3-C225-4CAE-AD8A-34C004DE1AE4") WSLCContainer
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IWSLCContainer, IWSLCCompatContainer, IFastRundown, ISupportErrorInfo>,
      public COMImplClass<WSLCContainerImpl, std::weak_ptr<WSLCContainerImpl>>
{

public:
    WSLCContainer(WSLCSession& session, std::function<void(const WSLCContainerImpl*)>&& OnDeleted);

    IFACEMETHOD(Attach)(_In_opt_ LPCSTR DetachKeys, _Out_ WSLCHandle* Stdin, _Out_ WSLCHandle* Stdout, _Out_ WSLCHandle* Stderr) override;
    IFACEMETHOD(Stop)(_In_ WSLCSignal Signal, _In_ LONG TimeoutSeconds) override;
    IFACEMETHOD(Kill)(_In_ WSLCSignal Signal) override;
    IFACEMETHOD(Delete)(WSLCDeleteFlags Flags) override;
    IFACEMETHOD(Export)(_In_ WSLCHandle TarHandle) override;
    IFACEMETHOD(UploadArchive)(_In_ WSLCHandle TarHandle, _In_ LPCSTR DestPath, _In_ ULONGLONG ContentSize) override;
    IFACEMETHOD(DownloadArchive)(_In_ LPCSTR SrcPath, _In_ WSLCHandle OutHandle) override;
    IFACEMETHOD(GetState)(_Out_ WSLCContainerState* State) override;
    IFACEMETHOD(GetInitProcess)(_Out_ IWSLCProcess** process) override;
    IFACEMETHOD(Exec)(_In_ const WSLCProcessOptions* Options, _In_opt_ const WSLCProcessStartOptions* StartOptions, _Out_ IWSLCProcess** Process) override;
    IFACEMETHOD(Start)(WSLCContainerStartFlags Flags, _In_opt_ const WSLCProcessStartOptions* StartOptions, _In_opt_ IWarningCallback* WarningCallback) override;
    IFACEMETHOD(Inspect)(_Out_ LPSTR* Output) override;
    IFACEMETHOD(Logs)(_In_ WSLCLogsFlags Flags, _Out_ WSLCHandle* Stdout, _Out_ WSLCHandle* Stderr, _In_ ULONGLONG Since, _In_ ULONGLONG Until, _In_ ULONGLONG Tail) override;
    IFACEMETHOD(GetId)(_Out_ WSLCContainerId Id) override;
    IFACEMETHOD(GetName)(_Out_ LPSTR* Name) override;
    IFACEMETHOD(GetLabels)(_Out_ WSLCLabelInformation** Labels, _Out_ ULONG* Count) override;
    IFACEMETHOD(Stats)(_Out_ LPSTR* Output) override;
    IFACEMETHOD(ConnectToNetwork)(_In_ const WSLCNetworkConnectionOptions* Options) override;
    IFACEMETHOD(DisconnectFromNetwork)(_In_ LPCSTR NetworkName) override;

    // IWSLCCompatContainer.
    IFACEMETHOD(Start)(_In_ WSLCContainerStartFlags Flags) override;
    IFACEMETHOD(GetInitProcess)(_Out_ IWSLCCompatProcess** Process) override;
    IFACEMETHOD(Exec)(_In_ const WSLCCompatProcessOptions* Options, _Out_ IWSLCCompatProcess** Process) override;

    IFACEMETHOD(InterfaceSupportsErrorInfo)(REFIID riid);

    // Cache read-only properties so they remain accessible after the impl is disconnected.
    // Called from WSLCContainerImpl::PrepareDisconnectComWrapper() while m_lock is held exclusively.
    void CacheState(const std::string& id, const std::string& name, WSLCContainerState state, const Microsoft::WRL::ComPtr<IWSLCProcess>& initProcess) noexcept;

private:
    WSLCSession& m_session;
    std::function<void(const WSLCContainerImpl*)> m_onDeleted;

    // Cached read-only properties populated by CacheState() so they remain
    // accessible after the impl is disconnected.
    mutable wil::srwlock m_cacheLock;
    _Guarded_by_(m_cacheLock) std::optional<std::string> m_cachedId;
    _Guarded_by_(m_cacheLock) std::optional<std::string> m_cachedName;
    _Guarded_by_(m_cacheLock) std::optional<WSLCContainerState> m_cachedState;
    _Guarded_by_(m_cacheLock) Microsoft::WRL::ComPtr<IWSLCProcess> m_cachedInitProcess;
};

} // namespace wsl::windows::service::wslc
