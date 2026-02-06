/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLASessionReference.cpp

Abstract:

    Implementation for WSLASessionReference.

    This class provides a weak reference to a session that the SYSTEM service
    can use to:
    - Check if a session is still alive (OpenSession fails if session is gone)
    - Get session metadata for enumeration without holding a strong ref
    - Terminate sessions when requested by elevated callers

--*/

#include "WSLASessionReference.h"
#include "WSLASession.h"
#include "wslutil.h"

namespace wslutil = wsl::windows::common::wslutil;
namespace wsla = wsl::windows::service::wsla;

wsla::WSLASessionReference::WSLASessionReference(_In_ WSLASession* Session) :
    m_sessionId(Session->GetId()),
    m_creatorPid(Session->GetCreatorPid()),
    m_displayName(Session->DisplayName()),
    m_tokenInfo(wil::get_token_information<TOKEN_USER>(GetCurrentProcessToken())),
    m_elevated(Session->IsTokenElevated())
{
    Microsoft::WRL::ComPtr<IWeakReferenceSource> weakRefSource;
    THROW_IF_FAILED(Session->QueryInterface(IID_PPV_ARGS(&weakRefSource)));
    THROW_IF_FAILED(weakRefSource->GetWeakReference(&m_weakSession));

    WSL_LOG(
        "WSLASessionReferenceCreated",
        TraceLoggingLevel(WINEVENT_LEVEL_INFO),
        TraceLoggingUInt32(m_sessionId, "SessionId"),
        TraceLoggingWideString(m_displayName.c_str(), "DisplayName"));
}

wsla::WSLASessionReference::~WSLASessionReference()
{
    WSL_LOG(
        "WSLASessionReferenceDestroyed",
        TraceLoggingLevel(WINEVENT_LEVEL_INFO),
        TraceLoggingUInt32(m_sessionId, "SessionId"),
        TraceLoggingWideString(m_displayName.c_str(), "DisplayName"));
}

HRESULT wsla::WSLASessionReference::OpenSession(_Out_ IWSLASession** Session)
try
{
    *Session = nullptr;

    Microsoft::WRL::ComPtr<IWSLASession> lockedSession;
    THROW_IF_FAILED(m_weakSession->Resolve(__uuidof(IWSLASession), reinterpret_cast<IInspectable**>(lockedSession.GetAddressOf())));

    if (!lockedSession)
    {
        return HRESULT_FROM_WIN32(ERROR_OBJECT_NO_LONGER_EXISTS);
    }

    WSLASessionState state{};
    RETURN_IF_FAILED(lockedSession->GetState(&state));
    if (state == WSLASessionStateTerminated)
    {
        return HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
    }

    *Session = lockedSession.Detach();
    return S_OK;
}
CATCH_RETURN()

HRESULT wsla::WSLASessionReference::GetId(_Out_ ULONG* Id)
{
    *Id = m_sessionId;
    return S_OK;
}

HRESULT wsla::WSLASessionReference::GetCreatorPid(_Out_ DWORD* Pid)
{
    *Pid = m_creatorPid;
    return S_OK;
}

HRESULT wsla::WSLASessionReference::GetDisplayName(_Out_ LPWSTR* DisplayName)
try
{
    *DisplayName = wil::make_cotaskmem_string(m_displayName.c_str()).release();
    return S_OK;
}
CATCH_RETURN()

HRESULT wsla::WSLASessionReference::GetSid(_Out_ LPWSTR* Sid)
try
{
    auto sidString = wslutil::SidToString(m_tokenInfo->User.Sid);
    *Sid = wil::make_cotaskmem_string(sidString.get()).release();
    return S_OK;
}
CATCH_RETURN()

HRESULT wsla::WSLASessionReference::IsElevated(_Out_ BOOL* Elevated)
{
    *Elevated = m_elevated ? TRUE : FALSE;
    return S_OK;
}

HRESULT wsla::WSLASessionReference::Terminate()
try
{
    // Resolve the session first, then mark as terminated.
    // OpenSession() checks m_terminated and returns ERROR_INVALID_STATE, so we must
    // get the session reference before setting the flag.
    Microsoft::WRL::ComPtr<IWSLASession> session;
    if (SUCCEEDED(OpenSession(&session)))
    {
        return session->Terminate();
    }

    return S_OK; // Session already gone
}
CATCH_RETURN()
