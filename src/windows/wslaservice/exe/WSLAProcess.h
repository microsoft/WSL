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

class WSLAVirtualMachine;

class DECLSPEC_UUID("AFBEA6D6-D8A4-4F81-8FED-F947EB74B33B") WSLAProcess
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IWSLAProcess, IFastRundown>
{
public:
    WSLAProcess(std::map<int, wil::unique_handle>&& handles, int pid, WSLAVirtualMachine* virtualMachine);
    ~WSLAProcess();
    WSLAProcess(const WSLAProcess&) = delete;
    WSLAProcess& operator=(const WSLAProcess&) = delete;

    IFACEMETHOD(Signal)(_In_ int Signal) override;
    IFACEMETHOD(GetExitEvent)(_Out_ ULONG* Event) override;
    IFACEMETHOD(GetStdHandle)(_In_ ULONG Index, _Out_ ULONG* Handle) override;
    IFACEMETHOD(GetPid)(_Out_ int* Pid) override;
    IFACEMETHOD(GetState)(_Out_ WSLA_PROCESS_STATE* State, _Out_ int* Code) override;

    void OnTerminated(bool Signalled, int Code);
    void OnVmTerminated();
    wil::unique_handle& GetStdHandle(int Index);
    wil::unique_event& GetExitEvent();
    int GetPid() const;

private:
    std::recursive_mutex m_mutex;
    std::map<int, wil::unique_handle> m_handles;
    int m_pid = -1;
    int m_exitedCode = -1;
    WSLA_PROCESS_STATE m_state = WslaProcessStateRunning;
    wil::unique_event m_exitEvent{wil::EventOptions::ManualReset};
    WSLAVirtualMachine* m_virtualMachine{};
};
} // namespace wsl::windows::service::wsla