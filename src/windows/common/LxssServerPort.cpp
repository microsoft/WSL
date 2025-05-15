/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    LxssServerPort.cpp

Abstract:

    This file contains a wrapper class for LxBus server ports.

--*/

#include "precomp.h"
#include "LxssServerPort.h"
#include "LxssMessagePort.h"

LxssServerPort::LxssServerPort()
{
}

LxssServerPort::LxssServerPort(_In_ wil::unique_handle&& ServerPortHandle) : m_serverPort(std::move(ServerPortHandle))
{
}

void LxssServerPort::RegisterLxBusServer(_In_ const wil::unique_handle& InstanceHandle, _In_ LPCSTR ServerName)
{
    WI_ASSERT(!m_serverPort);

    LXBUS_REGISTER_SERVER_PARAMETERS RegisterServer = {};
    RegisterServer.Input.ServerName = ServerName;
    THROW_IF_NTSTATUS_FAILED(LxBusClientRegisterServer(InstanceHandle.get(), &RegisterServer));

    m_serverPort.reset(UlongToHandle(RegisterServer.Output.ServerPort));
}

HANDLE
LxssServerPort::ReleaseServerPort()
{
    return m_serverPort.release();
}

std::unique_ptr<LxssMessagePort> LxssServerPort::WaitForConnection(_In_ ULONG TimeoutMs)
{
    std::unique_ptr<LxssMessagePort> MessagePort;
    THROW_IF_NTSTATUS_FAILED(WaitForConnectionNoThrow(&MessagePort, TimeoutMs));

    return MessagePort;
}

NTSTATUS
LxssServerPort::WaitForConnectionNoThrow(_Out_ std::unique_ptr<LxssMessagePort>* MessagePort, _In_ ULONG TimeoutMs) const
{
    LXBUS_IPC_SERVER_WAIT_FOR_CONNECTION_PARAMETERS Params = {};
    Params.Input.TimeoutMs = TimeoutMs;
    const NTSTATUS Status = LxBusClientWaitForConnection(m_serverPort.get(), &Params);
    if (NT_SUCCESS(Status))
    {
        *MessagePort = std::make_unique<LxssMessagePort>(UlongToHandle(Params.Output.MessagePort));
    }

    return Status;
}
