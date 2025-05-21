/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    lxbusclient.cpp

Abstract:

    This file contains the LxBus client library implementation.

--*/

#include "precomp.h"

NTSTATUS
LxBusClientpIoctlInternal(
    _In_ HANDLE Handle,
    _In_opt_ HANDLE Event,
    _Inout_ PIO_STATUS_BLOCK IoStatusBlock,
    _In_ ULONG ControlCode,
    _In_reads_bytes_(InputBufferSize) PVOID InputBuffer,
    _In_ ULONG InputBufferSize,
    _Out_writes_bytes_(OutputBufferSize) PVOID OutputBuffer,
    _In_ ULONG OutputBufferSize);

NTSTATUS
LxBusClientCreateUnnamedServer(_In_ HANDLE MessagePortHandle, _Out_ PLXBUS_IPC_MESSAGE_CREATE_UNNAMED_SERVER_PARAMETERS Parameters)

/*++

Routine Description:

    This routine creates an unnamed server with an LxBus message port.

Arguments:

    MessagePortHandle - Supplies a handle to a message port.

    Parameters - Supplies a pointer to the create unnamed server parameters.

Return Value:

    NT status code.

--*/

{
    return LxBusClientpIoctl(MessagePortHandle, LXBUS_IPC_MESSAGE_IOCTL_CREATE_UNNAMED_SERVER, NULL, 0, Parameters, sizeof(*Parameters));
}

NTSTATUS
LxBusClientDisconnectConsole(_In_ HANDLE MessagePortHandle, _Inout_ PLXBUS_IPC_MESSAGE_DISCONNECT_CONSOLE_PARAMETERS Parameters)

/*++

Routine Description:

    This routine sends a disconnect console ioctl to the specified message port
    handle.

Arguments:

    MessagePortHandle - Supplies a handle to the message port to issue the
        ioctl to.

    Parameters - Supplies a pointer to the disconnect console parameters.

Return Value:

    NT status code.

--*/

{
    return LxBusClientpIoctl(MessagePortHandle, LXBUS_IPC_MESSAGE_IOCTL_DISCONNECT_CONSOLE, Parameters, sizeof(*Parameters), NULL, 0);
}

NTSTATUS
LxBusClientMarshalConsole(_In_ HANDLE MessagePortHandle, _Inout_ PLXBUS_IPC_MESSAGE_MARSHAL_CONSOLE_PARAMETERS Parameters)

/*++

Routine Description:

    This routine sends a marshal console ioctl to the specified message port
    handle.

Arguments:

    MessagePortHandle - Supplies a handle to the message port to issue the
        ioctl to.

    Parameters - Supplies a pointer to the marshal console parameters.

Return Value:

    NT status code.

--*/

{
    return LxBusClientpIoctl(
        MessagePortHandle, LXBUS_IPC_MESSAGE_IOCTL_MARSHAL_CONSOLE, Parameters, sizeof(*Parameters), Parameters, sizeof(*Parameters));
}

NTSTATUS
LxBusClientReleaseConsole(_In_ HANDLE MessagePortHandle, _In_ PLXBUS_IPC_MESSAGE_IOCTL_CANCEL_MARSHAL_PARAMETERS Parameters)

/*++

Routine Description:

    This routine sends a cleanup console ioctl to the specified message port
    handle.

Arguments:

    MessagePortHandle - Supplies a handle to the message port to issue the
        ioctl to.

    Parameters - Supplies a pointer to the release console parameters.

Return Value:

    NT status code.

--*/

{
    return LxBusClientpIoctl(MessagePortHandle, LXBUS_IPC_MESSAGE_IOCTL_CANCEL_MARSHAL, Parameters, sizeof(*Parameters), NULL, 0);
}

NTSTATUS
LxBusClientMarshalForkToken(_In_ HANDLE MessagePortHandle, _Inout_ PLXBUS_IPC_MESSAGE_MARSHAL_FORK_TOKEN_PARAMETERS Parameters)

/*++

Routine Description:

    This routine sends a marshal fork token ioctl to the specified message port
    handle.

Arguments:

    MessagePortHandle - Supplies a handle to the message port to issue the
        ioctl to.

    Parameters - Supplies a pointer to the marshal fork token parameters.

Return Value:

    NT status code.

--*/

{
    return LxBusClientpIoctl(
        MessagePortHandle, LXBUS_IPC_MESSAGE_IOCTL_MARSHAL_FORK_TOKEN, Parameters, sizeof(*Parameters), Parameters, sizeof(*Parameters));
}

