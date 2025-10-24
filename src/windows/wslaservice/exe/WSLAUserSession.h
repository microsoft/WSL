/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLAUserSession.h

Abstract:

    TODO

--*/
#pragma once
#include "WSLASession.h"
//#include "wslaservice.idl" // For WSLA_SESSION_CONFIGURATION and IWSLASession

namespace wsl::windows::service::wsla {

struct IWSLASession;

class WSLAUserSessionImpl
{
public:
    WSLAUserSessionImpl(HANDLE Token, wil::unique_tokeninfo_ptr<TOKEN_USER>&& TokenInfo);
    WSLAUserSessionImpl(WSLAUserSessionImpl&&) = default;
    WSLAUserSessionImpl& operator=(WSLAUserSessionImpl&&) = default;

    ~WSLAUserSessionImpl();

    PSID GetUserSid() const;

    HRESULT WSLACreateSession(const WSLA_SESSION_CONFIGURATION* WslaSessionConfig, IWSLASession** WslaSession);

    HRESULT WSLAReleaseSession(const IWSLASession* WslaSession);

    HRESULT WSLAListSessions(std::vector<IWSLASession*>& WslaSessions);

    void OnWslaSessionTerminated(WSLASession* WslaSession);

private:
    wil::unique_tokeninfo_ptr<TOKEN_USER> m_tokenInfo;

    std::recursive_mutex m_wslaSessionsLock;
    std::vector<WSLASession*> m_wslaSessions;
};

class DECLSPEC_UUID("a9b7a1b9-0671-405c-95f1-e0612cb4ce8f") WSLAUserSession
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IWSLAUserSession, IFastRundown>
{
public:
    WSLAUserSession(std::weak_ptr<WSLAUserSessionImpl>&& Session);
    WSLAUserSession(const WSLAUserSession&) = delete;
    WSLAUserSession& operator=(const WSLAUserSession&) = delete;

    IFACEMETHOD(GetVersion)(_Out_ WSL_VERSION* Version) override;
    IFACEMETHOD(WSLACreateSession)(const WSLA_SESSION_CONFIGURATION* WslaSessionConfig, IWSLASession** WslaSession) override;
    IFACEMETHOD(WSLAReleaseSession)(const IWSLASession* WslaSession) override;
    IFACEMETHOD(WSLAListSessions)(std::vector<WSLASession*>& Sessions) override;

private:
    std::weak_ptr<WSLAUserSessionImpl> m_session;
};

} // namespace wsl::windows::service::wsla
