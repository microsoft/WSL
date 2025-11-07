/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLAProcess.h

Abstract:

    Contains the definition for WSLAProcess

--*/
#pragma once

#include "wslaservice.h"

namespace wsl::windows::service::wsla {

class DECLSPEC_UUID("AFBEA6D6-D8A4-4F81-8FED-F947EB74B33B") WSLAProcess
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IWSLAProcess, IFastRundown>
{
public:
    WSLAProcess() = default; // TODO
    WSLAProcess(const WSLAProcess&) = delete;
    WSLAProcess& operator=(const WSLAProcess&) = delete;

    IFACEMETHOD(Signal)(_In_ int Signal) override;
    IFACEMETHOD(GetExitEvent)(_Out_ ULONG* Event) override;
    IFACEMETHOD(GetStdHandle)(_In_ ULONG Index, _Out_ ULONG* Handle) override;
    IFACEMETHOD(GetPid)(_Out_ int* Pid) override;
    IFACEMETHOD(GetState)(_Out_ WSLA_PROCESS_STATE* State, _Out_ int* Code) override;

private:
    std::vector<wil::unique_handle> m_handles;
    wil::unique_event m_exitEvent{wil::EventOptions::ManualReset};
};
} // namespace wsl::windows::service::wsla