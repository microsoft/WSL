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
        std::lock_guard lock(m_wslaSessionsLock);

        for (auto& e : m_sessions)
        {
            e->OnUserSessionTerminating();
        }
    }
}

void WSLAUserSessionImpl::OnSessionTerminated(WSLASession* Session)
{
    std::lock_guard lock(m_wslaSessionsLock);
    WI_VERIFY(m_sessions.erase(Session) == 1);
}

PSID WSLAUserSessionImpl::GetUserSid() const
{
    return m_tokenInfo->User.Sid;
}

HRESULT WSLAUserSessionImpl::CreateSession(const WSLA_SESSION_SETTINGS* Settings, IWSLASession** WslaSession)
{
    ULONG id = m_nextSessionId++;
    auto session = wil::MakeOrThrow<WSLASession>(id, *Settings, *this);

    std::lock_guard lock(m_wslaSessionsLock);
    auto it = m_sessions.emplace(session.Get());

    // Client now owns the session.
    // TODO: Add a flag for the client to specify that the session should outlive its process.

    THROW_IF_FAILED(session.CopyTo(__uuidof(IWSLASession), (void**)WslaSession));

    return S_OK;
}

HRESULT WSLAUserSessionImpl::OpenSessionByName(LPCWSTR DisplayName, IWSLASession** Session)
{
    std::lock_guard lock(m_wslaSessionsLock);

    // TODO: ACL check
    // TODO: Check for duplicate on session creation.
    for (auto& e : m_sessions)
    {
        if (e->DisplayName() == DisplayName)
        {
            THROW_IF_FAILED(e->QueryInterface(__uuidof(IWSLASession), (void**)Session));
            return S_OK;
        }
    }

    return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
}

HRESULT wsl::windows::service::wsla::WSLAUserSessionImpl::ListSessions(_Out_ WSLA_SESSION_INFORMATION** Sessions, _Out_ ULONG* SessionsCount)
{
    std::lock_guard lock(m_wslaSessionsLock);
    auto output = wil::make_unique_cotaskmem<WSLA_SESSION_INFORMATION[]>(m_sessions.size());

    size_t index = 0;
    for (auto* session : m_sessions)
    {
        output[index].SessionId = session->GetId();
        output[index].CreatorPid = 0; // placeholder until we populate this later

        session->CopyDisplayName(output[index].DisplayName, _countof(output[index].DisplayName));

        ++index;
    }
    *Sessions = output.release();
    *SessionsCount = static_cast<ULONG>(m_sessions.size());
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

HRESULT wsl::windows::service::wsla::WSLAUserSession::CreateSession(const WSLA_SESSION_SETTINGS* Settings, IWSLASession** WslaSession)
try
{
    auto session = m_session.lock();
    RETURN_HR_IF(RPC_E_DISCONNECTED, !session);

    return session->CreateSession(Settings, WslaSession);
}
CATCH_RETURN();

HRESULT wsl::windows::service::wsla::WSLAUserSession::ListSessions(WSLA_SESSION_INFORMATION** Sessions, ULONG* SessionsCount)
try
{
    if (!Sessions || !SessionsCount)
    {
        return E_INVALIDARG;
    }

    auto session = m_session.lock();
    RETURN_HR_IF(RPC_E_DISCONNECTED, !session);

    RETURN_IF_FAILED(session->ListSessions(Sessions, SessionsCount));
    return S_OK;
}
CATCH_RETURN();

HRESULT wsl::windows::service::wsla::WSLAUserSession::OpenSession(ULONG Id, IWSLASession** Session)
{
    return E_NOTIMPL;
}

HRESULT wsl::windows::service::wsla::WSLAUserSession::OpenSessionByName(LPCWSTR DisplayName, IWSLASession** Session)
try
{
    auto session = m_session.lock();
    RETURN_HR_IF(RPC_E_DISCONNECTED, !session);

    return session->OpenSessionByName(DisplayName, Session);
}
CATCH_RETURN();