NTSTATUS
LxBusClientReleaseForkToken(_In_ HANDLE MessagePortHandle, _In_ PLXBUS_IPC_MESSAGE_IOCTL_CANCEL_MARSHAL_PARAMETERS Parameters)

/*++

Routine Description:

    This routine sends a cleanup fork token ioctl to the specified message port
    handle.

Arguments:

    MessagePortHandle - Supplies a handle to the message port to issue the
        ioctl to.

    Parameters - Supplies a pointer to the release fork token parameters.

Return Value:

    NT status code.

--*/

{
    return LxBusClientpIoctl(MessagePortHandle, LXBUS_IPC_MESSAGE_IOCTL_CANCEL_MARSHAL, Parameters, sizeof(*Parameters), NULL, 0);
}

NTSTATUS
LxBusClientMarshalHandle(_In_ HANDLE MessagePortHandle, _Inout_ PLXBUS_IPC_MESSAGE_MARSHAL_HANDLE_PARAMETERS Parameters)

/*++

Routine Description:

    This routine sends a marshal handle ioctl to the specified message port
    handle.

Arguments:

    MessagePortHandle - Supplies a handle to the message port to issue the
        ioctl to.

    Parameters - Supplies a pointer to the marshal handle parameters.

Return Value:

    NT status code.

--*/

{
    return LxBusClientpIoctl(
        MessagePortHandle, LXBUS_IPC_MESSAGE_IOCTL_MARSHAL_HANDLE, Parameters, sizeof(*Parameters), Parameters, sizeof(*Parameters));
}

NTSTATUS
LxBusClientReleaseHandle(_In_ HANDLE MessagePortHandle, _In_ PLXBUS_IPC_MESSAGE_IOCTL_CANCEL_MARSHAL_PARAMETERS Parameters)

/*++

Routine Description:

    This routine sends a cleanup handle ioctl to the specified message port
    handle.

Arguments:

    MessagePortHandle - Supplies a handle to the message port to issue the
        ioctl to.

    Parameters - Supplies a pointer to the release handle parameters.

Return Value:

    NT status code.

--*/

{
    return LxBusClientpIoctl(MessagePortHandle, LXBUS_IPC_MESSAGE_IOCTL_CANCEL_MARSHAL, Parameters, sizeof(*Parameters), NULL, 0);
}

NTSTATUS
LxBusClientMarshalProcess(_In_ HANDLE MessagePortHandle, _Inout_ PLXBUS_IPC_MESSAGE_MARSHAL_PROCESS_PARAMETERS Parameters)

/*++

Routine Description:

    This routine sends a marshal process ioctl to the specified message port
    handle.

Arguments:

    MessagePortHandle - Supplies a handle to the message port to issue the
        ioctl to.

    Parameters - Supplies a pointer to the marshal process parameters.

Return Value:

    NT status code.

--*/

{
    return LxBusClientpIoctl(
        MessagePortHandle, LXBUS_IPC_MESSAGE_IOCTL_MARSHAL_PROCESS, Parameters, sizeof(*Parameters), Parameters, sizeof(*Parameters));
}

NTSTATUS
LxBusClientpIoctl(
    _In_ HANDLE Handle,
    _In_ ULONG ControlCode,
    _In_reads_bytes_(InputBufferSize) PVOID InputBuffer,
    _In_ ULONG InputBufferSize,
    _Out_writes_bytes_(OutputBufferSize) PVOID OutputBuffer,
    _In_ ULONG OutputBufferSize)

/*++

Routine Description:

    This routine implements the lxbus control device IOCTL handler.

Arguments:

    Handle - Supplies the handle to issue the ioctl to.

    ControlCode - Supplies ioctl to be processed.

    InputBuffer - Supplies the input buffer as passed by user.

    InputBufferSize - Supplies the size of the input buffer.

    OutputBuffer - Supplies a output buffer as passed by the user.

    OutputBufferSize - Supplies the size of the output buffer.

Return Value:

    NT status code.

--*/

{
    NTSTATUS Status;
    IO_STATUS_BLOCK IoStatus;

    ZeroMemory(&IoStatus, sizeof(IoStatus));
    Status = LxBusClientpIoctlInternal(Handle, NULL, &IoStatus, ControlCode, InputBuffer, InputBufferSize, OutputBuffer, OutputBufferSize);

    WI_ASSERT(Status != STATUS_PENDING);

    WI_ASSERT(!NT_SUCCESS(Status) || (OutputBufferSize == IoStatus.Information));

    return Status;
}

