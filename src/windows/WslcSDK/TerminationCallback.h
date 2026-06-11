/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    TerminationCallback.h

Abstract:

    Header for a type that implements IWSLCSDKTerminationCallback.

--*/
#pragma once
#include "WSLSDK.h"
#include "wslcsdkprivate.h"
#include <winrt/base.h>

struct TerminationCallback : public winrt::implements<TerminationCallback, IWSLCSDKTerminationCallback>
{
    TerminationCallback(WslcSessionTerminationCallback callback, PVOID context);

    // IWSLCSDKTerminationCallback
    HRESULT STDMETHODCALLTYPE OnTermination(WSLCSDKVirtualMachineTerminationReason Reason, LPCWSTR Details) override;

    // Creates a TerminationCallback if the options provides a callback.
    static winrt::com_ptr<TerminationCallback> CreateIf(const WslcSessionOptionsInternal* options);

private:
    WslcSessionTerminationCallback m_callback = nullptr;
    PVOID m_context = nullptr;
};
