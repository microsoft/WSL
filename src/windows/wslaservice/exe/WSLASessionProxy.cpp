/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLASessionProxy.cpp

Abstract:

    Implementation for WSLASessionProxy.

    This is a lightweight proxy that provides:
    - GetSession() to hand out the underlying remote IWSLASession for direct calls
    - Session metadata accessors for the session manager
    - Lifetime management (Terminate on destruction for non-persistent sessions)

--*/

#include "WSLASessionProxy.h"

using wsl::windows::service::wsla::CallingProcessTokenInfo;
using wsl::windows::service::wsla::WSLASessionProxy;

WSLASessionProxy::WSLASessionProxy(
    _In_ ULONG SessionId, _In_ DWORD CreatorPid, _In_ std::wstring DisplayName, _In_ CallingProcessTokenInfo TokenInfo, _In_ wil::com_ptr<IWSLASession> Session) :
    m_sessionId(SessionId),
    m_creatorPid(CreatorPid),
    m_displayName(std::move(DisplayName)),
    m_tokenInfo(std::move(TokenInfo)),
    m_session(std::move(Session))
{
    WSL_LOG(
        "WSLASessionProxyCreated",
        TraceLoggingLevel(WINEVENT_LEVEL_INFO),
        TraceLoggingUInt32(m_sessionId, "SessionId"),
        TraceLoggingWideString(m_displayName.c_str(), "DisplayName"));
}

WSLASessionProxy::~WSLASessionProxy()
{
    WSL_LOG(
        "WSLASessionProxyDestroyed",
        TraceLoggingLevel(WINEVENT_LEVEL_INFO),
        TraceLoggingUInt32(m_sessionId, "SessionId"),
        TraceLoggingWideString(m_displayName.c_str(), "DisplayName"));

    // Terminate the underlying session when the proxy is destroyed.
    // For non-persistent sessions, this happens when all client refs are released.
    // For persistent sessions, this only happens during service shutdown.
    if (m_session && !m_terminated)
    {
        LOG_IF_FAILED(m_session->Terminate());
    }
}

void WSLASessionProxy::CopyDisplayName(_Out_writes_z_(bufferLength) PWSTR Buffer, size_t BufferLength) const
{
    THROW_HR_IF(E_BOUNDS, m_displayName.size() + 1 > BufferLength);
    wcscpy_s(Buffer, BufferLength, m_displayName.c_str());
}

HRESULT WSLASessionProxy::GetSession(_Out_ IWSLASession** Session)
{
    m_session.copy_to(Session);
    return S_OK;
}

HRESULT WSLASessionProxy::GetId(_Out_ ULONG* Id)
{
    *Id = m_sessionId;
    return S_OK;
}

HRESULT WSLASessionProxy::Terminate()
{
    m_terminated = true;
    return m_session->Terminate();
}
