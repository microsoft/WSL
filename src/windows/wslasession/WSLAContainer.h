/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLAContainer.h

Abstract:

    Contains the definition for WSLAContainer.

--*/

#pragma once

#include "ServiceProcessLauncher.h"
#include "WSLASession.h"
#include "ContainerEventTracker.h"
#include "DockerHTTPClient.h"
#include "WSLAProcessControl.h"
#include "IORelay.h"
#include "COMImplClass.h"
#include "wsla_schema.h"
#include "WSLAContainerMetadata.h"

namespace wsl::windows::service::wsla {

class WSLAContainer;
class WSLASession;

class WSLAContainerImpl
{
public:
    struct ContainerPortMapping
    {
        NON_COPYABLE(ContainerPortMapping);

        ContainerPortMapping(WSLAVirtualMachine::VMPortMapping&& VmMapping, uint16_t ContainerPort) :
            VmMapping(std::move(VmMapping)), ContainerPort(ContainerPort)
        {
        }

        ContainerPortMapping(ContainerPortMapping&& Other) :
            VmMapping(std::move(Other.VmMapping)), ContainerPort(Other.ContainerPort)
        {
        }

        ContainerPortMapping& operator=(ContainerPortMapping&& Other)
        {
            if (this != &Other)
            {
                VmMapping = std::move(Other.VmMapping);
                ContainerPort = Other.ContainerPort;
            }
            return *this;
        }

        const char* ProtocolString() const
        {
            if (VmMapping.Protocol == IPPROTO_TCP)
            {
                return "tcp";
            }
            else
            {
                WI_ASSERT(VmMapping.Protocol == IPPROTO_UDP);
                return "udp";
            }
        }

        WSLAPortMapping Serialize() const
        {
            return WSLAPortMapping{
                .HostPort = ntohs(VmMapping.BindAddress.Ipv4.sin_port),
                .VmPort = VmMapping.VmPort.Port(),
                .ContainerPort = ContainerPort,
                .Family = VmMapping.BindAddress.si_family,
                .Protocol = VmMapping.Protocol,
                .BindingAddress = VmMapping.BindingAddressString()};
        }

        WSLAVirtualMachine::VMPortMapping VmMapping;
        uint16_t ContainerPort{};
    };

    NON_COPYABLE(WSLAContainerImpl);
    NON_MOVABLE(WSLAContainerImpl);

    WSLAContainerImpl(
        WSLASession& wslaSession,
        WSLAVirtualMachine& virtualMachine,
        std::string&& Id,
        std::string&& Name,
        std::string&& Image,
        std::vector<WSLAVolumeMount>&& volumes,
        std::vector<ContainerPortMapping>&& ports,
        std::map<std::string, std::string>&& labels,
        std::function<void(const WSLAContainerImpl*)>&& OnDeleted,
        ContainerEventTracker& EventTracker,
        DockerHTTPClient& DockerClient,
        IORelay& Relay,
        WSLAContainerState InitialState,
        WSLAProcessFlags InitProcessFlags,
        WSLAContainerFlags ContainerFlags);

    ~WSLAContainerImpl();

    void Start(WSLAContainerStartFlags Flags, LPCSTR DetachKeys);
    void Attach(LPCSTR DetachKeys, ULONG* Stdin, ULONG* Stdout, ULONG* Stderr) const;
    void Stop(_In_ WSLASignal Signal, _In_ LONG TimeoutSeconds);
    void Delete(WSLADeleteFlags Flags);
    void Export(ULONG TarHandle) const;
    void GetStateChangedAt(_Out_ ULONGLONG* StateChangedAt);
    void GetCreatedAt(_Out_ ULONGLONG* CreatedAt);
    void GetState(_Out_ WSLAContainerState* State);
    void GetInitProcess(_Out_ IWSLAProcess** process) const;
    void Exec(_In_ const WSLAProcessOptions* Options, LPCSTR DetachKeys, _Out_ IWSLAProcess** Process);
    void Inspect(LPSTR* Output) const;
    void Logs(WSLALogsFlags Flags, ULONG* Stdout, ULONG* Stderr, ULONGLONG Since, ULONGLONG Until, ULONGLONG Tail) const;
    void GetLabels(WSLALabelInformation** Labels, ULONG* Count) const;

    void CopyTo(IWSLAContainer** Container) const;

    const std::string& Image() const noexcept;
    const std::string& Name() const noexcept;
    WSLAContainerState State() const noexcept;

    __requires_lock_held(m_lock) void Transition(WSLAContainerState State) noexcept;

    void OnProcessReleased(DockerExecProcessControl* process) noexcept;

    const std::string& ID() const noexcept;

    // Returns the container flags used to decide whether to
    // auto-delete the container on stop.
    WSLAContainerFlags Flags() const noexcept
    {
        return m_containerFlags;
    }

    static std::unique_ptr<WSLAContainerImpl> Create(
        const WSLAContainerOptions& Options,
        WSLASession& wslaSession,
        WSLAVirtualMachine& virtualMachine,
        std::function<void(const WSLAContainerImpl*)>&& OnDeleted,
        ContainerEventTracker& EventTracker,
        DockerHTTPClient& DockerClient,
        IORelay& Relay);

