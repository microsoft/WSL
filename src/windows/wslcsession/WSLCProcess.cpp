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

WSLCProcess::WSLCProcess(std::shared_ptr<WSLCProcessControl> Control, std::unique_ptr<WSLCProcessIO>&& Io, WSLCProcessFlags Flags) :
    m_control(std::move(Control)), m_io(std::move(Io)), m_flags(Flags)
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

HRESULT WSLCProcess::GetStdHandle(WSLCFD Fd, WSLCHandle* Handle)
try
{
    RETURN_HR_IF_MSG(HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED), !m_io, "Process IO not attached");

    auto typedHandle = m_io->OpenFd(Fd);

    RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !typedHandle.is_valid());

    DWORD Access = SYNCHRONIZE;

    WI_SetFlagIf(Access, GENERIC_WRITE, Fd == WSLCFDTty || Fd == WSLCFDStdin);
    WI_SetFlagIf(Access, GENERIC_READ, Fd == WSLCFDStdout || Fd == WSLCFDStderr || Fd == WSLCFDTty);

    *Handle = common::wslutil::ToCOMOutputHandle(typedHandle.get(), Access, typedHandle.Type);

    WSL_LOG(
        "GetStdHandle",
        TraceLoggingValue(static_cast<int>(Fd), "fd"),
        TraceLoggingValue(typedHandle.get(), "handle"),
        TraceLoggingValue(static_cast<int>(Handle->Type), "type"));

    return S_OK;
}
CATCH_RETURN();

HRESULT WSLCProcess::GetFlags(WSLCProcessFlags* Flags)
try
{
    *Flags = m_flags;
    return S_OK;
}
CATCH_RETURN();

wil::unique_handle WSLCProcess::GetStdHandle(int Index)
{
    THROW_WIN32_IF(ERROR_INVALID_STATE, !m_io);

    return std::move(m_io->OpenFd(Index).Handle);
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