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

class DECLSPEC_UUID("B1F1C4E3-C225-4CAE-AD8A-34C004DE1AE4") WSLAContainer
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IWSLAContainer, IFastRundown>
{
public:
    NON_COPYABLE(WSLAContainer);

    WSLAContainer(WSLAVirtualMachine* parentVM, const WSLA_CONTAINER_OPTIONS& Options, std::string&& Id, ContainerEventTracker& tracker);
    ~WSLAContainer();

    void Start(const WSLA_CONTAINER_OPTIONS& Options);

    IFACEMETHOD(Stop)(_In_ int Signal, _In_ ULONG TimeoutMs) override;
    IFACEMETHOD(Delete)() override;
    IFACEMETHOD(GetState)(_Out_ WSLA_CONTAINER_STATE* State) override;
    IFACEMETHOD(GetInitProcess)(_Out_ IWSLAProcess** process) override;
    IFACEMETHOD(Exec)(_In_ const WSLA_PROCESS_OPTIONS* Options, _Out_ IWSLAProcess** Process, _Out_ int* Errno) override;

    const std::string& Image() const noexcept;
    WSLA_CONTAINER_STATE State() noexcept;

    static Microsoft::WRL::ComPtr<WSLAContainer> Create(const WSLA_CONTAINER_OPTIONS& Options, WSLAVirtualMachine& parentVM, ContainerEventTracker& tracker);

private:
    void OnEvent(ContainerEvent event);
    void WaitForContainerEvent();

    std::optional<std::string> GetNerdctlStatus();

    std::mutex m_lock;
    wil::unique_event m_startedEvent{wil::EventOptions::ManualReset};
    std::optional<ServiceRunningProcess> m_containerProcess;
    std::string m_name;
    std::string m_image;
    std::string m_id;
    WSLA_CONTAINER_STATE m_state = WslaContainerStateInvalid;
    WSLAVirtualMachine* m_parentVM = nullptr;
    std::recursive_mutex m_lock;
    ContainerEventTracker::ContainerTrackingReference m_trackingReference;

    static std::vector<std::string> PrepareNerdctlCreateCommand(const WSLA_CONTAINER_OPTIONS& options, std::vector<std::string>&& inputOptions);
};
} // namespace wsl::windows::service::wsla