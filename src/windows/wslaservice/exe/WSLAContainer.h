/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLAContainer.h

Abstract:

    Contains the definition for WSLAContainer.

--*/

#pragma once

#include "ServiceProcessLauncher.h"
#include "wslaservice.h"
#include "WSLAVirtualMachine.h"
#include "ContainerEventTracker.h"
#include "DockerHTTPClient.h"
#include "WSLAProcessControl.h"
#include "LogsRelay.h"
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
        std::function<void(const WSLAContainerImpl*)>&& OnDeleted,
        ContainerEventTracker& EventTracker,
        DockerHTTPClient& DockerClient,
        WSLA_CONTAINER_STATE InitialState,
        bool Tty);

    ~WSLAContainerImpl();

    void Start();

    void Stop(_In_ int Signal, _In_ ULONG TimeoutMs);
    void Delete();
    void GetState(_Out_ WSLA_CONTAINER_STATE* State);
    void GetInitProcess(_Out_ IWSLAProcess** process);
    void Exec(_In_ const WSLA_PROCESS_OPTIONS* Options, _Out_ IWSLAProcess** Process, _Out_ int* Errno);
    void Inspect(LPSTR* Output);
    void Logs(WSLALogsFlags Flags, ULONG* Stdout, ULONG* Stderr, ULONGLONG Since, ULONGLONG Until, ULONGLONG Tail);

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
        DockerHTTPClient& DockerClient);

    static std::unique_ptr<WSLAContainerImpl> Open(
        const common::docker_schema::ContainerInfo& DockerContainer,
        WSLAVirtualMachine& parentVM,
        std::function<void(const WSLAContainerImpl*)>&& OnDeleted,
        ContainerEventTracker& EventTracker,
        DockerHTTPClient& DockerClient);

private:
    void OnEvent(ContainerEvent event, std::optional<int> exitCode);
    void WaitForContainerEvent();

    std::recursive_mutex m_lock;
    std::string m_name;
    std::string m_image;
    std::string m_id;
    bool m_tty{}; // TODO: have a flag for this at the API level.
    std::vector<DockerExecProcessControl*> m_processes;
    DockerHTTPClient& m_dockerClient;
    WSLA_CONTAINER_STATE m_state = WslaContainerStateInvalid;
    WSLAVirtualMachine* m_parentVM = nullptr;
    std::vector<WSLAPortMapping> m_mappedPorts;
    std::vector<WSLAVolumeMount> m_mountedVolumes;
    Microsoft::WRL::ComPtr<WSLAContainer> m_comWrapper;
    Microsoft::WRL::ComPtr<WSLAProcess> m_initProcess;
    DockerContainerProcessControl* m_initProcessControl = nullptr;
    ContainerEventTracker& m_eventTracker;
    ContainerEventTracker::ContainerTrackingReference m_containerEvents;
    LogsRelay m_logsRelay;

    static std::pair<bool, bool> ParseFdStatus(const WSLA_PROCESS_OPTIONS& Options);
    static void AddEnvironmentVariables(std::vector<std::string>& args, const WSLA_PROCESS_OPTIONS& options);

    static void MountVolumes(std::vector<WSLAVolumeMount>& volumes, WSLAVirtualMachine& parentVM);
    static void UnmountVolumes(const std::vector<WSLAVolumeMount>& volumes, WSLAVirtualMachine& parentVM);
};

class DECLSPEC_UUID("B1F1C4E3-C225-4CAE-AD8A-34C004DE1AE4") WSLAContainer
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IWSLAContainer, IFastRundown>,
      public COMImplClass<WSLAContainerImpl>
{

public:
    WSLAContainer(WSLAContainerImpl* impl, std::function<void(const WSLAContainerImpl*)>&& OnDeleted);

    IFACEMETHOD(Stop)(_In_ int Signal, _In_ ULONG TimeoutMs) override;
    IFACEMETHOD(Delete)() override;
    IFACEMETHOD(GetState)(_Out_ WSLA_CONTAINER_STATE* State) override;
    IFACEMETHOD(GetInitProcess)(_Out_ IWSLAProcess** process) override;
    IFACEMETHOD(Exec)(_In_ const WSLA_PROCESS_OPTIONS* Options, _Out_ IWSLAProcess** Process, _Out_ int* Errno) override;
    IFACEMETHOD(Start)() override;
    IFACEMETHOD(Inspect)(_Out_ LPSTR* Output) override;
    IFACEMETHOD(Logs)(_In_ WSLALogsFlags Flags, _Out_ ULONG* Stdout, _Out_ ULONG* Stderr, _In_ ULONGLONG Since, _In_ ULONGLONG Until, _In_ ULONGLONG Tail) override;
    IFACEMETHOD(GetId)(_Out_ WSLAContainerId Id) override;
    IFACEMETHOD(GetName)(_Out_ LPSTR* Name) override;

private:
    std::function<void(const WSLAContainerImpl*)> m_onDeleted;
};
} // namespace wsl::windows::service::wsla