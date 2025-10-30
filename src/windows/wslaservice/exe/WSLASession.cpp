/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLASession.cpp

Abstract:

    This file contains the implementation of the WSLASession COM class.

--*/

#include "WSLASession.h"

using wsl::windows::service::wsla::WSLASession;

HRESULT WSLASession::GetDisplayName(LPWSTR* DisplayName)
{
    *DisplayName = wil::make_unique_string<wil::unique_cotaskmem_string>(m_sessionConfig.DisplayName).release();
    return S_OK;
}

WSLASession::WSLASession(const WSLA_SESSION_CONFIGURATION& SessionConfiguration) : m_sessionConfig(SessionConfiguration)
{
}
