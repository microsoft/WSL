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

ServiceRunningProcess::ServiceRunningProcess(const Microsoft::WRL::ComPtr<WSLAProcess>& process, WSLAProcessFlags flags) :
    common::RunningWSLAProcess(flags)
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
    return wil::unique_event{wsl::windows::common::helpers::DuplicateHandle(m_process->GetExitEvent())};
}

WSLAProcess& ServiceRunningProcess::Get()
{
    return *m_process.Get();
}

void ServiceRunningProcess::GetState(WSLA_PROCESS_STATE* State, int* Code)
{
    THROW_IF_FAILED(m_process->GetState(State, Code));
}

std::tuple<HRESULT, int, std::optional<ServiceRunningProcess>> ServiceProcessLauncher::LaunchNoThrow(WSLAVirtualMachine& virtualMachine)
{
    auto [options, commandLine, env] = CreateProcessOptions();
    int error = -1;

    std::optional<ServiceRunningProcess> process;
    auto result = wil::ResultFromException(
        [&]() { process.emplace(virtualMachine.CreateLinuxProcess(m_executable.c_str(), options, &error), m_flags); });

    return {result, error, std::move(process)};
}

ServiceRunningProcess ServiceProcessLauncher::Launch(WSLAVirtualMachine& virtualMachine)
{
    auto [hresult, error, process] = LaunchNoThrow(virtualMachine);
    if (FAILED(hresult))
    {
        auto commandLine = wsl::shared::string::Join(m_arguments, ' ');
        THROW_HR_MSG(hresult, "Failed to launch process: %hs (commandline: %hs). Errno = %i", m_executable.c_str(), commandLine.c_str(), error);
    }

    return std::move(process.value());
}