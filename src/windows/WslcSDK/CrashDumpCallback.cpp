/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    CrashDumpCallback.cpp

Abstract:

    Implementation of a type that implements ICrashDumpCallback.

--*/
#include "precomp.h"
#include "CrashDumpCallback.h"

CrashDumpCallback::CrashDumpCallback(WslcSessionCrashDumpCallback callback, PVOID context) :
    m_callback(callback), m_context(context)
{
}

HRESULT STDMETHODCALLTYPE CrashDumpCallback::OnCrashDump(
    _In_ LPCWSTR DumpPath, _In_opt_ LPCSTR ProcessName, _In_ ULONGLONG Pid, _In_ ULONG Signal, _In_ ULONGLONG Timestamp)
try
{
    if (m_callback)
    {
        WslcSessionCrashDumpInfo info{};
        info.dumpPath = DumpPath;
        info.processName = ProcessName ? ProcessName : "";
        info.pid = Pid;
        info.signal = Signal;
        info.timestamp = Timestamp;

        m_callback(&info, m_context);
    }

    return S_OK;
}
CATCH_RETURN();
