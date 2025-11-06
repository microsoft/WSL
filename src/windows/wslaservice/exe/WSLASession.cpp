/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLASession.cpp

Abstract:

    This file contains the implementation of the WSLASession COM class.

--*/

#include "WSLASession.h"
#include "WSLAUserSession.h"

using wsl::windows::service::wsla::WSLASession;

WSLASession::WSLASession(const WSLA_SESSION_SETTINGS& Settings, WSLAUserSessionImpl& userSessionImpl, const VIRTUAL_MACHINE_SETTINGS& VmSettings) :
    m_sessionSettings(Settings),
    m_userSession(userSessionImpl),
    m_virtualMachine(VmSettings, userSessionImpl.GetUserSid(), &userSessionImpl),
    m_displayName(Settings.DisplayName)
{
    m_virtualMachine.Start();
}

HRESULT WSLASession::GetDisplayName(LPWSTR* DisplayName)
{
    *DisplayName = wil::make_unique_string<wil::unique_cotaskmem_string>(m_displayName.c_str()).release();
    return S_OK;
}

HRESULT WSLASession::GetVirtualMachine(IWSLAVirtualMachine** VirtualMachine)
{
    THROW_IF_FAILED(m_virtualMachine.QueryInterface(__uuidof(IWSLAVirtualMachine), (void**)VirtualMachine));
    return S_OK;
}
