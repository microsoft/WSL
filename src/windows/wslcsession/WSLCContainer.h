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
#include "ContainerEventTracker.h"
#include "DockerHTTPClient.h"
#include "WSLCProcessControl.h"
#include "IORelay.h"
#include "COMImplClass.h"
#include "wslc_schema.h"
#include "WSLCContainerMetadata.h"
#include "WSLCNetworkMetadata.h"
#include "WSLCVhdVolume.h"
#include <unordered_map>

namespace wsl::windows::service::wslc {

class WSLCContainer;
class WSLCSession;

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

class WSLCContainerImpl
{
public:
    NON_COPYABLE(WSLCContainerImpl);
    NON_MOVABLE(WSLCContainerImpl);

    WSLCContainerImpl(
        WSLCSession& wslcSession,
        WSLCVirtualMachine& virtualMachine,
        std::string&& Id,
        std::string&& Name,
        std::string&& Image,
        WSLCContainerNetworkType NetworkMode,
        std::vector<WSLCVolumeMount>&& volumes,
        std::vector<ContainerPortMapping>&& ports,
        std::map<std::string, std::string>&& labels,
        std::function<void(const WSLCContainerImpl*)>&& OnDeleted,
        ContainerEventTracker& EventTracker,
        DockerHTTPClient& DockerClient,
        IORelay& Relay,
        WSLCContainerState InitialState,
        std::uint64_t CreatedAt,
        WSLCProcessFlags InitProcessFlags,
        WSLCContainerFlags ContainerFlags);

    ~WSLCContainerImpl();

    void Start(WSLCContainerStartFlags Flags, LPCSTR DetachKeys);
    void Attach(LPCSTR DetachKeys, WSLCHandle* Stdin, WSLCHandle* Stdout, WSLCHandle* Stderr) const;
    void Stop(_In_ WSLCSignal Signal, _In_ LONG TimeoutSeconds, bool Kill);
    void Delete(WSLCDeleteFlags Flags);
    void Export(WSLCHandle TarHandle) const;
    void GetStateChangedAt(_Out_ ULONGLONG* StateChangedAt);
    void GetCreatedAt(_Out_ ULONGLONG* CreatedAt);
    void GetState(_Out_ WSLCContainerState* State);
    void GetInitProcess(_Out_ IWSLCProcess** process) const;
    void Exec(_In_ const WSLCProcessOptions* Options, LPCSTR DetachKeys, _Out_ IWSLCProcess** Process);
    void Inspect(LPSTR* Output) const;
    void Logs(WSLCLogsFlags Flags, WSLCHandle* Stdout, WSLCHandle* Stderr, ULONGLONG Since, ULONGLONG Until, ULONGLONG Tail) const;
    void GetLabels(WSLCLabelInformation** Labels, ULONG* Count) const;

    void CopyTo(IWSLCContainer** Container) const;

    const std::string& Image() const noexcept;
    const std::string& Name() const noexcept;
    WSLCContainerState State() const noexcept;
    std::vector<WSLCPortMapping> GetPorts() const;

    __requires_lock_held(m_lock) void Transition(WSLCContainerState State, std::optional<std::uint64_t> stateChangedAt = std::nullopt) noexcept;

    void OnProcessReleased(DockerExecProcessControl* process) noexcept;

    const std::string& ID() const noexcept;

    // Returns the container flags used to decide whether to
    // auto-delete the container on stop.
    WSLCContainerFlags Flags() const noexcept
    {
        return m_containerFlags;
    }

    static std::unique_ptr<WSLCContainerImpl> Create(
        const WSLCContainerOptions& Options,
        WSLCSession& wslcSession,
        WSLCVirtualMachine& virtualMachine,
        const std::unordered_map<std::string, std::unique_ptr<IWSLCVolume>>& SessionVolumes,
        const std::unordered_map<std::string, NetworkEntry>& SessionNetworks,
        std::function<void(const WSLCContainerImpl*)>&& OnDeleted,
        ContainerEventTracker& EventTracker,
        DockerHTTPClient& DockerClient,
        IORelay& Relay);

    static std::unique_ptr<WSLCContainerImpl> Open(
        const common::docker_schema::ContainerInfo& DockerContainer,
        WSLCSession& wslcSession,
        WSLCVirtualMachine& virtualMachine,
        const std::unordered_map<std::string, std::unique_ptr<IWSLCVolume>>& sessionVolumes,
        const std::unordered_set<std::string>& anonymousVolumes,
        std::function<void(const WSLCContainerImpl*)>&& OnDeleted,
        ContainerEventTracker& EventTracker,
        DockerHTTPClient& DockerClient,
        IORelay& Relay);

private:
    __requires_exclusive_lock_held(m_lock) [[nodiscard]] unique_com_disconnect DeleteExclusiveLockHeld(WSLCDeleteFlags Flags);

    void AllocateBridgedModePorts();
    void OnEvent(ContainerEvent event, std::optional<int> exitCode, std::uint64_t eventTime);

    bool WaitForEvent(const wil::unique_event& Event, std::chrono::milliseconds Timeout) const;

