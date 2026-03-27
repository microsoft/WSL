/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCSessionReference.cpp

Abstract:

    Implementation for WSLCSessionReference.

    This class provides a weak reference to a session that the SYSTEM service
    can use to:
    - Check if a session is still alive (OpenSession fails if session is gone)
    - Terminate sessions when requested by elevated callers

--*/

#include "WSLCSessionReference.h"
#include "WSLCSession.h"

namespace wslc = wsl::windows::service::wslc;

wslc::WSLCSessionReference::WSLCSessionReference(_In_ WSLCSession* Session)
{
    Microsoft::WRL::ComPtr<IWeakReferenceSource> weakRefSource;
    THROW_IF_FAILED(Session->QueryInterface(IID_PPV_ARGS(&weakRefSource)));
    THROW_IF_FAILED(weakRefSource->GetWeakReference(&m_weakSession));
}

wslc::WSLCSessionReference::~WSLCSessionReference() = default;

HRESULT wslc::WSLCSessionReference::OpenSession(_Out_ IWSLCSession** Session)
{
    *Session = nullptr;

    Microsoft::WRL::ComPtr<IWSLCSession> lockedSession;
    RETURN_IF_FAILED(m_weakSession->Resolve(__uuidof(IWSLCSession), reinterpret_cast<IInspectable**>(lockedSession.GetAddressOf())));

    RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_OBJECT_NO_LONGER_EXISTS), !lockedSession);

    WSLCSessionState state{};
    RETURN_IF_FAILED(lockedSession->GetState(&state));

    RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), state != WSLCSessionStateRunning);

    *Session = lockedSession.Detach();
    return S_OK;
}

HRESULT wslc::WSLCSessionReference::Terminate()
try
{
    // Resolve the weak reference directly (bypassing OpenSession which checks GetState).
    // We want to terminate regardless of session state.
    Microsoft::WRL::ComPtr<IWSLCSession> session;
    RETURN_IF_FAILED(m_weakSession->Resolve(__uuidof(IWSLCSession), reinterpret_cast<IInspectable**>(session.GetAddressOf())));

    if (session)
    {
        return session->Terminate();
    }

    return S_OK; // Session already released
}
CATCH_RETURN()
