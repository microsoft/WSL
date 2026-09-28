/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    LxssPort.h

Abstract:

    This file contains declarations for the port abstract base class.

--*/

#pragma once

class LxssPort
{
public:
    virtual std::shared_ptr<LxssPort> CreateSessionLeader(_In_ HANDLE ClientProcess) = 0;
    virtual void DisconnectConsole(_In_ HANDLE ClientProcess) = 0;
    virtual wil::cs_leave_scope_exit Lock() = 0;
    virtual void Receive(_Out_writes_bytes_(Length) PVOID Buffer, _In_ ULONG Length, _In_opt_ HANDLE ClientProcess = nullptr, _In_ DWORD Timeout = INFINITE) = 0;
    virtual void Send(_In_reads_bytes_(Length) PVOID Buffer, _In_ ULONG Length) = 0;
};
