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

namespace wsl::windows::service::wsla {

class DECLSPEC_UUID("B1F1C4E3-C225-4CAE-AD8A-34C004DE1AE4") WSLAContainer
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IWSLAContainer, IFastRundown>
{
public:
    WSLAContainer(WSLAVirtualMachine* parentVM, ServiceRunningProcess&& containerProcess, const char* name, const char* image);
    ~WSLAContainer();

    NON_COPYABLE(WSLAContainer);

    IFACEMETHOD(Start)() override;
    IFACEMETHOD(Stop)(_In_ int Signal, _In_ ULONG TimeoutMs) override;
    IFACEMETHOD(Delete)() override;
    IFACEMETHOD(GetState)(_Out_ WSLA_CONTAINER_STATE* State) override;
    IFACEMETHOD(GetInitProcess)(_Out_ IWSLAProcess** process) override;
    IFACEMETHOD(Exec)(_In_ const WSLA_PROCESS_OPTIONS* Options, _Out_ IWSLAProcess** Process, _Out_ int* Errno) override;

    void GetName(char Name[WSLA_MAX_IMAGE_NAME_LENGTH + 1]) const noexcept;
    void GetImage(char Name[WSLA_MAX_CONTAINER_NAME_LENGTH + 1]) const noexcept;

    static Microsoft::WRL::ComPtr<WSLAContainer> Create(const WSLA_CONTAINER_OPTIONS& Options, WSLAVirtualMachine& parentVM);

private:
    ServiceRunningProcess m_containerProcess;
    std::string m_name;
    std::string m_image;
    WSLAVirtualMachine* m_parentVM = nullptr;

    static std::vector<std::string> PrepareNerdctlRunCommand(const WSLA_CONTAINER_OPTIONS& options, std::vector<std::string>&& inputOptions);
};
} // namespace wsl::windows::service::wsla