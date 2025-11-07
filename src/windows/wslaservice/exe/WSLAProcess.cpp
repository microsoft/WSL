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

    m_virtualMachine->OnProcessReleased(m_pid);
}

HRESULT WSLAProcess::Signal(int Signal)
{
    std::lock_guard lock{m_mutex};
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), m_state != WslaProcessStateRunning || m_virtualMachine == nullptr);

    return m_virtualMachine->Signal(m_pid, Signal);
}

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
    // m_handles is immutable, so m_mutex doesn't need to be acquired.

    auto it = m_handles.find(Index);
    if (it == m_handles.end())
    {
        RETURN_HR(HRESULT_FROM_WIN32(ERROR_NOT_FOUND));
    }

    *Handle = HandleToUlong(common::wslutil::DuplicateHandleToCallingProcess(it->second.get()));

    return S_OK;
}
CATCH_RETURN();

HRESULT WSLAProcess::GetPid(int* Pid)
try
{
    // m_pid is immutable, so m_mutex doesn't need to be acquired.

    // TODO: Container processes should return the container pid, and not root namespace pid.
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

void WSLAProcess::OnTerminated(bool Signalled, int Code)
{
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
