/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    Session.cpp

Abstract:

    This file contains the implementation of the WinRT wrapper for the WSLC SDK Session class.

--*/

#include "precomp.h"

#include "Session.h"
#include "Microsoft.WSL.Containers.Session.g.cpp"

namespace WSLC = winrt::Microsoft::WSL::Containers;

namespace winrt::Microsoft::WSL::Containers {

implementation::Session::~Session()
{
    if (m_session)
    {
        WslcReleaseSession(m_session);
        m_session = nullptr;
    }
}

WSLC::Session implementation::Session::Create(WSLC::SessionSettings const& settings)
{
    auto session = winrt::make_self<implementation::Session>();
    wil::unique_cotaskmem_string errorMessage;
    auto hr = WslcCreateSession(implementation::GetStructPointer(settings), implementation::GetHandlePointer(session), &errorMessage);
    THROW_MSG_IF_FAILED(hr, errorMessage);
    return *session;
}

void implementation::Session::Terminate()
{
    winrt::check_hresult(WslcTerminateSession(m_session));
}

WslcSession implementation::Session::ToHandle()
{
    return m_session;
}

WslcSession* implementation::Session::ToHandlePointer()
{
    return &m_session;
}
} // namespace winrt::Microsoft::WSL::Containers
