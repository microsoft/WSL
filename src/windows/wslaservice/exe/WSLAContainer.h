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

namespace wsl::windows::service::wsla {

struct VolumeMountInfo
{
    std::wstring HostPath;
    std::string ParentVMPath;
    std::string ContainerPath;
    BOOL ReadOnly;
};

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

    WSLAContainerImpl(
        WSLAVirtualMachine* parentVM,
        const WSLA_CONTAINER_OPTIONS& Options,
        std::string&& Id,
        ContainerEventTracker& tracker,
        std::vector<VolumeMountInfo>&& volumes,
        std::vector<PortMapping>&& ports);
    ~WSLAContainerImpl();

    void Start(const WSLA_CONTAINER_OPTIONS& Options);

    void Stop(_In_ int Signal, _In_ ULONG TimeoutMs);
    void Delete();
    void GetState(_Out_ WSLA_CONTAINER_STATE* State);
    void GetInitProcess(_Out_ IWSLAProcess** process);
    void Exec(_In_ const WSLA_PROCESS_OPTIONS* Options, _Out_ IWSLAProcess** Process, _Out_ int* Errno);

    const std::string& Image() const noexcept;
    WSLA_CONTAINER_STATE State() noexcept;

    static std::shared_ptr<WSLAContainerImpl> Create(const WSLA_CONTAINER_OPTIONS& Options, WSLAVirtualMachine& parentVM, ContainerEventTracker& tracker);

private:
    void OnEvent(ContainerEvent event);
    void WaitForContainerEvent();

    std::optional<std::string> GetNerdctlStatus();

    std::recursive_mutex m_lock;
    wil::unique_event m_startedEvent{wil::EventOptions::ManualReset};
    std::optional<ServiceRunningProcess> m_containerProcess;
    std::string m_name;
    std::string m_image;
    std::string m_id;
    WSLA_CONTAINER_STATE m_state = WslaContainerStateInvalid;
    WSLAVirtualMachine* m_parentVM = nullptr;
    ContainerEventTracker::ContainerTrackingReference m_trackingReference;
    std::vector<PortMapping> m_mappedPorts;
    std::vector<VolumeMountInfo> m_mountedVolumes;

    static std::vector<std::string> PrepareNerdctlCreateCommand(
        const WSLA_CONTAINER_OPTIONS& options, std::vector<std::string>&& inputOptions, std::vector<VolumeMountInfo>& volumes);
    static std::pair<bool, bool> ParseFdStatus(const WSLA_PROCESS_OPTIONS& Options);
    static void AddEnvironmentVariables(std::vector<std::string>& args, const WSLA_PROCESS_OPTIONS& options);

    static std::vector<VolumeMountInfo> MountVolumes(const WSLA_CONTAINER_OPTIONS& Options, WSLAVirtualMachine& parentVM);
    static void UnmountVolumes(const std::vector<VolumeMountInfo>& volumes, WSLAVirtualMachine& parentVM);
};

class DECLSPEC_UUID("B1F1C4E3-C225-4CAE-AD8A-34C004DE1AE4") WSLAContainer
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IWSLAContainer, IFastRundown>
{

public:
    WSLAContainer(std::weak_ptr<WSLAContainerImpl>&& impl);

    IFACEMETHOD(Stop)(_In_ int Signal, _In_ ULONG TimeoutMs) override;
    IFACEMETHOD(Delete)() override;
    IFACEMETHOD(GetState)(_Out_ WSLA_CONTAINER_STATE* State) override;
    IFACEMETHOD(GetInitProcess)(_Out_ IWSLAProcess** process) override;
    IFACEMETHOD(Exec)(_In_ const WSLA_PROCESS_OPTIONS* Options, _Out_ IWSLAProcess** Process, _Out_ int* Errno) override;

private:
    template <typename... Args>
    HRESULT CallImpl(void (WSLAContainerImpl::*routine)(Args... args), Args... args)
    try
    {
        auto impl = m_impl.lock();
        RETURN_HR_IF(RPC_E_DISCONNECTED, !impl);

        (impl.get()->*routine)(std::forward<Args>(args)...);

        return S_OK;
    }
    CATCH_RETURN();

    std::weak_ptr<WSLAContainerImpl> m_impl;
};
} // namespace wsl::windows::service::wsla