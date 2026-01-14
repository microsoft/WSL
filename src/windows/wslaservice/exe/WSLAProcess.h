/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLAProcess.h

Abstract:

    Contains the definition for WSLAProcess

--*/
#pragma once

#include "wslaservice.h"
#include "WSLAProcessControl.h"
#include "WSLAProcessIO.h"

namespace wsl::windows::service::wsla {

class WSLAVirtualMachine;

class DECLSPEC_UUID("AFBEA6D6-D8A4-4F81-8FED-F947EB74B33B") WSLAProcess
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IWSLAProcess, IFastRundown>
{
public:
    WSLAProcess(std::unique_ptr<WSLAProcessControl>&& Control, std::unique_ptr<WSLAProcessIO>&& Io);
    WSLAProcess(const WSLAProcess&) = delete;
    WSLAProcess& operator=(const WSLAProcess&) = delete;

    IFACEMETHOD(Signal)(_In_ int Signal) override;
    IFACEMETHOD(GetExitEvent)(_Out_ ULONG* Event) override;
    IFACEMETHOD(GetStdHandle)(_In_ ULONG Index, _Out_ ULONG* Handle) override;
    IFACEMETHOD(GetPid)(_Out_ int* Pid) override;
    IFACEMETHOD(GetState)(_Out_ WSLA_PROCESS_STATE* State, _Out_ int* Code) override;
    IFACEMETHOD(ResizeTty)(_In_ ULONG Rows, _In_ ULONG Columns) override;

    wil::unique_handle GetStdHandle(int Index);
    HANDLE GetExitEvent();
    int GetPid() const;

private:
    std::unique_ptr<WSLAProcessControl> m_control;
    std::unique_ptr<WSLAProcessIO> m_io;
};
} // namespace wsl::windows::service::wsla