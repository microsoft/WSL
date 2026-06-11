/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    CrashDumpCallback.h

Abstract:

    Header for a type that implements IWSLCSDKCrashDumpCallback. Bridges the COM
    IWSLCSDKCrashDumpCallback interface back to the C-style SDK callback registered
    via WslcRegisterSessionCrashDumpCallback.

--*/
#pragma once
#include "WSLSDK.h"
#include "wslcsdkprivate.h"
#include <winrt/base.h>

struct CrashDumpCallback : public winrt::implements<CrashDumpCallback, IWSLCSDKCrashDumpCallback>
{
    CrashDumpCallback(WslcSessionCrashDumpCallback callback, PVOID context);

    // IWSLCSDKCrashDumpCallback
    HRESULT STDMETHODCALLTYPE OnCrashDump(_In_ LPCWSTR DumpPath, _In_opt_ LPCSTR ProcessName, _In_ ULONGLONG Pid, _In_ ULONG Signal, _In_ ULONGLONG Timestamp) override;

private:
    WslcSessionCrashDumpCallback m_callback = nullptr;
    PVOID m_context = nullptr;
};
