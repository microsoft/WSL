/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    TerminationCallback.cpp

Abstract:

    Implementation of a type that implements ITerminationCallback.

--*/
#pragma once
#include "TerminationCallback.h"

namespace
{
    WslcSessionTerminationReason ConvertReason(WSLAVirtualMachineTerminationReason Reason)
    {
        switch (Reason)
        {
        case WSLAVirtualMachineTerminationReasonShutdown:
            return WSLC_SESSION_TERMINATION_REASON_SHUTDOWN;
        case WSLAVirtualMachineTerminationReasonCrashed:
            return WSLC_SESSION_TERMINATION_REASON_CRASHED;
        default:
            return WSLC_SESSION_TERMINATION_REASON_UNKNOWN;
        }
    }
}

TerminationCallback::TerminationCallback(WslcSessionTerminationCallback terminationCallback, PVOID terminationCallbackContext) :
    m_terminationCallback(terminationCallback), m_terminationCallbackContext(terminationCallbackContext)
{}

// TODO: Details is lost?
HRESULT STDMETHODCALLTYPE TerminationCallback::OnTermination(WSLAVirtualMachineTerminationReason Reason, LPCWSTR)
{
    if (m_terminationCallback)
    {
        m_terminationCallback(ConvertReason(Reason), m_terminationCallbackContext);
    }

    return S_OK;
}

winrt::com_ptr<TerminationCallback> TerminationCallback::CreateIf(const WSLC_SESSION_OPTIONS_INTERNAL* options)
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
