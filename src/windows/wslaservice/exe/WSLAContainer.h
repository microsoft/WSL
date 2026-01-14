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

namespace wsl::windows::service::wsla {

struct VolumeMountInfo
{
    std::wstring HostPath;
    std::string ParentVMPath;
    std::string ContainerPath;
    bool ReadOnly;
};

class WSLAContainer;

class WSLAContainerImpl
{
public:
    struct PortMapping
    {
        uint16_t HostPort;
        uint16_t VmPort;
        uint16_t ContainerPort;
        int Family;
        bool MappedToHost = false;
    };

    NON_COPYABLE(WSLAContainerImpl);
    NON_MOVABLE(WSLAContainerImpl);

    WSLAContainerImpl(
        WSLAVirtualMachine* parentVM,
        const WSLA_CONTAINER_OPTIONS& Options,
        std::string&& Id,
        std::vector<VolumeMountInfo>&& volumes,
        std::vector<PortMapping>&& ports,
        std::function<void(const WSLAContainerImpl*)>&& OnDeleted,
        ContainerEventTracker& EventTracker,
        DockerHTTPClient& DockerClient);
    ~WSLAContainerImpl();

    void Start();

    void Stop(_In_ int Signal, _In_ ULONG TimeoutMs);
    void Delete();
    void GetState(_Out_ WSLA_CONTAINER_STATE* State);
    void GetInitProcess(_Out_ IWSLAProcess** process);
    void Exec(_In_ const WSLA_PROCESS_OPTIONS* Options, _Out_ IWSLAProcess** Process, _Out_ int* Errno);
    void Inspect(LPSTR* Output);

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
    std::vector<PortMapping> m_mappedPorts;
    std::vector<VolumeMountInfo> m_mountedVolumes;
    Microsoft::WRL::ComPtr<WSLAContainer> m_comWrapper;
    Microsoft::WRL::ComPtr<WSLAProcess> m_initProcess;
    DockerContainerProcessControl* m_initProcessControl = nullptr;
    ContainerEventTracker& m_eventTracker;
    ContainerEventTracker::ContainerTrackingReference m_containerEvents;

    static std::pair<bool, bool> ParseFdStatus(const WSLA_PROCESS_OPTIONS& Options);
    static void AddEnvironmentVariables(std::vector<std::string>& args, const WSLA_PROCESS_OPTIONS& options);

    static std::vector<VolumeMountInfo> MountVolumes(const WSLA_CONTAINER_OPTIONS& Options, WSLAVirtualMachine& parentVM);
    static void UnmountVolumes(const std::vector<VolumeMountInfo>& volumes, WSLAVirtualMachine& parentVM);
};

class DECLSPEC_UUID("B1F1C4E3-C225-4CAE-AD8A-34C004DE1AE4") WSLAContainer
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IWSLAContainer, IFastRundown>
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

    void Disconnect() noexcept;

private:
    template <typename... Args>
    HRESULT CallImpl(void (WSLAContainerImpl::*routine)(Args... args), Args... args)
    try
    {
        std::lock_guard lock{m_lock};
        RETURN_HR_IF(RPC_E_DISCONNECTED, m_impl == nullptr);

        (m_impl->*routine)(std::forward<Args>(args)...);

        return S_OK;
    }
    CATCH_RETURN();

    WSLAContainerImpl* m_impl = nullptr;
    std::function<void(const WSLAContainerImpl*)> m_onDeleted;
    std::recursive_mutex m_lock;
};
} // namespace wsl::windows::service::wsla