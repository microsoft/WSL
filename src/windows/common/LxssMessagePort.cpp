/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    LxssMessagePort.cpp

Abstract:

    This file contains a wrapper class for LxBus message ports.

--*/

#include "precomp.h"
#include "LxssMessagePort.h"
#include "LxssServerPort.h"

// Defines.

#define LAUNCH_PROCESS_DEFAULT_BUFFER_SIZE 1024

LxssMessagePort::LxssMessagePort(_In_ HANDLE MessagePort) : m_messagePort(MessagePort), m_messageEvent(wil::EventOptions::None)
{
    //
    // N.B. The class takes ownership of the handle.
    //
}

LxssMessagePort::LxssMessagePort(_In_ LxssMessagePort&& Source) :
    m_messagePort(std::move(Source.m_messagePort)),
    m_messageEvent(std::move(Source.m_messageEvent)),
    m_serverPort(std::move(Source.m_serverPort))
{
}

LxssMessagePort::LxssMessagePort(_In_ std::unique_ptr<LxssMessagePort>&& SourcePointer) :
    LxssMessagePort(std::move(*SourcePointer.get()))
{
}

std::shared_ptr<LxssPort> LxssMessagePort::CreateSessionLeader(_In_ HANDLE ClientProcess)
{
    THROW_HR_IF(E_UNEXPECTED, (!m_serverPort));

    const LXBUS_IPC_MESSAGE_MARSHAL_CONSOLE_DATA Data{HandleToUlong(ClientProcess)};

    const LXBUS_IPC_CONSOLE_ID MarshalId = this->MarshalConsole(&Data);
    auto ReleaseConsole = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&] { this->ReleaseConsole(MarshalId); });

    LX_INIT_CREATE_SESSION Message{{LxInitMessageCreateSession, sizeof(Message)}, MarshalId};

    Send(&Message, sizeof(Message));
    auto LocalMessagePort = m_serverPort->WaitForConnection(c_defaultMessageTimeout);
    ReleaseConsole.release();
    return LocalMessagePort;
}

wil::unique_handle LxssMessagePort::CreateUnnamedServer(_Out_ PLXBUS_SERVER_ID ServerId) const
{
    LXBUS_IPC_MESSAGE_CREATE_UNNAMED_SERVER_PARAMETERS Parameters;
    THROW_IF_NTSTATUS_FAILED(LxBusClientCreateUnnamedServer(m_messagePort.get(), &Parameters));

    *ServerId = Parameters.Output.ServerId;
    return wil::unique_handle(ULongToHandle(Parameters.Output.ServerPort));
}

void LxssMessagePort::DisconnectConsole(_In_ HANDLE ClientProcess)
{
    LXBUS_IPC_MESSAGE_DISCONNECT_CONSOLE_PARAMETERS Parameters;
    NTSTATUS Status;
    Parameters.Input.ConsoleData.ClientProcess = HandleToUlong(ClientProcess);
    Status = LxBusClientDisconnectConsole(m_messagePort.get(), &Parameters);

    // Console disconnect is expected to fail in two cases:
    //     1. The instance has been torn down: STATUS_NOT_FOUND
    //     2. The tty device that had the console reference has already been
    //        closed: STATUS_NO_SUCH_DEVICE
    if ((Status != STATUS_NOT_FOUND) && (Status != STATUS_NO_SUCH_DEVICE))
    {
        THROW_IF_NTSTATUS_FAILED(Status);
    }
}

wil::cs_leave_scope_exit LxssMessagePort::Lock()
{
    return m_lock.lock();
}

LXBUS_IPC_CONSOLE_ID
LxssMessagePort::MarshalConsole(_In_ PCLXBUS_IPC_MESSAGE_MARSHAL_CONSOLE_DATA ConsoleData) const
{
    LXBUS_IPC_MESSAGE_MARSHAL_CONSOLE_PARAMETERS Parameters;
    Parameters.Input.ConsoleData = *ConsoleData;
    THROW_IF_NTSTATUS_FAILED(LxBusClientMarshalConsole(m_messagePort.get(), &Parameters));

    return Parameters.Output.ConsoleId;
}

