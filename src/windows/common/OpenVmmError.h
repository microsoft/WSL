// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

namespace wsl::windows::common::openvmm {

inline PCSTR FailureCategory(HRESULT Result) noexcept
{
    switch (Result)
    {
    case E_INVALIDARG:
    case HRESULT_FROM_WIN32(ERROR_NOT_FOUND):
    case HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS):
    case E_NOTIMPL:
        return "Validation";
    case E_ACCESSDENIED:
    case HRESULT_FROM_WIN32(ERROR_LOGON_FAILURE):
        return "Authorization";
    case HRESULT_FROM_WIN32(WAIT_TIMEOUT):
        return "Timeout";
    case E_ABORT:
        return "Cancellation";
    case HRESULT_FROM_WIN32(ERROR_CONNECTION_ABORTED):
        return "Transport";
    case HRESULT_FROM_WIN32(ERROR_INVALID_DATA):
        return "Protocol";
    case HRESULT_FROM_WIN32(ERROR_INVALID_STATE):
        return "InvalidState";
    default:
        return "ServerOrResource";
    }
}

inline bool RequiresRecreation(HRESULT Result) noexcept
{
    // The ABI does not expose dispatch/state information. Retire on any failure except
    // unambiguous validation/authorization rejection, even a possible pre-dispatch timeout.
    switch (Result)
    {
    case E_INVALIDARG:
    case HRESULT_FROM_WIN32(ERROR_NOT_FOUND):
    case HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS):
    case E_NOTIMPL:
    case E_ACCESSDENIED:
    case HRESULT_FROM_WIN32(ERROR_LOGON_FAILURE):
        return false;
    default:
        return FAILED(Result);
    }
}

} // namespace wsl::windows::common::openvmm
