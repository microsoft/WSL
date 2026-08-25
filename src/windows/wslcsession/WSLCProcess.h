/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCProcess.h

Abstract:

    Contains the definition for WSLCProcess

--*/
#pragma once

#include "wslc.h"
#include "WSLCCompat.h"
#include "WSLCProcessControl.h"
#include "WSLCProcessIO.h"

namespace wsl::windows::service::wslc {

class WSLCVirtualMachine;

class DECLSPEC_UUID("AFBEA6D6-D8A4-4F81-8FED-F947EB74B33B") WSLCProcess
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IWSLCProcess, IWSLCCompatProcess, IFastRundown>
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

    // IWSLCCompatProcess - converts the WSLCCompat types to the wslc.idl types and forwards to the methods above.
    IFACEMETHOD(GetStdHandle)(_In_ WSLCFD Fd, _Out_ WSLCCompatHandle* Handle) override;

    wil::unique_handle GetStdHandle(int Index);
    HANDLE GetExitEvent();
    int GetPid() const;

    // Attaches an opaque keep-alive token whose lifetime is bound to this process object. A
    // root-namespace process is not tracked as a container, so it relies on this token to hold an
    // activity reference on the owning session for as long as the client keeps the process alive,
    // preventing the idle worker from tearing the VM down (and killing the process) underneath it.
    void SetKeepAliveToken(Microsoft::WRL::ComPtr<IUnknown>&& Token) noexcept
    {
        m_keepAliveToken = std::move(Token);
    }

private:
    WSLCProcessFlags m_flags;
    std::shared_ptr<WSLCProcessControl> m_control;
    std::unique_ptr<WSLCProcessIO> m_io;
    Microsoft::WRL::ComPtr<IUnknown> m_keepAliveToken;
};
} // namespace wsl::windows::service::wslc