LXBUS_IPC_FORK_TOKEN_ID
LxssMessagePort::MarshalForkToken(_In_ HANDLE TokenHandle) const
{
    LXBUS_IPC_MESSAGE_MARSHAL_FORK_TOKEN_PARAMETERS Parameters;
    Parameters.Input.TokenHandle = HandleToULong(TokenHandle);
    THROW_IF_NTSTATUS_FAILED(LxBusClientMarshalForkToken(m_messagePort.get(), &Parameters));

    return Parameters.Output.ForkTokenId;
}

LXBUS_IPC_HANDLE_ID
LxssMessagePort::MarshalHandle(_In_ PCLXBUS_IPC_MESSAGE_MARSHAL_HANDLE_DATA HandleData) const
{
    LXBUS_IPC_MESSAGE_MARSHAL_HANDLE_PARAMETERS Parameters;
    Parameters.Input.HandleData = *HandleData;
    THROW_IF_NTSTATUS_FAILED(LxBusClientMarshalHandle(m_messagePort.get(), &Parameters));

    return Parameters.Output.HandleId;
}

LXBUS_IPC_PROCESS_ID
LxssMessagePort::MarshalProcess(_In_ HANDLE ProcessHandle, _In_ bool TerminateOnClose) const
{
    LXBUS_IPC_MESSAGE_MARSHAL_PROCESS_PARAMETERS Parameters;
    Parameters.Input.Process = HandleToULong(ProcessHandle);
    if (TerminateOnClose)
    {
        Parameters.Input.Flags = LXBUS_IPC_MARSHAL_PROCESS_FLAG_TERMINATE_ON_CLOSE;
    }

    THROW_IF_NTSTATUS_FAILED(LxBusClientMarshalProcess(m_messagePort.get(), &Parameters));

    return Parameters.Output.ProcessId;
}

void LxssMessagePort::Receive(_Out_writes_bytes_(Length) PVOID Buffer, _In_ ULONG Length, _In_opt_ HANDLE, _In_ DWORD Timeout)
{
    IO_STATUS_BLOCK IoStatus;
    ULONG SizeReceived;
    const NTSTATUS Status =
        LxBusClientReceiveMessageAsync(m_messagePort.get(), Buffer, Length, &SizeReceived, &IoStatus, m_messageEvent.get());
    THROW_IF_NTSTATUS_FAILED(Status);

    if (Status == STATUS_PENDING)
    {
        WaitForMessage(&IoStatus, Timeout);
    }
    else
    {
        WI_ASSERT(Status == STATUS_SUCCESS);
    }

    THROW_IF_NTSTATUS_FAILED(IoStatus.Status);
    THROW_HR_IF(E_UNEXPECTED, ((NT_SUCCESS(IoStatus.Status)) && (Length != static_cast<ULONG>(IoStatus.Information))));

    return;
}

std::vector<gsl::byte> LxssMessagePort::Receive(DWORD Timeout)
{
    IO_STATUS_BLOCK IoStatus;
    std::vector<gsl::byte> Message;
    ULONG SizeReceived;
    NTSTATUS Status;
    Message.resize(LAUNCH_PROCESS_DEFAULT_BUFFER_SIZE);
    for (;;)
    {
        Status = LxBusClientReceiveMessageAsync(
            m_messagePort.get(), Message.data(), static_cast<ULONG>(Message.size()), &SizeReceived, &IoStatus, m_messageEvent.get());

        if (Status == STATUS_PENDING)
        {
            WaitForMessage(&IoStatus, Timeout);
            Status = IoStatus.Status;
            SizeReceived = static_cast<ULONG>(IoStatus.Information);
        }

        //
        // Grow the buffer if it was not large enough.
        //
        // N.B. When a provided buffer is too small, LxBus will write the
        //      required size of the buffer as a SIZE_T into the beginning of
        //      the buffer.
        //

        if (Status == STATUS_BUFFER_TOO_SMALL)
        {
            Message.resize(*((PSIZE_T)Message.data()));
        }
        else
        {
            break;
        }
    }

    THROW_IF_NTSTATUS_FAILED(Status);

    //
    // Resize the buffer to be the size of the received message.
    //

    Message.resize(SizeReceived);
    return Message;
}

void LxssMessagePort::ReleaseConsole(_In_ LXBUS_IPC_CONSOLE_ID ConsoleId) const
{
    LXBUS_IPC_MESSAGE_IOCTL_CANCEL_MARSHAL_PARAMETERS Parameters;
    Parameters.Input.Id.Console = ConsoleId;
    Parameters.Input.Type = LxBusIpcReleaseTypeConsole;
    THROW_IF_NTSTATUS_FAILED(LxBusClientReleaseConsole(m_messagePort.get(), &Parameters));
}

