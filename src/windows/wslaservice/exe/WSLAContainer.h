/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLAContainer.h

Abstract:

    Contains the definition for WSLAContainer.

--*/

#pragma once

#include "wslaservice.h"

namespace wsl::windows::service::wsla {

class DECLSPEC_UUID("B1F1C4E3-C225-4CAE-AD8A-34C004DE1AE4") WSLAContainer
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IWSLAContainer, IFastRundown>
{
public:
    WSLAContainer() = default; // TODO
    WSLAContainer(const WSLAContainer&) = delete;
    WSLAContainer& operator=(const WSLAContainer&) = delete;

    IFACEMETHOD(Start)() override;
    IFACEMETHOD(Stop)(_In_ int Signal, _In_ ULONG TimeoutMs) override;
    IFACEMETHOD(Delete)() override;
    IFACEMETHOD(GetState)(_Out_ WSLA_CONTAINER_STATE* State) override;
    IFACEMETHOD(GetInitProcess)(_Out_ IWSLAProcess** process) override;
    IFACEMETHOD(Exec)(_In_ const WSLA_PROCESS_OPTIONS* Options, _Out_ IWSLAProcess** Process, _Out_ int* Errno) override;

private:
};
} // namespace wsl::windows::service::wsla