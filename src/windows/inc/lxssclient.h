/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    lxssclient.h

Abstract:

    This header file contains data structures and prototypes for the LXSS
    client library.

--*/

#pragma once

NTSTATUS
LxssClientInitialize(VOID);

NTSTATUS
LxssClientInstanceCreate(_In_ PLX_KINSTANCECREATESTART Parameters, _Out_ PHANDLE InstanceHandle);

NTSTATUS
LxssClientInstanceDestroy(_In_ HANDLE InstanceHandle);

NTSTATUS
LxssClientInstanceGetExitStatus(_In_ HANDLE InstanceHandle, _Out_ PLONG ExitStatus);

NTSTATUS
LxssClientInstanceStart(_In_ HANDLE InstanceHandle, _In_ HANDLE ParentProcess);

NTSTATUS
LxssClientInstanceStop(_In_ HANDLE InstanceHandle);

NTSTATUS
LxssClientMapPath(
    _In_ HANDLE InstanceHandle,
    _In_ HANDLE WindowsDataRoot,
    _In_ LPCSTR Source,
    _In_ LPCSTR Target,
    _In_ LPCSTR FsType,
    _In_ ULONG MountFlags,
    _In_ ULONG Uid,
    _In_ ULONG Gid,
    _In_ ULONG Mode);

VOID LxssClientUninitialize(VOID);

NTSTATUS
LxssClientUnmapPath(_In_ HANDLE InstanceHandle, _In_ LPCSTR MountPath);
