/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLAProcess.cpp

Abstract:

    Contains the implementation of WSLAProcess.

--*/

#include "precomp.h"
#include "WSLAProcess.h"
#include "WSLAVirtualMachine.h"

using wsl::windows::service::wsla::WSLAProcess;

WSLAProcess::WSLAProcess(std::unique_ptr<WSLAProcessControl>&& Control, std::unique_ptr<WSLAProcessIO>&& Io) :
    m_control(std::move(Control)), m_io(std::move(Io))
{
}

HRESULT WSLAProcess::Signal(int Signal)
try
{
    m_control->Signal(Signal);
    return S_OK;
}
CATCH_RETURN();

HRESULT WSLAProcess::GetExitEvent(ULONG* Event)
try
{
    *Event = HandleToUlong(common::wslutil::DuplicateHandleToCallingProcess(m_control->GetExitEvent().get(), SYNCHRONIZE));
    return S_OK;
}
CATCH_RETURN();

HRESULT WSLAProcess::GetStdHandle(ULONG Index, ULONG* Handle)
try
{
    RETURN_HR_IF_MSG(HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED), !m_io, "Process IO not attached");

    auto handle = m_io->OpenFd(Index);

    RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !handle.is_valid());

    *Handle = HandleToUlong(common::wslutil::DuplicateHandleToCallingProcess(handle.get()));

    WSL_LOG(
        "GetStdHandle",
        TraceLoggingValue(Index, "fd"),
        TraceLoggingValue(handle.get(), "handle"),
        TraceLoggingValue(*Handle, "remoteHandle"));

    return S_OK;
}
CATCH_RETURN();

wil::unique_handle WSLAProcess::GetStdHandle(int Index)
{
    THROW_WIN32_IF(ERROR_INVALID_STATE, !m_io);

    return m_io->OpenFd(Index);
}

HANDLE WSLAProcess::GetExitEvent()
{
    return m_control->GetExitEvent().get();
}

HRESULT WSLAProcess::GetPid(int* Pid)
try
{
    *Pid = m_control->GetPid();
    return S_OK;
}
CATCH_RETURN();

int WSLAProcess::GetPid() const
{
    return m_control->GetPid();
}

HRESULT WSLAProcess::GetState(WSLA_PROCESS_STATE* State, int* Code)
try
{
    std::tie(*State, *Code) = m_control->GetState();
    return S_OK;
}
CATCH_RETURN();

HRESULT WSLAProcess::ResizeTty(ULONG Rows, ULONG Columns)
try
{
    m_control->ResizeTty(Rows, Columns);
    return S_OK;
}
CATCH_RETURN();