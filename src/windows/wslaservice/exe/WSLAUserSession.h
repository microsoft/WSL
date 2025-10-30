/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLAUserSession.h

Abstract:

    TODO

--*/

#pragma once
#include "WSLAVirtualMachine.h"
#include "WSLASession.h"

namespace wsl::windows::service::wsla {

class WSLAUserSessionImpl
{
public:
    WSLAUserSessionImpl(HANDLE Token, wil::unique_tokeninfo_ptr<TOKEN_USER>&& TokenInfo);
    WSLAUserSessionImpl(WSLAUserSessionImpl&&) = default;
    WSLAUserSessionImpl& operator=(WSLAUserSessionImpl&&) = default;

    ~WSLAUserSessionImpl();

    PSID GetUserSid() const;

    HRESULT CreateVirtualMachine(const VIRTUAL_MACHINE_SETTINGS* Settings, IWSLAVirtualMachine** VirtualMachine);
    HRESULT CreateSession(const WSLA_SESSION_CONFIGURATION* WslaSessionConfig, const VIRTUAL_MACHINE_SETTINGS* VmSettings, IWSLASession** WslaSession);

    void OnVmTerminated(WSLAVirtualMachine* machine);

    /* HRESULT ReleaseSession(const IWSLASession* WslaSession);

    HRESULT ListSessions(std::vector<IWSLASession*>& WslaSessions); */

    void OnWslaSessionTerminated(WSLASession* WslaSession);

private:
    wil::unique_tokeninfo_ptr<TOKEN_USER> m_tokenInfo;

    std::recursive_mutex m_wslaSessionsLock;
    // TODO-WSLA: Consider using a weak_ptr to easily destroy when the last client reference is released.
    std::vector<Microsoft::WRL::ComPtr<WSLASession>> m_wslaSessions;
    std::recursive_mutex m_lock;
    std::vector<WSLAVirtualMachine*> m_virtualMachines;
};

class DECLSPEC_UUID("a9b7a1b9-0671-405c-95f1-e0612cb4ce8f") WSLAUserSession
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IWSLAUserSession, IFastRundown>
{
public:
    WSLAUserSession(std::weak_ptr<WSLAUserSessionImpl>&& Session);
    WSLAUserSession(const WSLAUserSession&) = delete;
    WSLAUserSession& operator=(const WSLAUserSession&) = delete;

    IFACEMETHOD(GetVersion)(_Out_ WSL_VERSION* Version) override;
    IFACEMETHOD(CreateVirtualMachine)(const VIRTUAL_MACHINE_SETTINGS* Settings, IWSLAVirtualMachine** VirtualMachine) override;
    IFACEMETHOD(CreateSession)(const WSLA_SESSION_CONFIGURATION* WslaSessionConfig, const VIRTUAL_MACHINE_SETTINGS* VmSettings, IWSLASession** WslaSession);
    //IFACEMETHOD(ReleaseSession)(_In_ const IWSLASession* WslaSession) override;
    //IFACEMETHOD(ListSessions)(_Out_ std::vector<WSLASession*>& Sessions) override;

private:
    std::weak_ptr<WSLAUserSessionImpl> m_session;
};

} // namespace wsl::windows::service::wsla
