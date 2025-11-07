/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLAProcess.cpp

Abstract:

    Contains the implementation of WSLAProcess.

--*/

#include "precomp.h"
#include "WSLAProcess.h"

using wsl::windows::service::wsla::WSLAProcess;

HRESULT WSLAProcess::Signal(int Signal)
{
    return E_NOTIMPL;
}

HRESULT WSLAProcess::GetExitEvent(ULONG* Event)
try
{
    *Event = HandleToUlong(common::wslutil::DuplicateHandleToCallingProcess(m_exitEvent.get()));
    return S_OK;
}
CATCH_RETURN();

HRESULT WSLAProcess::GetStdHandle(ULONG Index, ULONG* Handle)
{
    return E_NOTIMPL;
}

HRESULT WSLAProcess::GetPid(int* Pid)
{
    return E_NOTIMPL;
}

HRESULT WSLAProcess::GetState(WSLA_PROCESS_STATE* State, int* Code)
{
    return E_NOTIMPL;
}
