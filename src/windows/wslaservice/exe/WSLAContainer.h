/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLAContainer.h

Abstract:

    Contains the definition for WSLAContainer.

--*/

#pragma once

#include "ServiceProcessLauncher.h"
#include "WSLAVirtualMachine.h"
#include "ContainerEventTracker.h"
#include "DockerHTTPClient.h"
#include "WSLAProcessControl.h"
#include "IORelay.h"
#include "COMImplClass.h"
#include "WSLAContainerMetadata.h"

namespace wsl::windows::service::wsla {

class WSLAContainer;

class WSLAContainerImpl
{
public:
    NON_COPYABLE(WSLAContainerImpl);
    NON_MOVABLE(WSLAContainerImpl);

    WSLAContainerImpl(
        WSLAVirtualMachine* parentVM,
        std::string&& Id,
        std::string&& Name,
        std::string&& Image,
        std::vector<WSLAVolumeMount>&& volumes,
        std::vector<WSLAPortMapping>&& ports,
        std::map<std::string, std::string>&& labels,
        std::function<void(const WSLAContainerImpl*)>&& OnDeleted,
        ContainerEventTracker& EventTracker,
        DockerHTTPClient& DockerClient,
        IORelay& Relay,
        WSLA_CONTAINER_STATE InitialState,
        WSLAProcessFlags InitProcessFlags,
        WSLAContainerFlags ContainerFlags);

    ~WSLAContainerImpl();

    void Start(WSLAContainerStartFlags Flags);

    void Attach(ULONG* Stdin, ULONG* Stdout, ULONG* Stderr);
    void Stop(_In_ WSLASignal Signal, _In_ LONGLONG TimeoutSeconds);
    void Delete();
    void GetState(_Out_ WSLA_CONTAINER_STATE* State);
    void GetInitProcess(_Out_ IWSLAProcess** process);
    void Exec(_In_ const WSLA_PROCESS_OPTIONS* Options, _Out_ IWSLAProcess** Process, _Out_ int* Errno);
    void Inspect(LPSTR* Output);
    void GetID(LPSTR* Id);
    void Logs(WSLALogsFlags Flags, ULONG* Stdout, ULONG* Stderr, ULONGLONG Since, ULONGLONG Until, ULONGLONG Tail);
    void GetLabels(WSLA_LABEL_INFORMATION** Labels, ULONG* Count);

    IWSLAContainer& ComWrapper();

    const std::string& Image() const noexcept;
    const std::string& Name() const noexcept;
    WSLA_CONTAINER_STATE State() noexcept;

    void OnProcessReleased(DockerExecProcessControl* process);

    const std::string& ID() const noexcept;

    static std::unique_ptr<WSLAContainerImpl> Create(
        const WSLA_CONTAINER_OPTIONS& Options,
        WSLAVirtualMachine& parentVM,
        std::function<void(const WSLAContainerImpl*)>&& OnDeleted,
        ContainerEventTracker& EventTracker,
        DockerHTTPClient& DockerClient,
        IORelay& Relay);

    static std::unique_ptr<WSLAContainerImpl> Open(
        const common::docker_schema::ContainerInfo& DockerContainer,
        WSLAVirtualMachine& parentVM,
        std::function<void(const WSLAContainerImpl*)>&& OnDeleted,
        ContainerEventTracker& EventTracker,
        DockerHTTPClient& DockerClient,
        IORelay& Relay);

private:
    void OnEvent(ContainerEvent event, std::optional<int> exitCode);
    void WaitForContainerEvent();
    std::unique_ptr<RelayedProcessIO> CreateRelayedProcessIO(wil::unique_handle&& stream, WSLAProcessFlags flags);

    std::recursive_mutex m_lock;
    std::string m_name;
    std::string m_image;
    std::string m_id;
    WSLAProcessFlags m_initProcessFlags{};
    WSLAContainerFlags m_containerFlags{};
    std::vector<DockerExecProcessControl*> m_processes;
    DockerHTTPClient& m_dockerClient;
    WSLA_CONTAINER_STATE m_state = WslaContainerStateInvalid;
    WSLAVirtualMachine* m_parentVM = nullptr;
    std::vector<WSLAPortMapping> m_mappedPorts;
    std::vector<WSLAVolumeMount> m_mountedVolumes;
    std::map<std::string, std::string> m_labels;
    Microsoft::WRL::ComPtr<WSLAContainer> m_comWrapper;
    Microsoft::WRL::ComPtr<WSLAProcess> m_initProcess;
    DockerContainerProcessControl* m_initProcessControl = nullptr;
    ContainerEventTracker& m_eventTracker;
    ContainerEventTracker::ContainerTrackingReference m_containerEvents;
    IORelay& m_ioRelay;
};

class DECLSPEC_UUID("B1F1C4E3-C225-4CAE-AD8A-34C004DE1AE4") WSLAContainer
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IWSLAContainer, IFastRundown>,
      public COMImplClass<WSLAContainerImpl>
{

public:
    WSLAContainer(WSLAContainerImpl* impl, std::function<void(const WSLAContainerImpl*)>&& OnDeleted);

    IFACEMETHOD(Attach)(_Out_ ULONG* Stdin, _Out_ ULONG* Stdout, _Out_ ULONG* Stderr) override;
    IFACEMETHOD(Stop)(_In_ WSLASignal Signal, _In_ LONGLONG TimeoutSeconds) override;
    IFACEMETHOD(Delete)() override;
    IFACEMETHOD(GetState)(_Out_ WSLA_CONTAINER_STATE* State) override;
    IFACEMETHOD(GetInitProcess)(_Out_ IWSLAProcess** process) override;
    IFACEMETHOD(Exec)(_In_ const WSLA_PROCESS_OPTIONS* Options, _Out_ IWSLAProcess** Process, _Out_ int* Errno) override;
    IFACEMETHOD(Start)(WSLAContainerStartFlags Flags) override;
    IFACEMETHOD(Inspect)(_Out_ LPSTR* Output) override;
    IFACEMETHOD(GetID)(_Out_ LPSTR* Output) override;
    IFACEMETHOD(Logs)(_In_ WSLALogsFlags Flags, _Out_ ULONG* Stdout, _Out_ ULONG* Stderr, _In_ ULONGLONG Since, _In_ ULONGLONG Until, _In_ ULONGLONG Tail) override;
    IFACEMETHOD(GetId)(_Out_ WSLAContainerId Id) override;
    IFACEMETHOD(GetName)(_Out_ LPSTR* Name) override;
    IFACEMETHOD(GetLabels)(_Out_ WSLA_LABEL_INFORMATION** Labels, _Out_ ULONG* Count) override;

private:
    std::function<void(const WSLAContainerImpl*)> m_onDeleted;
};
} // namespace wsl::windows::service::wsla