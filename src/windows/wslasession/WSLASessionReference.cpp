/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLASessionReference.cpp

Abstract:

    Implementation for WSLASessionReference.

    This class provides a weak reference to a session that the SYSTEM service
    can use to:
    - Check if a session is still alive (OpenSession fails if session is gone)
    - Terminate sessions when requested by elevated callers

--*/

#include "WSLASessionReference.h"
#include "WSLASession.h"

namespace wsla = wsl::windows::service::wsla;

wsla::WSLASessionReference::WSLASessionReference(_In_ WSLASession* Session)
{
    Microsoft::WRL::ComPtr<IWeakReferenceSource> weakRefSource;
    THROW_IF_FAILED(Session->QueryInterface(IID_PPV_ARGS(&weakRefSource)));
    THROW_IF_FAILED(weakRefSource->GetWeakReference(&m_weakSession));
}

wsla::WSLASessionReference::~WSLASessionReference() = default;

HRESULT wsla::WSLASessionReference::OpenSession(_Out_ IWSLASession** Session)
{
    *Session = nullptr;

    Microsoft::WRL::ComPtr<IWSLASession> lockedSession;
    RETURN_IF_FAILED(m_weakSession->Resolve(__uuidof(IWSLASession), reinterpret_cast<IInspectable**>(lockedSession.GetAddressOf())));

    RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_OBJECT_NO_LONGER_EXISTS), !lockedSession);

    WSLASessionState state{};
    RETURN_IF_FAILED(lockedSession->GetState(&state));

    RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), state != WSLASessionStateRunning);

    *Session = lockedSession.Detach();
    return S_OK;
}

HRESULT wsla::WSLASessionReference::Terminate()
try
{
    // Resolve the weak reference directly (bypassing OpenSession which checks GetState).
    // We want to terminate regardless of session state.
    Microsoft::WRL::ComPtr<IWSLASession> session;
    RETURN_IF_FAILED(m_weakSession->Resolve(__uuidof(IWSLASession), reinterpret_cast<IInspectable**>(session.GetAddressOf())));

    if (session)
    {
        return session->Terminate();
    }

    return S_OK; // Session already released
}
CATCH_RETURN()
