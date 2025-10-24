/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLAUserSession.cpp

Abstract:

    TODO

--*/
#include "WSLAUserSession.h"

using wsl::windows::service::wsla::WSLAUserSessionImpl;

WSLAUserSessionImpl::WSLAUserSessionImpl(HANDLE Token, wil::unique_tokeninfo_ptr<TOKEN_USER>&& TokenInfo) :
    m_tokenInfo(std::move(TokenInfo))
{
}

WSLAUserSessionImpl::~WSLAUserSessionImpl()
{
    // Manually signal the VM termination events. This prevents being stuck on an API call that holds the VM lock.
    {
        std::lock_guard lock(m_wslaSessionsLock);

        for (auto* e : m_wslaSessions)
        {
            e->OnSessionTerminating();
        }
    }
}

void WSLAUserSessionImpl::OnWslaSessionTerminated(WSLASession* session)
{
    std::lock_guard lock(m_wslaSessionsLock);
    auto pred = [session](const auto* e) { return session == e; };

    // Remove any stale session reference.
    m_wslaSessions.erase(std::remove_if(m_wslaSessions.begin(), m_wslaSessions.end(), pred), m_wslaSessions.end());
}

HRESULT WSLAUserSessionImpl::WSLACreateSession(const WSLA_SESSION_CONFIGURATION* Settings, IWSLASession** WslaSession)
{
    auto session = wil::MakeOrThrow<WSLASession>(*Settings, GetUserSid(), this);

    {
        std::lock_guard lock(m_wslaSessionsLock);
        m_wslaSessions.emplace_back(session.Get());
    }

    session->Start();
    THROW_IF_FAILED(session.CopyTo(__uuidof(IWSLASession), (void**)WslaSession));

    return S_OK;
}

PSID WSLAUserSessionImpl::GetUserSid() const
{
    return m_tokenInfo->User.Sid;
}

wsl::windows::service::wsla::WSLAUserSession::WSLAUserSession(std::weak_ptr<WSLAUserSessionImpl>&& Session) :
    m_session(std::move(Session))
{
}

HRESULT wsl::windows::service::wsla::WSLAUserSession::GetVersion(_Out_ WSL_VERSION* Version)
{
    Version->Major = WSL_PACKAGE_VERSION_MAJOR;
    Version->Minor = WSL_PACKAGE_VERSION_MINOR;
    Version->Revision = WSL_PACKAGE_VERSION_REVISION;

    return S_OK;
}

HRESULT wsl::windows::service::wsla::WSLAUserSession::WSLACreateSession(const WSLA_SESSION_CONFIGURATION* Settings, IWSLASession** WslaSession)
try
{
    auto session = m_session.lock();
    RETURN_HR_IF(RPC_E_DISCONNECTED, !session);

    return session->WSLACreateSession(Settings, WslaSession);
}
CATCH_RETURN();