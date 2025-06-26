/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    DllMain.cpp

Abstract:

    This file the entrypoint for the LSW client library.

--*/

#include "precomp.h"
#include "wslservice.h"

HRESULT GetWslVersion(WSL_VERSION* Version)
try
{
    wil::com_ptr<ILSWUserSession> session;

    THROW_IF_FAILED(CoCreateInstance(__uuidof(LSWUserSession), nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&session)));

    return session->GetVersion(Version);
}
CATCH_RETURN();

EXTERN_C BOOL STDAPICALLTYPE DllMain(_In_ HINSTANCE Instance, _In_ DWORD Reason, _In_opt_ LPVOID Reserved)
{
    wil::DLLMain(Instance, Reason, Reserved);

    switch (Reason)
    {
    case DLL_PROCESS_ATTACH:
        WslTraceLoggingInitialize(LxssTelemetryProvider, false);
        break;

    case DLL_PROCESS_DETACH:
        WslTraceLoggingUninitialize();
        break;
    }

    return TRUE;
}