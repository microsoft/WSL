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

WSLAProcess::WSLAProcess(std::map<int, wil::unique_handle>&& handles, int pid, WSLAVirtualMachine* virtualMachine) :
    m_handles(std::move(handles)), m_pid(pid), m_virtualMachine(virtualMachine)
{
}

WSLAProcess::~WSLAProcess()
{
    std::lock_guard lock{m_mutex};
    if (m_virtualMachine != nullptr)
    {
        m_virtualMachine->OnProcessReleased(m_pid);
    }
}

void WSLAProcess::OnVmTerminated()
{
    WI_ASSERT(m_virtualMachine != nullptr);

    std::lock_guard lock{m_mutex};
    m_virtualMachine = nullptr;

    // Make sure that the process is in a terminated state, so users don't think that it might still be running.
    if (m_state == WslaProcessStateRunning)
    {
        m_state = WslaProcessStateSignalled;
        m_exitedCode = 9; // SIGKILL

        m_exitEvent.SetEvent();
    }
}

HRESULT WSLAProcess::Signal(int Signal)
try
{
    std::lock_guard lock{m_mutex};
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), m_state != WslaProcessStateRunning || m_virtualMachine == nullptr);

    return m_virtualMachine->Signal(m_pid, Signal);
}
CATCH_RETURN();

HRESULT WSLAProcess::GetExitEvent(ULONG* Event)
try
{
    *Event = HandleToUlong(common::wslutil::DuplicateHandleToCallingProcess(m_exitEvent.get()));
    return S_OK;
}
CATCH_RETURN();

HRESULT WSLAProcess::GetStdHandle(ULONG Index, ULONG* Handle)
try
{
    std::lock_guard lock{m_mutex};

    auto& socket = GetStdHandle(Index);
    RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !socket.is_valid());

    *Handle = HandleToUlong(common::wslutil::DuplicateHandleToCallingProcess(socket.get()));
    WSL_LOG(
        "GetStdHandle",
        TraceLoggingValue(Index, "fd"),
        TraceLoggingValue(socket.get(), "handle"),
        TraceLoggingValue(*Handle, "remoteHandle"));

    socket.reset();
    return S_OK;
}
CATCH_RETURN();

wil::unique_handle& WSLAProcess::GetStdHandle(int Index)
{
    std::lock_guard lock{m_mutex};

    auto it = m_handles.find(Index);
    THROW_HR_IF_MSG(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), it == m_handles.end(), "Pid: %i, Fd: %i", m_pid, Index);

    return it->second;
}

wil::unique_event& WSLAProcess::GetExitEvent()
{
    return m_exitEvent;
}

HRESULT WSLAProcess::GetPid(int* Pid)
try
{
    // m_pid is immutable, so m_mutex doesn't need to be acquired.

    // TODO: Container processes should return the container pid, and not the root namespace pid.
    *Pid = m_pid;
    return S_OK;
}
CATCH_RETURN();

int WSLAProcess::GetPid() const
{
    // m_pid is immutable, so m_mutex doesn't need to be acquired.
    return m_pid;
}

HRESULT WSLAProcess::GetState(WSLA_PROCESS_STATE* State, int* Code)
try
{
    std::lock_guard lock{m_mutex};

    *State = m_state;
    *Code = m_exitedCode;

    return S_OK;
}
CATCH_RETURN();

HRESULT WSLAProcess::ResizeTty(ULONG Rows, ULONG Columns)
{
    return E_NOTIMPL;
}

void WSLAProcess::OnTerminated(bool Signalled, int Code)
{
    WI_ASSERT(m_virtualMachine != nullptr);

    {
        std::lock_guard lock{m_mutex};
        THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), m_state != WslaProcessStateRunning);

        if (Signalled)
        {
            m_state = WslaProcessStateSignalled;
        }
        else
        {
            m_state = WslaProcessStateExited;
        }

        m_exitedCode = Code;
    }

    m_exitEvent.SetEvent();
}