NTSTATUS
LxBusClientpIoctlInternal(
    _In_ HANDLE Handle,
    _In_opt_ HANDLE Event,
    _Inout_ PIO_STATUS_BLOCK IoStatus,
    _In_ ULONG ControlCode,
    _In_reads_bytes_(InputBufferSize) PVOID InputBuffer,
    _In_ ULONG InputBufferSize,
    _Out_writes_bytes_(OutputBufferSize) PVOID OutputBuffer,
    _In_ ULONG OutputBufferSize)

/*++

Routine Description:

    This routine implements the lxbus control device IOCTL handler.

Arguments:

    Handle - Supplies the handle to issue the ioctl to.

    Event - Supplies an optional event used for async IO.

    IoStatus - Supplies a pointer to the IO status block.

    ControlCode - Supplies ioctl to be processed.

    InputBuffer - Supplies the input buffer as passed by user.

    InputBufferSize - Supplies the size of the input buffer.

    OutputBuffer - Supplies a output buffer as passed by the user.

    OutputBufferSize - Supplies the size of the output buffer.

Return Value:

    NT status code.

--*/

{
    NTSTATUS Status;

    if (Handle == NULL)
    {
        Status = STATUS_DEVICE_NOT_CONNECTED;
        goto LxBusClientpIoctlInternalEnd;
    }

    Status = NtDeviceIoControlFile(Handle, Event, NULL, NULL, IoStatus, ControlCode, InputBuffer, InputBufferSize, OutputBuffer, OutputBufferSize);

LxBusClientpIoctlInternalEnd:
    return Status;
}

NTSTATUS
LxBusClientReceiveMessage(_In_ HANDLE MessagePortHandle, _Out_writes_to_(BufferSize, *SizeReceived) PVOID Buffer, _In_ ULONG BufferSize, _Out_ PULONG SizeReceived)

/*++

Routine Description:

    This routine receives a message from the given message port synchronously.

Arguments:

    MessagePortHandle - Supplies a handle to a message port.

    Buffer - Supplies a pointer to a buffer.

    BufferSize - Supplies the size of the buffer in bytes.

    SizeReceived - Supplies a buffer to store the number of bytes received.

Return Value:

    NT status code.

--*/

{
    HANDLE Event;
    IO_STATUS_BLOCK IoStatus;
    NTSTATUS Status;

    Event = NULL;
    RtlZeroMemory(&IoStatus, sizeof(IoStatus));
    Status = ZwCreateEvent(&Event, EVENT_ALL_ACCESS, NULL, SynchronizationEvent, FALSE);

    if (!NT_SUCCESS(Status))
    {
        goto LxBusClientReceiveMessageEnd;
    }

    Status = LxBusClientReceiveMessageAsync(MessagePortHandle, Buffer, BufferSize, SizeReceived, &IoStatus, Event);

    if (!NT_SUCCESS(Status))
    {
        goto LxBusClientReceiveMessageEnd;
    }

    if (Status == STATUS_PENDING)
    {
        Status = NtWaitForSingleObject(Event, FALSE, NULL);
        if (!NT_SUCCESS(Status))
        {
            goto LxBusClientReceiveMessageEnd;
        }
    }

    WI_ASSERT(NT_SUCCESS(Status));

    WI_ASSERT(IoStatus.Information <= ULONG_MAX);

    *SizeReceived = (ULONG)IoStatus.Information;
    Status = IoStatus.Status;

LxBusClientReceiveMessageEnd:
    if (Event != NULL)
    {
        NtClose(Event);
    }

    return Status;
}

NTSTATUS
LxBusClientReceiveMessageAsync(
    _In_ HANDLE MessagePortHandle,
    _Out_writes_bytes_to_(BufferSize, *SizeReceived) PVOID Buffer,
    _In_ ULONG BufferSize,
    _Out_ PULONG SizeReceived,
    _Out_ PIO_STATUS_BLOCK IoStatus,
    _Inout_opt_ HANDLE Event)

/*++

Routine Description:

    This routine receives a message from the given message port asynchronously.

Arguments:

    MessagePortHandle - Supplies a handle to a message port.

    Buffer - Supplies a pointer to a buffer.

    BufferSize - Supplies the size of the buffer in bytes.

    SizeReceived - Supplies a buffer to store the number of bytes received.

    IoStatus - Supplies a pointer to an io status block.

    Event - Supplies an optional event to be signaled when the operation is
        complete.

Return Value:

    NT status code.

--*/

