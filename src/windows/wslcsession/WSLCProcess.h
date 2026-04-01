/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCProcess.h

Abstract:

    Contains the definition for WSLCProcess

--*/
#pragma once

#include "wslc.h"
#include "WSLCProcessControl.h"
#include "WSLCProcessIO.h"

namespace wsl::windows::service::wslc {

class WSLCVirtualMachine;

class DECLSPEC_UUID("AFBEA6D6-D8A4-4F81-8FED-F947EB74B33B") WSLCProcess
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IWSLCProcess, IFastRundown>
{
public:
    WSLCProcess(std::shared_ptr<WSLCProcessControl> Control, std::unique_ptr<WSLCProcessIO>&& Io, WSLCProcessFlags Flags);
    WSLCProcess(const WSLCProcess&) = delete;
    WSLCProcess& operator=(const WSLCProcess&) = delete;

    IFACEMETHOD(Signal)(_In_ int Signal) override;
    IFACEMETHOD(GetExitEvent)(_Out_ HANDLE* Event) override;
    IFACEMETHOD(GetStdHandle)(_In_ WSLCFD Fd, _Out_ WSLCHandle* Handle) override;
    IFACEMETHOD(GetFlags)(_Out_ WSLCProcessFlags* Flags) override;
    IFACEMETHOD(GetPid)(_Out_ int* Pid) override;
    IFACEMETHOD(GetState)(_Out_ WSLCProcessState* State, _Out_ int* Code) override;
    IFACEMETHOD(ResizeTty)(_In_ ULONG Rows, _In_ ULONG Columns) override;

    wil::unique_handle GetStdHandle(int Index);
    HANDLE GetExitEvent();
    int GetPid() const;

private:
    WSLCProcessFlags m_flags;
    std::shared_ptr<WSLCProcessControl> m_control;
    std::unique_ptr<WSLCProcessIO> m_io;
};
} // namespace wsl::windows::service::wslc