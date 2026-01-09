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
    ForEachSession<void>([](WSLASession& e) { e.OnUserSessionTerminating(); });
}

PSID WSLAUserSessionImpl::GetUserSid() const
{
    return m_tokenInfo->User.Sid;
}

HRESULT WSLAUserSessionImpl::CreateSession(const WSLA_SESSION_SETTINGS* Settings, IWSLASession** WslaSession)
{
    ULONG id = m_nextSessionId++;
    auto session = wil::MakeOrThrow<WSLASession>(id, *Settings, *this);

    Microsoft::WRL::ComPtr<IWeakReference> weakRef;
    THROW_IF_FAILED(session->GetWeakReference(&weakRef));

    {
        std::lock_guard lock(m_wslaSessionsLock);
        m_sessions.emplace_back(std::move(weakRef));
    }

    // Client now owns the session.
    // TODO: Add a flag for the client to specify that the session should outlive its process.

    THROW_IF_FAILED(session.CopyTo(__uuidof(IWSLASession), (void**)WslaSession));

    return S_OK;
}

HRESULT WSLAUserSessionImpl::OpenSessionByName(LPCWSTR DisplayName, IWSLASession** Session)
{
    // TODO: ACL check
    // TODO: Check for duplicate on session creation.

    auto result = ForEachSession<HRESULT>([&](auto& e) {
        if (e.DisplayName() == DisplayName)
        {
            THROW_IF_FAILED(e.QueryInterface(__uuidof(IWSLASession), (void**)Session));
            return std::make_optional(S_OK);
        }
        else
        {
            return std::optional<HRESULT>{};
        }
    });

    return result.value_or(HRESULT_FROM_WIN32(ERROR_NOT_FOUND));
}

HRESULT wsl::windows::service::wsla::WSLAUserSessionImpl::ListSessions(_Out_ WSLA_SESSION_INFORMATION** Sessions, _Out_ ULONG* SessionsCount)
{
    std::vector<WSLA_SESSION_INFORMATION> sessionInfo;

    ForEachSession<void>([&](const auto& session) {
        auto& it = sessionInfo.emplace_back(WSLA_SESSION_INFORMATION{
            .SessionId = session.GetId(),
            .CreatorPid = 0,
        });

        session.CopyDisplayName(it.DisplayName, _countof(it.DisplayName));
    });

    auto output = wil::make_unique_cotaskmem<WSLA_SESSION_INFORMATION[]>(sessionInfo.size());
    for (auto i = 0; i < sessionInfo.size(); i++)
    {
        memcpy(&output[i], &sessionInfo[i], sizeof(WSLA_SESSION_INFORMATION));
    }

    *Sessions = output.release();
    *SessionsCount = static_cast<ULONG>(sessionInfo.size());
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