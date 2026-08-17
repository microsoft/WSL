/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ProgressCallback.cpp

Abstract:

    Implementation of a type that implements IWSLCCompatProgressCallback.

--*/
#include "precomp.h"
#include "ProgressCallback.h"

using namespace std::string_view_literals;

namespace {
WslcImageProgressStatus ConvertStatus(LPCSTR Status)
{
#define WSLC_STRING_TO_STATUS_MAPPING(_status_, _string_) \
    if (_string_##sv == Status) \
    { \
        return _status_; \
    }

#define WSLC_PREFIX_TO_STATUS_MAPPING(_status_, _prefix_) \
    if (std::string_view{Status}.starts_with(_prefix_##sv)) \
    { \
        return _status_; \
    }

    if (Status)
    {
        // TODO: Mapping engine strings to status values seems fragile.
        //       WSLC is intentionally avoiding this kind of thing for localization of engine strings, which amounts to the same
        //       thing. If we keep this, a test should be added to explicitly validate that each status is returned properly.
        WSLC_PREFIX_TO_STATUS_MAPPING(WSLC_IMAGE_PROGRESS_STATUS_PULLING, "Pulling from ");
        WSLC_STRING_TO_STATUS_MAPPING(WSLC_IMAGE_PROGRESS_STATUS_PULLING, "Pulling fs layer");
        WSLC_STRING_TO_STATUS_MAPPING(WSLC_IMAGE_PROGRESS_STATUS_WAITING, "Waiting");
        WSLC_STRING_TO_STATUS_MAPPING(WSLC_IMAGE_PROGRESS_STATUS_DOWNLOADING, "Downloading");
        WSLC_STRING_TO_STATUS_MAPPING(WSLC_IMAGE_PROGRESS_STATUS_COMPLETE, "Download complete");
        WSLC_STRING_TO_STATUS_MAPPING(WSLC_IMAGE_PROGRESS_STATUS_VERIFYING, "Verifying Checksum");
        WSLC_PREFIX_TO_STATUS_MAPPING(WSLC_IMAGE_PROGRESS_STATUS_VERIFYING, "Digest: ");
        WSLC_STRING_TO_STATUS_MAPPING(WSLC_IMAGE_PROGRESS_STATUS_EXTRACTING, "Extracting");
        WSLC_STRING_TO_STATUS_MAPPING(WSLC_IMAGE_PROGRESS_STATUS_COMPLETE, "Pull complete");
        WSLC_PREFIX_TO_STATUS_MAPPING(WSLC_IMAGE_PROGRESS_STATUS_COMPLETE, "Status: ");
    }

    WSL_LOG_DEBUG("UnknownImageProgressStatus", TraceLoggingString(Status, "status"));
    return WSLC_IMAGE_PROGRESS_STATUS_UNKNOWN;
}
} // namespace

ProgressCallback::ProgressCallback(WslcContainerImageProgressCallback callback, PVOID context) :
    m_callback(callback), m_context(context)
{
}

HRESULT STDMETHODCALLTYPE ProgressCallback::OnProgress(LPCSTR Status, LPCSTR Id, ULONGLONG Current, ULONGLONG Total)
{
    if (m_callback)
    {
        WslcImageProgressMessage message{};

        message.id = Id;
        message.status = ConvertStatus(Status);
        message.detail.currentBytes = Current;
        message.detail.totalBytes = Total;

        return m_callback(&message, m_context);
    }

    return S_OK;
}
