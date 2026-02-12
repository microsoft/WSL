/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    TerminationCallback.h

Abstract:

    Header for a type that implements ITerminationCallback.

--*/
#pragma once
#include "wslaservice.h"
#include "wslcsdkprivate.h"
#include <winrt/base.h>

struct TerminationCallback : public winrt::implements<TerminationCallback, ITerminationCallback>
{
    TerminationCallback(WslcSessionTerminationCallback callback, PVOID context);

    // ITerminationCallback
    HRESULT STDMETHODCALLTYPE OnTermination(WSLAVirtualMachineTerminationReason Reason, LPCWSTR Details) override;

    // Creates a TerminationCallback if the options provides a callback.
    static winrt::com_ptr<TerminationCallback> CreateIf(const WSLC_SESSION_OPTIONS_INTERNAL* options);

private:
    WslcSessionTerminationCallback m_callback;
    PVOID m_context;
};
