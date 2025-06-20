/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    LxssServerPort.h

Abstract:

    This file contains declarations for the server port wrapper class.

--*/

#pragma once

class LxssMessagePort;

class LxssServerPort
{
public:
    LxssServerPort();

    LxssServerPort(_In_ wil::unique_handle&& ServerPortHandle);

    void RegisterLxBusServer(_In_ const wil::unique_handle& InstanceHandle, _In_ LPCSTR ServerName);

    HANDLE
    ReleaseServerPort();

    std::unique_ptr<LxssMessagePort> WaitForConnection(_In_ ULONG TimeoutMs = LXBUS_IPC_INFINITE_TIMEOUT);

    NTSTATUS
    WaitForConnectionNoThrow(_Out_ std::unique_ptr<LxssMessagePort>* MessagePort, _In_ ULONG TimeoutMs = LXBUS_IPC_INFINITE_TIMEOUT) const;

private:
    wil::unique_handle m_serverPort;
};
