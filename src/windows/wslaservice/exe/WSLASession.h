/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLASession.h

Abstract:

    TODO

--*/

#pragma once

#include "wslaservice.h"
#include "WSLAVirtualMachine.h"

namespace wsl::windows::service::wsla {

class DECLSPEC_UUID("4877FEFC-4977-4929-A958-9F36AA1892A4") WSLASession
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IWSLASession, IFastRundown>
{
public:
    WSLASession(const WSLA_SESSION_SETTINGS& Settings, WSLAUserSessionImpl& userSessionImpl, const VIRTUAL_MACHINE_SETTINGS& VmSettings);
    IFACEMETHOD(GetDisplayName)(LPWSTR* DisplayName);
    IFACEMETHOD(GetVirtualMachine)(IWSLAVirtualMachine** VirtualMachine);

private:
    WSLA_SESSION_SETTINGS m_sessionSettings; // TODO: Revisit to see if we should have session settings as a member or not
    WSLAUserSessionImpl& m_userSession;
    WSLAVirtualMachine m_virtualMachine;
    std::wstring m_displayName;
};

} // namespace wsl::windows::service::wsla