/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCProcessLauncher.h

Abstract:

    Helper class to launch and wait for WSLC processes.
    This is designed to function both for VM level and container level processes.
    This class is also designed to work both from client & server side.

--*/

#pragma once
#include "wslc.h"
#include <variant>
#include <vector>
#include <string>

namespace wsl::windows::common {

class RunningWSLCProcess
{
public:
    struct ProcessResult
    {
        int Code;
        std::map<int, std::string> Output;
    };

    RunningWSLCProcess(WSLCProcessFlags Flags);
    NON_COPYABLE(RunningWSLCProcess);
    DEFAULT_MOVABLE(RunningWSLCProcess);

    ProcessResult WaitAndCaptureOutput(DWORD TimeoutMs = INFINITE, std::vector<std::unique_ptr<relay::OverlappedIOHandle>>&& ExtraHandles = {});
    int Wait(DWORD TimeoutMs = INFINITE);
    virtual wil::unique_handle GetStdHandle(int Index) = 0;
    virtual wil::unique_event GetExitEvent() = 0;
    int GetExitCode();
    WSLCProcessState State();

    WSLCProcessFlags Flags() const;

protected:
    virtual void GetState(WSLCProcessState* State, int* Code) = 0;

    WSLCProcessFlags m_flags{};
};

class ClientRunningWSLCProcess : public RunningWSLCProcess
{
public:
    NON_COPYABLE(ClientRunningWSLCProcess);
    DEFAULT_MOVABLE(ClientRunningWSLCProcess);

    ClientRunningWSLCProcess(wil::com_ptr<IWSLCProcess>&& process, WSLCProcessFlags Flags);
    wil::unique_handle GetStdHandle(int Index) override;
    wil::unique_event GetExitEvent() override;
    IWSLCProcess& Get();

protected:
    void GetState(WSLCProcessState* State, int* Code) override;

private:
    wil::com_ptr<IWSLCProcess> m_process;
};
class WSLCProcessLauncher
{
public:
    NON_COPYABLE(WSLCProcessLauncher);
    NON_MOVABLE(WSLCProcessLauncher);

    WSLCProcessLauncher(
        const std::string& Executable,
        const std::vector<std::string>& Arguments,
        const std::vector<std::string>& Environment = {},
        WSLCProcessFlags = WSLCProcessFlagsNone);

    void SetTtySize(ULONG Rows, ULONG Columns);
    void SetWorkingDirectory(std::string&& WorkingDirectory);
    void SetUser(std::string&& User);
    void SetDetachKeys(std::string&& DetachKeys);

    std::tuple<HRESULT, std::optional<ClientRunningWSLCProcess>, int> LaunchNoThrow(IWSLCSession& Session);
    std::tuple<HRESULT, std::optional<ClientRunningWSLCProcess>> LaunchNoThrow(IWSLCContainer& Container);

    template <typename T>
    auto Launch(T& Context)
    {
        auto result = LaunchNoThrow(Context);
        THROW_IF_FAILED(std::get<0>(result));

        return std::move(std::get<1>(result).value());
    }

    std::string FormatResult(const RunningWSLCProcess::ProcessResult& result);
    std::string FormatResult(const int code);

protected:
    std::tuple<WSLCProcessOptions, std::vector<const char*>, std::vector<const char*>> CreateProcessOptions();

    WSLCProcessFlags m_flags{};
    std::string m_executable;
    std::string m_workingDirectory;
    std::string m_user;
    std::optional<std::string> m_detachKeys;
    std::vector<std::string> m_arguments;
    std::vector<std::string> m_environment;
    DWORD m_rows = 0;
    DWORD m_columns = 0;
};

} // namespace wsl::windows::common