{
    LARGE_INTEGER ByteOffset{};
    const NTSTATUS Status = NtReadFile(MessagePortHandle, Event, NULL, NULL, IoStatus, Buffer, BufferSize, &ByteOffset, NULL);
    if (Status == STATUS_SUCCESS)
    {
        WI_ASSERT(IoStatus->Information <= ULONG_MAX);

        *SizeReceived = (ULONG)IoStatus->Information;
    }

    return Status;
}

NTSTATUS
LxBusClientRegisterServer(_In_ HANDLE LxBusHandle, _Inout_ PLXBUS_REGISTER_SERVER_PARAMETERS Parameters)

/*++

Routine Description:

    This routine registers an LxBus server with the given name.

Arguments:

    LxBusHandle - The LxBusHandle on which to perform the registration.

    Parameters - Supplies a pointer to the register server parameters.

Return Value:

    NT status code.

--*/

{
    return LxBusClientpIoctl(LxBusHandle, LXBUS_IOCTL_REGISTER_SERVER, Parameters, sizeof(*Parameters), Parameters, sizeof(*Parameters));
}

NTSTATUS
LxBusClientRegisterUserCallbackAsync(
    _In_ HANDLE LxBusHandle,
    _In_ HANDLE Event,
    _Inout_ PIO_STATUS_BLOCK IoStatus,
    _Inout_ PLXBUS_REGISTER_USER_CALLBACK_PARAMETERS Parameters,
    _Out_ PVOID OutputBuffer,
    _In_ ULONG OutputBufferSize)

/*++

Routine Description:

    This routine sends a user-callback registration ioctl to the specified
    instance handle.

Arguments:

    LxBusHandle - Supplies ths instance handle to issue the ioctl to.

    Event - Supplies an event to be signalled when the request has completed.

    IoStatus - Supplies a pointer to the IO status block.

    Parameters - Supplies a pointer to the user callback parameters.

    OutputBuffer - Supplies a pointer to the output buffer.

    OutputBufferSize - Supplies the size in bytes of the output buffer.

Return Value:

    NT status code.

--*/

{
    return LxBusClientpIoctlInternal(
        LxBusHandle, Event, IoStatus, LXBUS_IOCTL_REGISTER_USER_CALLBACK, Parameters, sizeof(*Parameters), OutputBuffer, OutputBufferSize);
}

NTSTATUS
LxBusClientSendMessage(_In_ HANDLE MessagePortHandle, _In_reads_bytes_(BufferSize) PVOID Buffer, _In_ ULONG BufferSize)

/*++

Routine Description:

    This routine sends a message to the given message port synchronously.

Arguments:

    MessagePortHandle - Supplies a handle to a message port.

    Buffer - Supplies a pointer to a buffer.

    BufferSize - Supplies the size of the buffer in bytes.

Return Value:

    NT status code.

--*/

{
    HANDLE Event;
    IO_STATUS_BLOCK IoStatus;
    NTSTATUS Status;

    Event = NULL;
    RtlZeroMemory(&IoStatus, sizeof(IoStatus));
    Status = ZwCreateEvent(&Event, EVENT_ALL_ACCESS, NULL, SynchronizationEvent, FALSE);

    if (!NT_SUCCESS(Status))
    {
        goto LxBusClientSendMessageEnd;
    }

    Status = LxBusClientSendMessageAsync(MessagePortHandle, Buffer, BufferSize, &IoStatus, Event);

    if (!NT_SUCCESS(Status))
    {
        goto LxBusClientSendMessageEnd;
    }

    WI_ASSERT((Status != STATUS_SUCCESS) || (BufferSize == IoStatus.Information));

    if (Status == STATUS_PENDING)
    {
        Status = NtWaitForSingleObject(Event, FALSE, NULL);
        if (!NT_SUCCESS(Status))
        {
            goto LxBusClientSendMessageEnd;
        }
    }

    Status = IoStatus.Status;

LxBusClientSendMessageEnd:
    if (Event != NULL)
    {
        NtClose(Event);
    }

    return Status;
}

NTSTATUS
LxBusClientSendMessageAsync(
    _In_ HANDLE MessagePortHandle, _In_reads_bytes_(BufferSize) PVOID Buffer, _In_ ULONG BufferSize, _Out_ PIO_STATUS_BLOCK IoStatus, _Inout_opt_ HANDLE Event)

