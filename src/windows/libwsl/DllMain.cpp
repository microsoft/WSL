/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    DllMain.cpp

Abstract:

    This file contains the entrypoint for libwsl.

--*/

#include "precomp.h"

static HINSTANCE g_dllInstance;

EXTERN_C BOOL STDAPICALLTYPE DllMain(_In_ HINSTANCE Instance, _In_ DWORD Reason, _In_opt_ LPVOID Reserved)
{
    wil::DLLMain(Instance, Reason, Reserved);

    switch (Reason)
    {
    case DLL_PROCESS_ATTACH:
        g_dllInstance = Instance;
        WslTraceLoggingInitialize(LxssTelemetryProvider, TRUE, nullptr);

        // Accidentally including a Module<OutOfProc> can result in lifetime issues because it will call
        // CoAddRefServerProcess/CoReleaseServerProcess outside of any WRL::Module<> that may be in use in the caller,
        // which means the global counter is getting updated without the Module<> specific checks (e.g. last reference
        // has been released).
        FAIL_FAST_HR_IF_MSG(
            HRESULT_FROM_WIN32(ERROR_INVALID_STATE),
            Microsoft::WRL::GetModuleBase() != nullptr,
            "A WRL::Module has been included");

        break;

    case DLL_PROCESS_DETACH:
        WslTraceLoggingUninitialize();
        break;
    }

    return TRUE;
}