    __requires_exclusive_lock_held(m_lock) [[nodiscard]] unique_com_disconnect ReleaseResources();
    __requires_exclusive_lock_held(m_lock) void ReleaseRuntimeResources();
    __requires_exclusive_lock_held(m_lock) void ReleaseProcesses();
    __requires_exclusive_lock_held(m_lock) [[nodiscard]] unique_com_disconnect PrepareDisconnectComWrapper();

    std::unique_ptr<RelayedProcessIO> CreateRelayedProcessIO(wil::unique_handle&& stream, WSLCProcessFlags flags);

    wsl::windows::common::wslc_schema::InspectContainer BuildInspectContainer(const wsl::windows::common::docker_schema::InspectContainer& dockerInspect) const;

    void MapPorts();
    void UnmapPorts();

    mutable wil::srwlock m_lock;
    std::string m_name;
    std::string m_image;
    std::string m_id;
    WSLCProcessFlags m_initProcessFlags{};
    WSLCContainerFlags m_containerFlags{};
    mutable std::mutex m_processesLock;
    __guarded_by(m_processesLock) std::vector<DockerExecProcessControl*> m_processes;
    __guarded_by(m_processesLock) Microsoft::WRL::ComPtr<IWSLCProcess> m_initProcess;
    __guarded_by(m_processesLock) DockerContainerProcessControl* m_initProcessControl = nullptr;

    struct StopNotification
    {
        std::atomic<std::uint64_t> EventTime{0};
        wil::unique_event Event{wil::EventOptions::None};
    } m_stopNotification;

    // Serializes Stop() callers and signals OnEvent that a Stop is in flight.
    // Must be acquired before m_lock when both are needed.
    std::mutex m_stopLock;

    DockerHTTPClient& m_dockerClient;
    std::uint64_t m_stateChangedAt{static_cast<std::uint64_t>(std::time(nullptr))};
    std::uint64_t m_createdAt{};
    WSLCContainerState m_state = WslcContainerStateInvalid;
    WSLCSession& m_wslcSession;
    WSLCVirtualMachine& m_virtualMachine;
    std::vector<ContainerPortMapping> m_mappedPorts;
    std::vector<WSLCVolumeMount> m_mountedVolumes;
    std::map<std::string, std::string> m_labels;
    Microsoft::WRL::ComPtr<WSLCContainer> m_comWrapper;
    ContainerEventTracker& m_eventTracker;
    ContainerEventTracker::ContainerTrackingReference m_containerEvents;
    IORelay& m_ioRelay;
    WSLCContainerNetworkType m_networkingMode{};
};

class DECLSPEC_UUID("B1F1C4E3-C225-4CAE-AD8A-34C004DE1AE4") WSLCContainer
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IWSLCContainer, IFastRundown, ISupportErrorInfo>,
      public COMImplClass<WSLCContainerImpl>
{

public:
    WSLCContainer(WSLCContainerImpl* impl, std::function<void(const WSLCContainerImpl*)>&& OnDeleted);

    IFACEMETHOD(Attach)(_In_opt_ LPCSTR DetachKeys, _Out_ WSLCHandle* Stdin, _Out_ WSLCHandle* Stdout, _Out_ WSLCHandle* Stderr) override;
    IFACEMETHOD(Stop)(_In_ WSLCSignal Signal, _In_ LONG TimeoutSeconds) override;
    IFACEMETHOD(Kill)(_In_ WSLCSignal Signal) override;
    IFACEMETHOD(Delete)(WSLCDeleteFlags Flags) override;
    IFACEMETHOD(Export)(_In_ WSLCHandle TarHandle) override;
    IFACEMETHOD(GetState)(_Out_ WSLCContainerState* State) override;
    IFACEMETHOD(GetInitProcess)(_Out_ IWSLCProcess** process) override;
    IFACEMETHOD(Exec)(_In_ const WSLCProcessOptions* Options, _In_opt_ LPCSTR DetachKeys, _Out_ IWSLCProcess** Process) override;
    IFACEMETHOD(Start)(WSLCContainerStartFlags Flags, _In_opt_ LPCSTR DetachKeys) override;
    IFACEMETHOD(Inspect)(_Out_ LPSTR* Output) override;
    IFACEMETHOD(Logs)(_In_ WSLCLogsFlags Flags, _Out_ WSLCHandle* Stdout, _Out_ WSLCHandle* Stderr, _In_ ULONGLONG Since, _In_ ULONGLONG Until, _In_ ULONGLONG Tail) override;
    IFACEMETHOD(GetId)(_Out_ WSLCContainerId Id) override;
    IFACEMETHOD(GetName)(_Out_ LPSTR* Name) override;
    IFACEMETHOD(GetLabels)(_Out_ WSLCLabelInformation** Labels, _Out_ ULONG* Count) override;

    IFACEMETHOD(InterfaceSupportsErrorInfo)(REFIID riid);

    // Cache read-only properties so they remain accessible after the impl is disconnected.
    // Called from WSLCContainerImpl::PrepareDisconnectComWrapper() while m_lock is held exclusively.
    void CacheState(const std::string& id, const std::string& name, WSLCContainerState state, const Microsoft::WRL::ComPtr<IWSLCProcess>& initProcess) noexcept;

private:
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
