/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLAUserSession.cpp

Abstract:

    Implementation for WSLASessionManager.

--*/

#include "WSLASessionManager.h"
#include "WSLASession.h"

using wsl::windows::service::wsla::WSLASessionManager;

WSLASessionManager::WSLASessionManager()
{
}

WSLASessionManager::~WSLASessionManager()
{
    // In case there are still COM references on sessions, signal that the user session is terminating
    // so the sessions are all in a 'terminated' state.
    ForEachSession<void>([](WSLASession& e) { e.OnUserSessionTerminating(); });
}

HRESULT WSLASessionManager::CreateSession(const WSLA_SESSION_SETTINGS* Settings, WSLASessionFlags Flags, IWSLASession** WslaSession)
try
{
    auto tokenInfo = GetCallingProcessTokenInfo();

    std::lock_guard lock(m_wslaSessionsLock);

    // Check for an existing session first.
    auto result = ForEachSession<HRESULT>([&](auto& session) -> std::optional<HRESULT> {
        if (session.DisplayName() == Settings->DisplayName)
        {
            RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), WI_IsFlagClear(Flags, WSLASessionFlagsOpenExisting));

            THROW_IF_FAILED(CheckTokenAccess(session, tokenInfo));
            return session.QueryInterface(__uuidof(IWSLASession), (void**)WslaSession);
        }

        return std::optional<HRESULT>{};
    });

    if (result.has_value())
    {
        return result.value();
    }

    // No session was found, create a new one.
    auto session = wil::MakeOrThrow<WSLASession>(m_nextSessionId++, *Settings, *this);

    if (WI_IsFlagSet(Flags, WSLASessionFlagsPersistent))
    {
        m_persistentSessions.push_back(session);
    }

    Microsoft::WRL::ComPtr<IWeakReference> weakRef;
    THROW_IF_FAILED(session->GetWeakReference(&weakRef));

    m_sessions.emplace_back(std::move(weakRef));

    THROW_IF_FAILED(session.CopyTo(__uuidof(IWSLASession), (void**)WslaSession));

    return S_OK;
}
CATCH_RETURN();

HRESULT WSLASessionManager::OpenSessionByName(LPCWSTR DisplayName, IWSLASession** Session)
{
    auto tokenInfo = GetCallingProcessTokenInfo();

    auto result = ForEachSession<HRESULT>([&](auto& e) {
        if (e.DisplayName() == DisplayName)
        {
            THROW_IF_FAILED(CheckTokenAccess(e, tokenInfo));
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

HRESULT WSLASessionManager::ListSessions(_Out_ WSLA_SESSION_INFORMATION** Sessions, _Out_ ULONG* SessionsCount)
{
    std::vector<WSLA_SESSION_INFORMATION> sessionInfo;

    ForEachSession<void>([&](const auto& session) {
        auto& it = sessionInfo.emplace_back(WSLA_SESSION_INFORMATION{.SessionId = session.GetId(), .CreatorPid = session.GetCreatorPid()});
        
        wcscpy_s(it.Sid, _countof(it.Sid), session.GetSidString().get());
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

HRESULT WSLASessionManager::GetVersion(_Out_ WSLA_VERSION* Version)
{
    Version->Major = WSL_PACKAGE_VERSION_MAJOR;
    Version->Minor = WSL_PACKAGE_VERSION_MINOR;
    Version->Revision = WSL_PACKAGE_VERSION_REVISION;

    return S_OK;
}

WSLASessionManager::CallingProcessTokenInfo WSLASessionManager::GetCallingProcessTokenInfo()
{
    const wil::unique_handle userToken = wsl::windows::common::security::GetUserToken(TokenImpersonation);

    auto tokenInfo = wil::get_token_information<TOKEN_USER>(userToken.get());

    auto elevated = wil::test_token_membership(userToken.get(), SECURITY_NT_AUTHORITY, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS);

    return {std::move(tokenInfo), elevated};
}

HRESULT WSLASessionManager::CheckTokenAccess(const WSLASession& Session, const CallingProcessTokenInfo& TokenInfo)
{
    // Allow elevated tokens to access all sessions.
    // Otherwise a token can only access sessions from the same SID and elevation status.
    // TODO: Offer proper ACL checks.

    if (TokenInfo.Elevated)
    {
        return S_OK; // Token is elevated, allow access.
    }

    if (!EqualSid(Session.GetSid(), TokenInfo.TokenUser->User.Sid))
    {
        return E_ACCESSDENIED; // Different account, deny access.
    }

    if (!TokenInfo.Elevated && Session.IsTokenElevated())
    {
        return HRESULT_FROM_WIN32(ERROR_ELEVATION_REQUIRED); // Non-elevated token trying to access elevated session, deny access.
    }

    return S_OK;
}