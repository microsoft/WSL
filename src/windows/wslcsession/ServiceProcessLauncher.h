/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ServiceProcessLauncher.h

Abstract:

    This file contains the definitions for ServiceProcessLauncher and ServiceRunningProcess.

--*/

#pragma once
#include "WSLCProcessLauncher.h"
#include "WSLCProcess.h"

namespace wsl::windows::service::wslc {

class WSLCVirtualMachine;

class ServiceRunningProcess : public common::RunningWSLCProcess
{
public:
    NON_COPYABLE(ServiceRunningProcess);
    DEFAULT_MOVABLE(ServiceRunningProcess);

    ServiceRunningProcess(const Microsoft::WRL::ComPtr<WSLCProcess>& process, WSLCProcessFlags Flags);
    wil::unique_handle GetStdHandle(int Index) override;
    wil::unique_event GetExitEvent() override;
    WSLCProcess& Get();

protected:
    void GetState(WSLCProcessState* State, int* Code) override;

private:
    Microsoft::WRL::ComPtr<WSLCProcess> m_process;
};

class ServiceProcessLauncher : public common::WSLCProcessLauncher
{
public:
    NON_COPYABLE(ServiceProcessLauncher);
    NON_MOVABLE(ServiceProcessLauncher);
    using WSLCProcessLauncher::WSLCProcessLauncher;

    std::tuple<HRESULT, int, std::optional<ServiceRunningProcess>> LaunchNoThrow(WSLCVirtualMachine& virtualMachine);
    ServiceRunningProcess Launch(WSLCVirtualMachine& virtualMachine);
};
} // namespace wsl::windows::service::wslc