/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    TerminationCallback.cpp

Abstract:

    Implementation of a type that implements ITerminationCallback.

--*/
#include "precomp.h"
#include "TerminationCallback.h"

namespace {
WslcSessionTerminationReason ConvertReason(WSLCVirtualMachineTerminationReason Reason)
{
    switch (Reason)
    {
    case WSLCVirtualMachineTerminationReasonShutdown:
        return WSLC_SESSION_TERMINATION_REASON_SHUTDOWN;
    case WSLCVirtualMachineTerminationReasonCrashed:
        return WSLC_SESSION_TERMINATION_REASON_CRASHED;
    default:
        return WSLC_SESSION_TERMINATION_REASON_UNKNOWN;
    }
}
} // namespace

TerminationCallback::TerminationCallback(WslcSessionTerminationCallback callback, PVOID context) :
    m_callback(callback), m_context(context)
{
}

// TODO: Details from the runtime are dropped; should the SDK callback function be updated to include the reasons string?
HRESULT STDMETHODCALLTYPE TerminationCallback::OnTermination(WSLCVirtualMachineTerminationReason Reason, LPCWSTR)
{
    if (m_callback)
    {
        m_callback(ConvertReason(Reason), m_context);
    }

    return S_OK;
}

winrt::com_ptr<TerminationCallback> TerminationCallback::CreateIf(const WslcSessionOptionsInternal* options)
{
    if (options->terminationCallback)
    {
        return winrt::make_self<TerminationCallback>(options->terminationCallback, options->terminationCallbackContext);
    }
    else
    {
        return nullptr;
    }
}
