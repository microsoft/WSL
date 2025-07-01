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

HRESULT CreateVm(const VIRTUAL_MACHINE_SETTINGS* Settings, ILSWVirtualMachine** VirtualMachine)
try
{
    wil::com_ptr<ILSWUserSession> session;

    THROW_IF_FAILED(CoCreateInstance(__uuidof(LSWUserSession), nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&session)));

    THROW_IF_FAILED(session->CreateVirtualMachine(Settings, VirtualMachine));

    wil::com_ptr_nothrow<IClientSecurity> clientSecurity;
    THROW_IF_FAILED((*VirtualMachine)->QueryInterface(IID_PPV_ARGS(&clientSecurity)));

    // Get the current proxy blanket settings.
    DWORD authnSvc, authzSvc, authnLvl, capabilites;
    THROW_IF_FAILED(clientSecurity->QueryBlanket(*VirtualMachine, &authnSvc, &authzSvc, NULL, &authnLvl, NULL, NULL, &capabilites));

    // Make sure that dynamic cloaking is used.
    WI_ClearFlag(capabilites, EOAC_STATIC_CLOAKING);
    WI_SetFlag(capabilites, EOAC_DYNAMIC_CLOAKING);
    THROW_IF_FAILED(clientSecurity->SetBlanket(*VirtualMachine, authnSvc, authzSvc, NULL, authnLvl, RPC_C_IMP_LEVEL_IMPERSONATE, NULL, capabilites));

    return S_OK;
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