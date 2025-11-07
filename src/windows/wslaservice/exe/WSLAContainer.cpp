/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLAContainer.cpp

Abstract:

    Contains the implementation of WSLAContainer.

--*/

#include "precomp.h"
#include "WSLAContainer.h"
#include "WSLAProcess.h"

using wsl::windows::service::wsla::WSLAContainer;

HRESULT WSLAContainer::Start()
{
    return E_NOTIMPL;
}

HRESULT WSLAContainer::Stop(int Signal, ULONG TimeoutMs)
{
    return E_NOTIMPL;
}

HRESULT WSLAContainer::Delete()
{
    return E_NOTIMPL;
}

HRESULT WSLAContainer::GetState(WSLA_CONTAINER_STATE* State)
{
    return E_NOTIMPL;
}

HRESULT WSLAContainer::GetInitProcess(IWSLAProcess** process)
{
    return E_NOTIMPL;
}

HRESULT WSLAContainer::Exec(const WSLA_PROCESS_OPTIONS* Options, IWSLAProcess** Process)
try
{
    auto process = wil::MakeOrThrow<WSLAProcess>();

    process.CopyTo(__uuidof(IWSLAProcess), (void**)Process);

    return S_OK;
}
CATCH_RETURN();