/*++

Routine Description:

    This routine sends a message to the given message port asynchronously.

Arguments:

    MessagePortHandle - Supplies a handle to a message port.

    Buffer - Supplies a pointer to a buffer.

    BufferSize - Supplies the size of the buffer in bytes.

    IoStatus - Supplies a pointer to an io status block.

    Event - Supplies an optional event to be signaled when the operation is
        complete.

Return Value:

    NT status code.

--*/

{
    LARGE_INTEGER ByteOffset{};
    const NTSTATUS Status = NtWriteFile(MessagePortHandle, Event, NULL, NULL, IoStatus, Buffer, BufferSize, &ByteOffset, NULL);

    WI_ASSERT((Status != STATUS_SUCCESS) || (BufferSize == IoStatus->Information));

    return Status;
}

NTSTATUS
LxBusClientUnmarshalProcess(_In_ HANDLE MessagePortHandle, _Inout_ PLXBUS_IPC_MESSAGE_UNMARSHAL_PROCESS_PARAMETERS Parameters)

/*++

Routine Description:

    This routine sends a unmarshal process ioctl to the specified message port
    handle.

Arguments:

    MessagePortHandle - Supplies a handle to the message port to issue the
        ioctl to.

    Parameters - Supplies a pointer to the unmarshal process parameters.

Return Value:

    NT status code.

--*/

{
    return LxBusClientpIoctl(
        MessagePortHandle, LXBUS_IPC_MESSAGE_IOCTL_UNMARSHAL_PROCESS, Parameters, sizeof(*Parameters), Parameters, sizeof(*Parameters));
}

NTSTATUS
LxBusClientUnmarshalVfsFile(_In_ HANDLE MessagePortHandle, _Inout_ PLXBUS_IPC_MESSAGE_UNMARSHAL_VFS_FILE_PARAMETERS Parameters)

/*++

Routine Description:

    This routine sends a unmarshal vfs file ioctl to the specified message port
    handle.

Arguments:

    MessagePortHandle - Supplies a handle to the message port to issue the
        ioctl to.

    Parameters - Supplies a pointer to the unmarshal vfs file parameters.

Return Value:

    NT status code.

--*/

{
    return LxBusClientpIoctl(
        MessagePortHandle, LXBUS_IPC_MESSAGE_IOCTL_UNMARSHAL_VFS_FILE, Parameters, sizeof(*Parameters), Parameters, sizeof(*Parameters));
}

NTSTATUS
LxBusClientUserCallbackSendResponse(_In_ HANDLE LxBusHandle, _Inout_ PLXBUS_REGISTER_USER_CALLBACK_PARAMETERS Parameters)

/*++

Routine Description:

    This routine sends a response from a user-callback to the specified
    instance handle.

Arguments:

    LxBusHandle - Supplies ths instance handle to issue the ioctl to.

    Parameters - Supplies a pointer to the user callback parameters.

Return Value:

    NT status code.

--*/

{
    return LxBusClientpIoctl(LxBusHandle, LXBUS_IOCTL_REGISTER_USER_CALLBACK, Parameters, sizeof(*Parameters), NULL, 0);
}

NTSTATUS
LxBusClientWaitForConnection(_In_ HANDLE ServerPortHandle, _Out_ PLXBUS_IPC_SERVER_WAIT_FOR_CONNECTION_PARAMETERS Parameters)

/*++

Routine Description:

    Waits for a client connection on the provided server port.

Arguments:

    ServerPortHandle - Supplies a handle to the server port.

    Parameters - Supplies a pointer to the wait for connection parameters.

Return Value:

    NT status code.

--*/

{
    return LxBusClientpIoctl(
        ServerPortHandle, LXBUS_IPC_SERVER_IOCTL_WAIT_FOR_CONNECTION, Parameters, sizeof(*Parameters), Parameters, sizeof(*Parameters));
}

NTSTATUS
LxBusClientWaitForLxProcess(_In_ HANDLE LxProcessHandle, _Out_ PLXBUS_IPC_LX_PROCESS_WAIT_FOR_TERMINATION_PARAMETERS Parameters)

/*++

Routine Description:

    Waits for a client connection on the provided server port.

Arguments:

    LxProcessHandle - Supplies a handle to the LX process.

    Parameters - Supplies a pointer to the wait for termination parameters.

Return Value:

    NT status code.

--*/

{
    return LxBusClientpIoctl(
        LxProcessHandle, LXBUS_IPC_LX_PROCESS_IOCTL_WAIT_FOR_TERMINATION, Parameters, sizeof(*Parameters), Parameters, sizeof(*Parameters));
}
