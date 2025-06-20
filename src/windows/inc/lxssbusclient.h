/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    lxssbusclient.h

Abstract:

    This header file contains data structures and prototypes for the LxBus
    client library.

--*/

#pragma once

NTSTATUS
LxBusClientCreateUnnamedServer(_In_ HANDLE MessagePortHandle, _Out_ PLXBUS_IPC_MESSAGE_CREATE_UNNAMED_SERVER_PARAMETERS Parameters);

NTSTATUS
LxBusClientDisconnectConsole(_In_ HANDLE MessagePortHandle, _Inout_ PLXBUS_IPC_MESSAGE_DISCONNECT_CONSOLE_PARAMETERS Parameters);

NTSTATUS
LxBusClientpIoctl(
    _In_ HANDLE Handle,
    _In_ ULONG ControlCode,
    _In_reads_bytes_(InputBufferSize) PVOID InputBuffer,
    _In_ ULONG InputBufferSize,
    _Out_writes_bytes_(OutputBufferSize) PVOID OutputBuffer,
    _In_ ULONG OutputBufferSize);

NTSTATUS
LxBusClientMarshalConsole(_In_ HANDLE MessagePortHandle, _Inout_ PLXBUS_IPC_MESSAGE_MARSHAL_CONSOLE_PARAMETERS Parameters);

NTSTATUS
LxBusClientMarshalForkToken(_In_ HANDLE MessagePortHandle, _Inout_ PLXBUS_IPC_MESSAGE_MARSHAL_FORK_TOKEN_PARAMETERS Parameters);

NTSTATUS
LxBusClientMarshalHandle(_In_ HANDLE MessagePortHandle, _Inout_ PLXBUS_IPC_MESSAGE_MARSHAL_HANDLE_PARAMETERS Parameters);

NTSTATUS
LxBusClientMarshalProcess(_In_ HANDLE MessagePortHandle, _Inout_ PLXBUS_IPC_MESSAGE_MARSHAL_PROCESS_PARAMETERS Parameters);

NTSTATUS
LxBusClientReceiveMessage(_In_ HANDLE MessagePortHandle, _Out_writes_to_(BufferSize, *SizeReceived) PVOID Buffer, _In_ ULONG BufferSize, _Out_ PULONG SizeReceived);

NTSTATUS
LxBusClientReceiveMessageAsync(
    _In_ HANDLE MessagePortHandle,
    _Out_writes_bytes_to_(BufferSize, *SizeReceived) PVOID Buffer,
    _In_ ULONG BufferSize,
    _Out_ PULONG SizeReceived,
    _Out_ PIO_STATUS_BLOCK IoStatus,
    _Inout_opt_ HANDLE Event);

NTSTATUS
LxBusClientRegisterServer(_In_ HANDLE LxBusHandle, _Inout_ PLXBUS_REGISTER_SERVER_PARAMETERS Parameters);

NTSTATUS
LxBusClientRegisterUserCallbackAsync(
    _In_ HANDLE MessagePortHandle,
    _In_ HANDLE Event,
    _Inout_ PIO_STATUS_BLOCK IoStatus,
    _Inout_ PLXBUS_REGISTER_USER_CALLBACK_PARAMETERS Parameters,
    _Out_ PVOID OutputBuffer,
    _In_ ULONG OutputBufferSize);

NTSTATUS
LxBusClientReleaseConsole(_In_ HANDLE MessagePortHandle, _In_ PLXBUS_IPC_MESSAGE_IOCTL_CANCEL_MARSHAL_PARAMETERS Parameters);

NTSTATUS
LxBusClientReleaseForkToken(_In_ HANDLE MessagePortHandle, _In_ PLXBUS_IPC_MESSAGE_IOCTL_CANCEL_MARSHAL_PARAMETERS Parameters);

NTSTATUS
LxBusClientReleaseHandle(_In_ HANDLE MessagePortHandle, _In_ PLXBUS_IPC_MESSAGE_IOCTL_CANCEL_MARSHAL_PARAMETERS Parameters);

NTSTATUS
LxBusClientSendMessage(_In_ HANDLE MessagePortHandle, _In_reads_bytes_(BufferSize) PVOID Buffer, _In_ ULONG BufferSize);

NTSTATUS
LxBusClientSendMessageAsync(
    _In_ HANDLE MessagePortHandle, _In_reads_bytes_(BufferSize) PVOID Buffer, _In_ ULONG BufferSize, _Out_ PIO_STATUS_BLOCK IoStatus, _Inout_opt_ HANDLE Event);

NTSTATUS
LxBusClientUnmarshalProcess(_In_ HANDLE MessagePortHandle, _Inout_ PLXBUS_IPC_MESSAGE_UNMARSHAL_PROCESS_PARAMETERS Parameters);

NTSTATUS
LxBusClientUnmarshalVfsFile(_In_ HANDLE MessagePortHandle, _Inout_ PLXBUS_IPC_MESSAGE_UNMARSHAL_VFS_FILE_PARAMETERS Parameters);

NTSTATUS
LxBusClientUserCallbackSendResponse(_In_ HANDLE MessagePortHandle, _Inout_ PLXBUS_REGISTER_USER_CALLBACK_PARAMETERS Parameters);

NTSTATUS
LxBusClientWaitForConnection(_In_ HANDLE ServerPortHandle, _Out_ PLXBUS_IPC_SERVER_WAIT_FOR_CONNECTION_PARAMETERS Parameters);

NTSTATUS
LxBusClientWaitForLxProcess(_In_ HANDLE LxProcessHandle, _Out_ PLXBUS_IPC_LX_PROCESS_WAIT_FOR_TERMINATION_PARAMETERS Parameters);
