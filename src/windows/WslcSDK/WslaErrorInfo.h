/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WslaErrorInfo.h

Abstract:

    Header for a wrapper to make WSLA_ERROR_INFO RAII.

--*/
#pragma once
#include "wslaservice.h"
#include "defs.h"

struct WslaErrorInfo
{
    // Disabling the error info causes the WSLA_ERROR_INFO* to return nullptr, preventing unnecessary copies of the error message if it will not be used.
    WslaErrorInfo(bool enabled = true);

    NON_COPYABLE(WslaErrorInfo);
    NON_MOVABLE(WslaErrorInfo);

    ~WslaErrorInfo();

    operator WSLA_ERROR_INFO*();

    void CopyMessageIf(PWSTR* errorMessage);

private:
    WSLA_ERROR_INFO m_errorInfo{};
    bool m_enabled;
};
