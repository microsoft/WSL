/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ProgressCallback.h

Abstract:

    Header for a type that implements IProgressCallback.

--*/
#pragma once
#include "wslaservice.h"
#include "wslcsdkprivate.h"
#include <winrt/base.h>

struct ProgressCallback : public winrt::implements<ProgressCallback, IProgressCallback>
{
    ProgressCallback(WslcContainerImageProgressCallback callback, PVOID context);

    // IProgressCallback
    HRESULT STDMETHODCALLTYPE OnProgress(LPCSTR Status, LPCSTR Id, ULONGLONG Current, ULONGLONG Total) override;

    // Creates a ProgressCallback if the options provides a callback.
    template <typename Options>
    static winrt::com_ptr<ProgressCallback> CreateIf(const Options* options)
    {
        if (options->progressCallback)
        {
            return winrt::make_self<ProgressCallback>(options->progressCallback, options->progressCallbackContext);
        }
        else
        {
            return nullptr;
        }
    }

private:
    WslcContainerImageProgressCallback m_callback = nullptr;
    PVOID m_context = nullptr;
};
