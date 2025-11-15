/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ServiceProcessLauncher.cpp

Abstract:

    This file contains the implementations of ServiceProcessLauncher and ServiceRunningProcess.

--*/

#include "ServiceProcessLauncher.h"
#include "WSLAVirtualMachine.h"

using wsl::windows::service::wsla::ServiceProcessLauncher;
using wsl::windows::service::wsla::ServiceRunningProcess;
using wsl::windows::service::wsla::WSLAProcess;

ServiceRunningProcess::ServiceRunningProcess(const Microsoft::WRL::ComPtr<WSLAProcess>& process, std::vector<WSLA_PROCESS_FD>&& fds) :
    common::RunningWSLAProcess(std::move(fds))
{
    process.CopyTo(m_process.GetAddressOf());
}

wil::unique_handle ServiceRunningProcess::GetStdHandle(int Index)
{
    return std::move(Get().GetStdHandle(Index));
}

wil::unique_event ServiceRunningProcess::GetExitEvent()
{
    // Unlike for std handles, the event handle needs to be duplicated, since we need to keep a reference to it
    // to signal it once the process exits.
    wil::unique_event event;
    THROW_IF_WIN32_BOOL_FALSE(
        DuplicateHandle(GetCurrentProcess(), m_process->GetExitEvent().get(), GetCurrentProcess(), &event, SYNCHRONIZE, false, 0));

    return event;
}

WSLAProcess& ServiceRunningProcess::Get()
{
    return *m_process.Get();
}

void ServiceRunningProcess::GetState(WSLA_PROCESS_STATE* State, int* Code)
{
    THROW_IF_FAILED(m_process->GetState(State, Code));
}

ServiceRunningProcess ServiceProcessLauncher::Launch(WSLAVirtualMachine& virtualMachine)
{
    auto [options, commandLine, env] = CreateProcessOptions();
    int Error = -1;

    return ServiceRunningProcess(virtualMachine.CreateLinuxProcess(options, &Error), std::move(m_fds));
}