void LxssMessagePort::ReleaseForkToken(_In_ LXBUS_IPC_FORK_TOKEN_ID ForkTokenId) const
{
    LXBUS_IPC_MESSAGE_IOCTL_CANCEL_MARSHAL_PARAMETERS Parameters;
    Parameters.Input.Id.Token = ForkTokenId;
    Parameters.Input.Type = LxBusIpcReleaseTypeForkToken;
    THROW_IF_NTSTATUS_FAILED(LxBusClientReleaseHandle(m_messagePort.get(), &Parameters));
}

void LxssMessagePort::ReleaseHandle(_In_ LXBUS_IPC_HANDLE_ID HandleId) const
{
    LXBUS_IPC_MESSAGE_IOCTL_CANCEL_MARSHAL_PARAMETERS Parameters;
    Parameters.Input.Id.Handle = HandleId;
    Parameters.Input.Type = LxBusIpcReleaseTypeHandle;
    THROW_IF_NTSTATUS_FAILED(LxBusClientReleaseHandle(m_messagePort.get(), &Parameters));
}

void LxssMessagePort::Send(_In_reads_bytes_(Length) PVOID Buffer, _In_ ULONG Length)
{
    IO_STATUS_BLOCK IoStatus;
    const NTSTATUS Status = LxBusClientSendMessageAsync(m_messagePort.get(), Buffer, Length, &IoStatus, m_messageEvent.get());
    THROW_IF_NTSTATUS_FAILED(Status);

    if (Status == STATUS_PENDING)
    {
        WaitForMessage(&IoStatus);
    }
    else
    {
        WI_ASSERT(Status == STATUS_SUCCESS);
    }

    THROW_IF_NTSTATUS_FAILED(IoStatus.Status);

    WI_ASSERT((Status != STATUS_SUCCESS) || (Length == IoStatus.Information));
}

void LxssMessagePort::SetServerPort(_In_ const std::shared_ptr<LxssServerPort>& ServerPort)
{
    m_serverPort = ServerPort;
}

wil::unique_handle LxssMessagePort::UnmarshalProcess(_In_ LXBUS_IPC_PROCESS_ID ProcessId) const
{
    LXBUS_IPC_MESSAGE_UNMARSHAL_PROCESS_PARAMETERS Parameters;
    Parameters.Input.ProcessId = ProcessId;
    THROW_IF_NTSTATUS_FAILED(LxBusClientUnmarshalProcess(m_messagePort.get(), &Parameters));

    wil::unique_handle ProcessHandle(ULongToHandle(Parameters.Output.ProcessHandle));
    return ProcessHandle;
}

wil::unique_handle LxssMessagePort::UnmarshalVfsFile(_In_ LXBUS_IPC_HANDLE_ID VfsFileId) const
{
    LXBUS_IPC_MESSAGE_UNMARSHAL_VFS_FILE_PARAMETERS Parameters;
    Parameters.Input.VfsFileId = VfsFileId;
    THROW_IF_NTSTATUS_FAILED(LxBusClientUnmarshalVfsFile(m_messagePort.get(), &Parameters));

    wil::unique_handle ProcessHandle(ULongToHandle(Parameters.Output.Handle));
    return ProcessHandle;
}

void LxssMessagePort::WaitForMessage(_In_ PIO_STATUS_BLOCK IoStatus, _In_ DWORD Timeout) const
{
    const DWORD WaitStatus = WaitForSingleObject(m_messageEvent.get(), Timeout);
    if (WaitStatus == WAIT_TIMEOUT)
    {
        IO_STATUS_BLOCK IoStatusCancel;
        const NTSTATUS Status = NtCancelIoFileEx(m_messagePort.get(), IoStatus, &IoStatusCancel);

        WI_ASSERT((Status == STATUS_SUCCESS) || (Status == STATUS_NOT_FOUND));

        WI_VERIFY(WaitForSingleObject(m_messageEvent.get(), Timeout) == WAIT_OBJECT_0);
    }
    else
    {
        WI_ASSERT(WaitStatus == WAIT_OBJECT_0);
    }
}
