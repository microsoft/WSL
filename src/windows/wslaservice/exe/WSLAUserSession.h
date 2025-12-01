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
#include <atomic>
#include <vector>

namespace wsl::windows::service::wsla {

class WSLAUserSessionImpl
{
public:
    WSLAUserSessionImpl(HANDLE Token, wil::unique_tokeninfo_ptr<TOKEN_USER>&& TokenInfo);
    WSLAUserSessionImpl(WSLAUserSessionImpl&&) = default;
    WSLAUserSessionImpl& operator=(WSLAUserSessionImpl&&) = default;

    ~WSLAUserSessionImpl();

    PSID GetUserSid() const;

    HRESULT CreateSession(const WSLA_SESSION_SETTINGS* Settings, const VIRTUAL_MACHINE_SETTINGS* VmSettings, IWSLASession** WslaSession);
    HRESULT ListSessions(_Out_ WSLA_SESSION_INFORMATION** Sessions, _Out_ ULONG* SessionsCount);
    void OnVmTerminated(WSLAVirtualMachine* machine);
    void OnSessionTerminated(WSLASession* Session);

private:
    wil::unique_tokeninfo_ptr<TOKEN_USER> m_tokenInfo;
    std::atomic<ULONG> m_nextSessionId{1};
    std::recursive_mutex m_wslaSessionsLock;
    std::recursive_mutex m_lock;
    // Track active sessions for diagnostics / ListSessions.
    std::vector<Microsoft::WRL::ComPtr<WSLASession>> m_wslaSessions;

    // TODO-WSLA: Consider using a weak_ptr to easily destroy when the last client reference is released.
    std::unordered_set<WSLASession*> m_sessions;
};

class DECLSPEC_UUID("a9b7a1b9-0671-405c-95f1-e0612cb4ce8f") WSLAUserSession
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IWSLAUserSession, IFastRundown>
{
public:
    WSLAUserSession(std::weak_ptr<WSLAUserSessionImpl>&& Session);
    WSLAUserSession(const WSLAUserSession&) = delete;
    WSLAUserSession& operator=(const WSLAUserSession&) = delete;

    IFACEMETHOD(GetVersion)(_Out_ WSLA_VERSION* Version) override;
    IFACEMETHOD(CreateSession)(const WSLA_SESSION_SETTINGS* WslaSessionSettings, const VIRTUAL_MACHINE_SETTINGS* VmSettings, IWSLASession** WslaSession) override;
    IFACEMETHOD(ListSessions)(_Out_ WSLA_SESSION_INFORMATION** Sessions, _Out_ ULONG* SessionsCount) override;
    IFACEMETHOD(OpenSession)(_In_ ULONG Id, _Out_ IWSLASession** Session) override;

private:
    std::weak_ptr<WSLAUserSessionImpl> m_session;
};

} // namespace wsl::windows::service::wsla
