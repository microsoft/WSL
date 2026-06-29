/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    CrashDumpCallback.h

Abstract:

    Header for a type that implements IWSLCCompatCrashDumpCallback. Bridges the COM
    IWSLCCompatCrashDumpCallback interface back to the C-style SDK callback registered
    via WslcRegisterSessionCrashDumpCallback.

--*/
#pragma once
#include "WSLCCompat.h"
#include "wslcsdkprivate.h"
#include <winrt/base.h>

struct CrashDumpCallback : public winrt::implements<CrashDumpCallback, IWSLCCompatCrashDumpCallback>
{
    CrashDumpCallback(WslcSessionCrashDumpCallback callback, PVOID context);

    // IWSLCCompatCrashDumpCallback
    HRESULT STDMETHODCALLTYPE OnCrashDump(_In_ LPCWSTR DumpPath, _In_opt_ LPCSTR ProcessName, _In_ ULONG Pid, _In_ ULONG Signal, _In_ ULONGLONG Timestamp) override;

private:
    WslcSessionCrashDumpCallback m_callback = nullptr;
    PVOID m_context = nullptr;
};
