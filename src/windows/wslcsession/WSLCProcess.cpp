/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCProcess.cpp

Abstract:

    Contains the implementation of WSLCProcess.

--*/

#include "precomp.h"
#include "WSLCProcess.h"
#include "WSLCVirtualMachine.h"

using wsl::windows::service::wslc::WSLCProcess;

WSLCProcess::WSLCProcess(std::unique_ptr<WSLCProcessControl>&& Control, std::unique_ptr<WSLCProcessIO>&& Io) :
    m_control(std::move(Control)), m_io(std::move(Io))
{
}

HRESULT WSLCProcess::Signal(int Signal)
try
{
    m_control->Signal(Signal);
    return S_OK;
}
CATCH_RETURN();

HRESULT WSLCProcess::GetExitEvent(HANDLE* Event)
try
{
    *Event = wsl::windows::common::wslutil::DuplicateHandle(m_control->GetExitEvent().get(), SYNCHRONIZE, FALSE);
    return S_OK;
}
CATCH_RETURN();

HRESULT WSLCProcess::GetStdHandle(ULONG Index, HANDLE* PipeHandle, HANDLE* SocketHandle)
try
{
    RETURN_HR_IF_MSG(HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED), !m_io, "Process IO not attached");

    auto handle = m_io->OpenFd(Index);

    RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !handle.is_valid());

    // Route to the correct out param based on the underlying handle type.
    // RelayedProcessIO (container) uses pipes; TTYProcessIO and VMProcessIO use sockets.
    if (m_io->IsSocketBased())
    {
        *SocketHandle = handle.release();
    }
    else
    {
        *PipeHandle = handle.release();
    }

    WSL_LOG(
        "GetStdHandle",
        TraceLoggingValue(Index, "fd"),
        TraceLoggingValue(m_io->IsSocketBased(), "isSocket"),
        TraceLoggingValue(*PipeHandle ? *PipeHandle : *SocketHandle, "handle"));

    return S_OK;
}
CATCH_RETURN();

wil::unique_handle WSLCProcess::GetStdHandle(int Index)
{
    THROW_WIN32_IF(ERROR_INVALID_STATE, !m_io);

    return m_io->OpenFd(Index);
}

HANDLE WSLCProcess::GetExitEvent()
{
    return m_control->GetExitEvent().get();
}

HRESULT WSLCProcess::GetPid(int* Pid)
try
{
    *Pid = m_control->GetPid();
    return S_OK;
}
CATCH_RETURN();

int WSLCProcess::GetPid() const
{
    return m_control->GetPid();
}

HRESULT WSLCProcess::GetState(WSLCProcessState* State, int* Code)
try
{
    std::tie(*State, *Code) = m_control->GetState();
    return S_OK;
}
CATCH_RETURN();

HRESULT WSLCProcess::ResizeTty(ULONG Rows, ULONG Columns)
try
{
    m_control->ResizeTty(Rows, Columns);
    return S_OK;
}
CATCH_RETURN();