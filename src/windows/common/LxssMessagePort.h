/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    LxssMessagePort.h

Abstract:

    This file contains declarations for the message port wrapper class.

--*/

#pragma once

#include "LxssPort.h"

class LxssServerPort;

class LxssMessagePort : public LxssPort
{
public:
    static inline DWORD c_defaultMessageTimeout = 30000;

    LxssMessagePort(_In_ HANDLE MessagePort);
    LxssMessagePort(_In_ LxssMessagePort&& Source);
    LxssMessagePort(_In_ std::unique_ptr<LxssMessagePort>&& SourcePointer);
    virtual ~LxssMessagePort() = default;

    std::shared_ptr<LxssPort> CreateSessionLeader(_In_ HANDLE ClientProcess) override;
    void DisconnectConsole(_In_ HANDLE ClientProcess) override;
    wil::cs_leave_scope_exit Lock() override;
    void Receive(_Out_writes_bytes_(Length) PVOID Buffer, _In_ ULONG Length, _In_opt_ HANDLE ClientProcess = nullptr, _In_ DWORD Timeout = INFINITE) override;
    void Send(_In_reads_bytes_(Length) PVOID Buffer, _In_ ULONG Length) override;

    wil::unique_handle CreateUnnamedServer(_Out_ PLXBUS_SERVER_ID ServerId) const;

    LXBUS_IPC_CONSOLE_ID
    MarshalConsole(_In_ PCLXBUS_IPC_MESSAGE_MARSHAL_CONSOLE_DATA ConsoleData) const;

    LXBUS_IPC_FORK_TOKEN_ID
    MarshalForkToken(_In_ HANDLE TokenHandle) const;

    LXBUS_IPC_HANDLE_ID
    MarshalHandle(_In_ PCLXBUS_IPC_MESSAGE_MARSHAL_HANDLE_DATA HandleData) const;

    LXBUS_IPC_PROCESS_ID
    MarshalProcess(_In_ HANDLE ProcessHandle, _In_ bool TerminateOnClose) const;

    std::vector<gsl::byte> Receive(DWORD Timeout = c_defaultMessageTimeout);

    void ReleaseConsole(_In_ LXBUS_IPC_CONSOLE_ID ConsoleId) const;

    void ReleaseForkToken(_In_ LXBUS_IPC_FORK_TOKEN_ID HandleId) const;

    void ReleaseHandle(_In_ LXBUS_IPC_HANDLE_ID HandleId) const;

    void SetServerPort(_In_ const std::shared_ptr<LxssServerPort>& ServerPort);

    wil::unique_handle UnmarshalProcess(_In_ LXBUS_IPC_PROCESS_ID ProcessId) const;

    wil::unique_handle UnmarshalVfsFile(_In_ LXBUS_IPC_HANDLE_ID VfsFileId) const;

private:
    void WaitForMessage(_In_ PIO_STATUS_BLOCK IoStatus, _In_ DWORD Timeout = INFINITE) const;

    wil::critical_section m_lock;
    wil::unique_handle m_messagePort;
    wil::unique_event m_messageEvent;
    std::shared_ptr<LxssServerPort> m_serverPort;
};
