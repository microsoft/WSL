/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ProgressCallback.cpp

Abstract:

    Implementation of a type that implements IProgressCallback.

--*/
#include "precomp.h"
#include "ProgressCallback.h"


using namespace std::string_view_literals;

namespace
{
    WslcImageProgressStatus ConvertStatus(LPCSTR Status)
    {
#define WSLC_STRING_TO_STATUS_MAPPING(_status_, _string_) \
        if (_string_##sv == Status) \
        { \
            return _status_; \
        }

        WSLC_STRING_TO_STATUS_MAPPING(WSLC_IMAGE_PROGRESS_PULLING, "Pulling fs layer");
        WSLC_STRING_TO_STATUS_MAPPING(WSLC_IMAGE_PROGRESS_WAITING, "Waiting");
        WSLC_STRING_TO_STATUS_MAPPING(WSLC_IMAGE_PROGRESS_DOWNLOADING, "Downloading");
        WSLC_STRING_TO_STATUS_MAPPING(WSLC_IMAGE_PROGRESS_VERIFYING, "Verifying Checksum");
        WSLC_STRING_TO_STATUS_MAPPING(WSLC_IMAGE_PROGRESS_EXTRACTING, "Extracting");
        WSLC_STRING_TO_STATUS_MAPPING(WSLC_IMAGE_PROGRESS_COMPLETE, "Pull complete");

        return WSLC_IMAGE_PROGRESS_UNKNOWN;
    }
}

ProgressCallback::ProgressCallback(WslcContainerImageProgressCallback callback, PVOID context) :
    m_callback(callback), m_context(context)
{}

HRESULT STDMETHODCALLTYPE ProgressCallback::OnProgress(LPCSTR Status, LPCSTR Id, ULONGLONG Current, ULONGLONG Total)
{
    if (m_callback)
    {
        WslcImageProgressMessage message{};

        message.id = Id;
        message.status = ConvertStatus(Status);
        message.detail.current = Current;
        message.detail.total = Total;

        m_callback(&message, m_context);
    }

    return S_OK;
}

winrt::com_ptr<ProgressCallback> ProgressCallback::CreateIf(const WslcPullImageOptions* options)
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