    static std::unique_ptr<WSLAContainerImpl> Open(
        const common::docker_schema::ContainerInfo& DockerContainer,
        WSLASession& wslaSession,
        WSLAVirtualMachine& virtualMachine,
        std::function<void(const WSLAContainerImpl*)>&& OnDeleted,
        ContainerEventTracker& EventTracker,
        DockerHTTPClient& DockerClient,
        IORelay& Relay);

private:
    __requires_exclusive_lock_held(m_lock) void DeleteExclusiveLockHeld(WSLADeleteFlags Flags);

    void OnEvent(ContainerEvent event, std::optional<int> exitCode);
    void WaitForContainerEvent();
    __requires_exclusive_lock_held(m_lock) void ReleaseResources();
    __requires_exclusive_lock_held(m_lock) void ReleaseRuntimeResources();
    __requires_exclusive_lock_held(m_lock) void DisconnectComWrapper();
    std::unique_ptr<RelayedProcessIO> CreateRelayedProcessIO(wil::unique_handle&& stream, WSLAProcessFlags flags);

    wsl::windows::common::wsla_schema::InspectContainer BuildInspectContainer(const wsl::windows::common::docker_schema::InspectContainer& dockerInspect) const;

    void MapPorts();
    void UnmapPorts();

    mutable wil::srwlock m_lock;
    std::string m_name;
    std::string m_image;
    std::string m_id;
    WSLAProcessFlags m_initProcessFlags{};
    WSLAContainerFlags m_containerFlags{};
    mutable std::mutex m_processesLock;
    __guarded_by(m_processesLock) std::vector<DockerExecProcessControl*> m_processes;
    __guarded_by(m_processesLock) Microsoft::WRL::ComPtr<WSLAProcess> m_initProcess;
    __guarded_by(m_processesLock) DockerContainerProcessControl* m_initProcessControl = nullptr;

    wil::unique_event m_stoppedNotifiedEvent{wil::EventOptions::ManualReset};
    DockerHTTPClient& m_dockerClient;
    std::uint64_t m_stateChangedAt{static_cast<std::uint64_t>(std::time(nullptr))};
    std::uint64_t m_createdAt{static_cast<std::uint64_t>(std::time(nullptr))};
    WSLAContainerState m_state = WslaContainerStateInvalid;
    WSLASession& m_wslaSession;
    WSLAVirtualMachine& m_virtualMachine;
    std::vector<ContainerPortMapping> m_mappedPorts;
    std::vector<WSLAVolumeMount> m_mountedVolumes;
    std::map<std::string, std::string> m_labels;
    Microsoft::WRL::ComPtr<WSLAContainer> m_comWrapper;
    ContainerEventTracker& m_eventTracker;
    ContainerEventTracker::ContainerTrackingReference m_containerEvents;
    IORelay& m_ioRelay;
};

class DECLSPEC_UUID("B1F1C4E3-C225-4CAE-AD8A-34C004DE1AE4") WSLAContainer
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IWSLAContainer, IFastRundown, ISupportErrorInfo>,
      public COMImplClass<WSLAContainerImpl>
{

public:
    WSLAContainer(WSLAContainerImpl* impl, std::function<void(const WSLAContainerImpl*)>&& OnDeleted);

    IFACEMETHOD(Attach)(_In_opt_ LPCSTR DetachKeys, _Out_ ULONG* Stdin, _Out_ ULONG* Stdout, _Out_ ULONG* Stderr) override;
    IFACEMETHOD(Stop)(_In_ WSLASignal Signal, _In_ LONG TimeoutSeconds) override;
    IFACEMETHOD(Delete)(WSLADeleteFlags Flags) override;
    IFACEMETHOD(Export)(_In_ ULONG TarHandle) override;
    IFACEMETHOD(GetState)(_Out_ WSLAContainerState* State) override;
    IFACEMETHOD(GetInitProcess)(_Out_ IWSLAProcess** process) override;
    IFACEMETHOD(Exec)(_In_ const WSLAProcessOptions* Options, _In_opt_ LPCSTR DetachKeys, _Out_ IWSLAProcess** Process) override;
    IFACEMETHOD(Start)(WSLAContainerStartFlags Flags, _In_opt_ LPCSTR DetachKeys) override;
    IFACEMETHOD(Inspect)(_Out_ LPSTR* Output) override;
    IFACEMETHOD(Logs)(_In_ WSLALogsFlags Flags, _Out_ ULONG* Stdout, _Out_ ULONG* Stderr, _In_ ULONGLONG Since, _In_ ULONGLONG Until, _In_ ULONGLONG Tail) override;
    IFACEMETHOD(GetId)(_Out_ WSLAContainerId Id) override;
    IFACEMETHOD(GetName)(_Out_ LPSTR* Name) override;
    IFACEMETHOD(GetLabels)(_Out_ WSLALabelInformation** Labels, _Out_ ULONG* Count) override;

    IFACEMETHOD(InterfaceSupportsErrorInfo)(REFIID riid);

private:
    std::function<void(const WSLAContainerImpl*)> m_onDeleted;
};
} // namespace wsl::windows::service::wsla