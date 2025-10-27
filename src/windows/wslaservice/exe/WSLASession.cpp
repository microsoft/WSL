/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLASession.coo

Abstract:

    This file contains the implementation of the WSLASession COM class.

--*/

#include "WSLASession.h"

using wsl::windows::service::wsla::WSLASession;

WSLASession::WSLASession(const WSLA_SESSION_SETTINGS& Settings) : m_displayName(Settings.DisplayName)
{
}

HRESULT WSLASession::GetDisplayName(LPWSTR* DisplayName)
{
    *DisplayName = wil::make_unique_string<wil::unique_cotaskmem_string>(m_displayName.c_str()).release();
    return S_OK;
}
