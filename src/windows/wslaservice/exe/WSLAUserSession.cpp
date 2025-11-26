/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLAUserSession.cpp

Abstract:

    TODO

--*/

#include "WSLAUserSession.h"
#include "WSLASession.h"

using wsl::windows::service::wsla::WSLAUserSessionImpl;

WSLAUserSessionImpl::WSLAUserSessionImpl(HANDLE Token, wil::unique_tokeninfo_ptr<TOKEN_USER>&& TokenInfo) :
    m_tokenInfo(std::move(TokenInfo))
{
}

WSLAUserSessionImpl::~WSLAUserSessionImpl()
{
    // In case there are still COM references on sessions, signal that the user session is terminating
    // so the sessions are all in a 'terminated' state.
    {
        std::lock_guard lock(m_lock);

        for (auto& e : m_sessions)
        {
            e->OnUserSessionTerminating();
        }
    }
}

void WSLAUserSessionImpl::OnSessionTerminated(WSLASession* Session)
{
    std::lock_guard lock(m_lock);
    WI_VERIFY(m_sessions.erase(Session) == 1);
}

PSID WSLAUserSessionImpl::GetUserSid() const
{
    return m_tokenInfo->User.Sid;
}

HRESULT wsl::windows::service::wsla::WSLAUserSessionImpl::CreateSession(
    const WSLA_SESSION_SETTINGS* Settings, const VIRTUAL_MACHINE_SETTINGS* VmSettings, IWSLASession** WslaSession)
{
    ULONG id = m_nextSessionId++;
    auto session = wil::MakeOrThrow<WSLASession>(id, *Settings, *this, *VmSettings);

    {
        std::lock_guard lock(m_wslaSessionsLock);
        auto it = m_sessions.emplace(session.Get());
        m_wslaSessions.emplace_back(session);
    // Client now owns the session.
    // TODO: Add a flag for the client to specify that the session should outlive its process.
    }
   
    THROW_IF_FAILED(session.CopyTo(__uuidof(IWSLASession), (void**)WslaSession));

    return S_OK;
}

HRESULT wsl::windows::service::wsla::WSLAUserSessionImpl::ListSessions(_Out_ WSLA_SESSION_INFORMATION** Sessions, _Out_ ULONG* SessionsCount)
{
    auto output = wil::make_unique_cotaskmem<WSLA_SESSION_INFORMATION[]>(m_wslaSessions.size());
    std::lock_guard lock(m_wslaSessionsLock);
    for (size_t i = 0; i < m_wslaSessions.size(); ++i)
    {
        output[i].SessionId = m_wslaSessions[i]->GetId();
        m_wslaSessions[i]->GetDisplayName(&output[i].DisplayName);
    }
    *Sessions = output.release();
    *SessionsCount = static_cast<ULONG>(m_wslaSessions.size());
    return S_OK;
}

wsl::windows::service::wsla::WSLAUserSession::WSLAUserSession(std::weak_ptr<WSLAUserSessionImpl>&& Session) :
    m_session(std::move(Session))
{
}

HRESULT wsl::windows::service::wsla::WSLAUserSession::GetVersion(_Out_ WSLA_VERSION* Version)
{
    Version->Major = WSL_PACKAGE_VERSION_MAJOR;
    Version->Minor = WSL_PACKAGE_VERSION_MINOR;
    Version->Revision = WSL_PACKAGE_VERSION_REVISION;

    return S_OK;
}

HRESULT wsl::windows::service::wsla::WSLAUserSession::CreateSession(
    const WSLA_SESSION_SETTINGS* Settings, const VIRTUAL_MACHINE_SETTINGS* VmSettings, IWSLASession** WslaSession)
try
{
    auto session = m_session.lock();
    RETURN_HR_IF(RPC_E_DISCONNECTED, !session);

    return session->CreateSession(Settings, VmSettings, WslaSession);
}
CATCH_RETURN();

HRESULT wsl::windows::service::wsla::WSLAUserSession::ListSessions(WSLA_SESSION_INFORMATION** Sessions, ULONG* SessionsCount)

{
    if (!Sessions || !SessionsCount)
    {
        return E_INVALIDARG;
    }

    // For now, return an empty list. We'll populate this from m_sessions later.
    auto session = m_session.lock();
    RETURN_HR_IF(RPC_E_DISCONNECTED, !session);

    return session->ListSessions(Sessions, SessionsCount);
}

HRESULT wsl::windows::service::wsla::WSLAUserSession::OpenSession(ULONG Id, IWSLASession** Session)
{
    return E_NOTIMPL;
}
