/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WslaErrorInfo.cpp

Abstract:

    Implementation of a wrapper to make WSLA_ERROR_INFO RAII.

--*/
#include "precomp.h"
#include "WslaErrorInfo.h"
#include "stringshared.h"

WslaErrorInfo::WslaErrorInfo(bool enabled) : m_enabled(enabled)
{}

WslaErrorInfo::~WslaErrorInfo()
{
    if (m_errorInfo.UserErrorMessage)
    {
        CoTaskMemFree(m_errorInfo.UserErrorMessage);
    }

    // WarningsPipe appears to be copied over from other usage, but is not currently used.
    // It also appears to be an input value, enabling a caller to provide a pipe and get real time output for the operation.
}

WslaErrorInfo::operator WSLA_ERROR_INFO* ()
{
    return m_enabled ? &m_errorInfo : nullptr;
}

void WslaErrorInfo::CopyMessageIf(PWSTR* errorMessage)
{
    if (errorMessage)
    {
        if (m_errorInfo.UserErrorMessage)
        {
            auto wideErrorMessage = wsl::shared::string::MultiByteToWide(m_errorInfo.UserErrorMessage);
            *errorMessage = wil::make_unique_string<wil::unique_cotaskmem_string>(wideErrorMessage.c_str(), wideErrorMessage.length()).release();
        }
        else
        {
            *errorMessage = nullptr;
        }
    }
}