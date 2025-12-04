/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    wsla.cpp

Abstract:

    This file contains the public WSLA api implementations.

--*/

#include "wsla.h"

HRESULT WslaCanRun(
    _Out_ BOOL* canRun)
{
    return E_NOTIMPL;
}

HRESULT WslaGetVersion(
    _Out_ WSLA_VERSION* version)
{
    return E_NOTIMPL;
}

HRESULT WslaInstallWithDependencies(
    _In_opt_ __callback WslaInstallCallback progressCallback,
    _In_opt_ PVOID context)
{
    return E_NOTIMPL;
}

HRESULT WslaCreateSession(
    _In_ const WSLA_CREATE_SESSION_OPTIONS* settings,
    _Out_ WslaSession* sesssion)
{
    return E_NOTIMPL;
}

HRESULT WslaReleaseSession(
    _In_ WslaSession session)
{
    return E_NOTIMPL;
}

HRESULT WslaPullContainerImage(
    _In_ WslaSession session,
    _In_ const WLSA_PULL_CONTAINER_IMAGE_OPTIONS* options)
{
    return E_NOTIMPL;
}

HRESULT WslaImportContainerImage(
    _In_ WslaSession session,
    _In_ const WLSA_PULL_CONTAINER_IMAGE_OPTIONS* options)
{
    return E_NOTIMPL;
}

HRESULT WslaListContainerImages(
    _In_ WslaSession sesssion,
    _Inout_ WSLA_CONTAINER_IMAGE_INFO* images,
    _Inout_ UINT32* count)
{
    return E_NOTIMPL;
}

HRESULT WslaDeleteContainerImage(
    _In_ WslaSession session,
    _In_ PCSTR imageName)
{
    return E_NOTIMPL;
}

HRESULT WslaCreateNewContainer(
    _In_ WslaSession session,
    _In_ const WSLA_CONTAINER_OPTIONS* options,
    _Out_ WslaRuntimeContainer* container,
    _Out_ WSLA_CONTAINER_PROCESS* initProcess)
{
    return E_NOTIMPL;
}

HRESULT WslaStartContainer(
    _In_ WslaRuntimeContainer container)
{
    return E_NOTIMPL;
}

HRESULT WslaStopContainer(
    _In_ WslaRuntimeContainer container)
{
    return E_NOTIMPL;
}

HRESULT WslaDeleteContainer(
    _In_ WslaRuntimeContainer container)
{
    return E_NOTIMPL;
}

HRESULT WslaRestartContainer(
    _In_ WslaRuntimeContainer container)
{
    return E_NOTIMPL;
}

HRESULT WslaGetContainerState(
    _In_ WslaRuntimeContainer container,
    _Out_ WSLA_CONTAINER_STATE* state)
{
    return E_NOTIMPL;
}

HRESULT WslaCreateContainerProcess(
    _In_ WslaRuntimeContainer container,
    _In_ const WSLA_CONTAINER_PROCESS_OPTIONS* options,
    _Out_ WSLA_CONTAINER_PROCESS* process)
{
    return E_NOTIMPL;
}

HRESULT WslaGetContainerProcessResult(
    _In_ const WSLA_CONTAINER_PROCESS* process,
    _Out_ WSLA_CONTAINER_PROCESS_RESULT* result)
{
    return E_NOTIMPL;
}

HRESULT WslaSignalContainerProcess(
    _In_ WSLA_CONTAINER_PROCESS* process,
    _In_ INT32 signal)
{
    return E_NOTIMPL;
}

HRESULT WslaCreateVhd(
    _In_ const WSLA_CREATE_VHD_OPTIONS* options)
{
    return E_NOTIMPL;
}