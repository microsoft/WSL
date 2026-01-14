/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ServiceProcessLauncher.h

Abstract:

    This file contains the definitions for ServiceProcessLauncher and ServiceRunningProcess.

--*/

#pragma once
#include "WSLAProcessLauncher.h"
#include "WSLAProcess.h"

namespace wsl::windows::service::wsla {

class WSLAVirtualMachine;

class ServiceRunningProcess : public common::RunningWSLAProcess
{
public:
    NON_COPYABLE(ServiceRunningProcess);
    DEFAULT_MOVABLE(ServiceRunningProcess);

    ServiceRunningProcess(const Microsoft::WRL::ComPtr<WSLAProcess>& process, std::vector<WSLA_PROCESS_FD>&& fds);
    wil::unique_handle GetStdHandle(int Index) override;
    wil::unique_event GetExitEvent() override;
    WSLAProcess& Get();

protected:
    void GetState(WSLA_PROCESS_STATE* State, int* Code) override;

private:
    Microsoft::WRL::ComPtr<WSLAProcess> m_process;
};

class ServiceProcessLauncher : public common::WSLAProcessLauncher
{
public:
    NON_COPYABLE(ServiceProcessLauncher);
    NON_MOVABLE(ServiceProcessLauncher);
    using WSLAProcessLauncher::WSLAProcessLauncher;

    std::tuple<HRESULT, int, std::optional<ServiceRunningProcess>> LaunchNoThrow(WSLAVirtualMachine& virtualMachine);
    ServiceRunningProcess Launch(WSLAVirtualMachine& virtualMachine);
};
} // namespace wsl::windows::service::wsla