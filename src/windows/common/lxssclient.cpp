/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    lxssclient.cpp

Abstract:

    This file contains the LXSS client library implementation.

--*/

#include "precomp.h"
#include "lxssbusclient.h"

HANDLE LxssRootHandle = NULL;

NTSTATUS
LxssClientInitialize(VOID)

/*++

Routine Description:

    Initializes a new LXSS client by connecting to the LXSS driver.

Arguments:

    None.

Return Value:

    NTSTATUS.

--*/

{
    UNICODE_STRING ControlDevicePath;
    IO_STATUS_BLOCK IoStatus;
    OBJECT_ATTRIBUTES ObjectAttributes;
    NTSTATUS Status;

    WI_ASSERT(LxssRootHandle == NULL);

    //
    // Create the device string and connect to the device.
    //

    RtlInitUnicodeString(&ControlDevicePath, LX_CONTROL_DEVICE_ROOT);
    InitializeObjectAttributes(&ObjectAttributes, &ControlDevicePath, OBJ_CASE_INSENSITIVE, NULL, NULL);

    Status = NtOpenFile(&LxssRootHandle, FILE_WRITE_DATA, &ObjectAttributes, &IoStatus, 0, 0);

    WI_ASSERT(Status != STATUS_PENDING);

    if (!NT_SUCCESS(Status))
    {
        if (LxssRootHandle != NULL)
        {
            WI_VERIFY(NT_SUCCESS(NtClose(LxssRootHandle)));
        }

        LxssRootHandle = NULL;
    }

    return Status;
}

NTSTATUS
LxssClientInstanceCreate(_In_ PLX_KINSTANCECREATESTART Parameters, _Out_ PHANDLE InstanceHandle)

/*++

Routine Description:

    This routine sends an instance create request to the LXSS driver.

Arguments:

    Parameters - Supplies a pointer to the input parameters.

    InstanceHandle - Supplies a buffer to receive a handle to the instance.

Return Value:

    NTSTATUS.

--*/

{
    //
    // Create the instance.
    //

    return LxBusClientpIoctl(
        LxssRootHandle, LXBUS_ROOT_IOCTL_CREATE_INSTANCE, Parameters, sizeof(*Parameters), InstanceHandle, sizeof(*InstanceHandle));
}

NTSTATUS
LxssClientInstanceDestroy(_In_ HANDLE InstanceHandle)

/*++

Routine Description:

    This routine sends an instance destroy request to the LXSS driver.

Arguments:

    InstanceHandle - Supplies a handle to an instance.

Return Value:

    NTSTATUS.

--*/

{
    LX_KINSTANCESETSTATE InstanceSetState;
    NTSTATUS Status;

    //
    // Destroy the instance.
    //

    RtlZeroMemory(&InstanceSetState, sizeof(InstanceSetState));
    InstanceSetState.Type = LxKInstanceSetStateTypeDestroy;
    Status = LxBusClientpIoctl(InstanceHandle, LXBUS_IOCTL_SET_INSTANCE_STATE, &InstanceSetState, sizeof(InstanceSetState), NULL, 0);

    return Status;
}

NTSTATUS
LxssClientInstanceGetExitStatus(_In_ HANDLE InstanceHandle, _Out_ PLONG ExitStatus)

/*++

Routine Description:

    This routine sends a instance get exit status request to the LXSS driver.

Arguments:

    InstanceHandle - Supplies a handle to an instance.

    ExitStatus - Supplies a buffer to receive the instance exit status.

Return Value:

    NTSTATUS.

--*/

{
    NTSTATUS ExitStatusLocal;
    NTSTATUS Status;

    Status = LxBusClientpIoctl(InstanceHandle, LXBUS_INSTANCE_IOCTL_GET_INIT_EXIT_STATUS, NULL, 0, &ExitStatusLocal, sizeof(ExitStatusLocal));

    if (!NT_SUCCESS(Status))
    {
        goto ClientInstanceGetExitStatusEnd;
    }

    *ExitStatus = ExitStatusLocal;

ClientInstanceGetExitStatusEnd:
    return Status;
}

NTSTATUS
LxssClientInstanceStart(_In_ HANDLE InstanceHandle, _In_ HANDLE ParentProcessHandle)

/*++

Routine Description:

    This routine sends an instance start request to the LXSS driver.

Arguments:

    InstanceHandle - Supplies a handle to an instance.

    ParentProcessHandle - Supplies a handle to the parent process for the
        instance.

Return Value:

    NTSTATUS.

--*/

{
    LX_KINSTANCESETSTATE InstanceSetState;
    NTSTATUS Status;

    //
    // Start the instance.
    //

    RtlZeroMemory(&InstanceSetState, sizeof(InstanceSetState));
    InstanceSetState.Type = LxKInstanceSetStateTypeStart;
    InstanceSetState.TypeData.StartParentProcessHandle = HandleToUlong(ParentProcessHandle);

    Status = LxBusClientpIoctl(InstanceHandle, LXBUS_IOCTL_SET_INSTANCE_STATE, &InstanceSetState, sizeof(InstanceSetState), NULL, 0);

    return Status;
}

NTSTATUS
LxssClientInstanceStop(_In_ HANDLE InstanceHandle)

/*++

Routine Description:

    This routine sends an instance stop request to the LXSS driver.

Arguments:

    InstanceHandle - Supplies a handle to an instance.

Return Value:

    NTSTATUS.

--*/

{
    LX_KINSTANCESETSTATE InstanceSetState;
    NTSTATUS Status;

    //
    // Stop the instance.
    //

    RtlZeroMemory(&InstanceSetState, sizeof(InstanceSetState));
    InstanceSetState.Type = LxKInstanceSetStateTypeStop;
    Status = LxBusClientpIoctl(InstanceHandle, LXBUS_IOCTL_SET_INSTANCE_STATE, &InstanceSetState, sizeof(InstanceSetState), NULL, 0);

    return Status;
}

VOID LxssClientUninitialize(VOID)

/*++

Routine Description:

    Uninitializes a new LXSS client by disconnecting from LXSS driver.

Arguments:

    None.

Return Value:

    None.

--*/

{
    if (LxssRootHandle != NULL)
    {
        WI_VERIFY(NT_SUCCESS(NtClose(LxssRootHandle)));
        LxssRootHandle = NULL;
    }
}
