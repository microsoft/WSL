/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLAProcessLauncher.h

Abstract:

    Helper class to launch and wait for WSLA processes.
    This is designed to function both for VM level and container level processes.
    This class is also designed to work both from client & server side.

--*/

#pragma once
#include "wslaservice.h"
#include <variant>
#include <vector>
#include <string>

namespace wsl::windows::common {

class RunningWSLAProcess
{
public:
    struct ProcessResult
    {
        int Code;
        std::map<int, std::string> Output;
    };

    RunningWSLAProcess(WSLAProcessFlags Flags);
    NON_COPYABLE(RunningWSLAProcess);
    DEFAULT_MOVABLE(RunningWSLAProcess);

    ProcessResult WaitAndCaptureOutput(DWORD TimeoutMs = INFINITE, std::vector<std::unique_ptr<relay::OverlappedIOHandle>>&& ExtraHandles = {});
    int Wait(DWORD TimeoutMs = INFINITE);
    virtual wil::unique_handle GetStdHandle(int Index) = 0;
    virtual wil::unique_event GetExitEvent() = 0;
    int GetExitCode();
    WSLA_PROCESS_STATE State();

    WSLAProcessFlags Flags() const;

protected:
    virtual void GetState(WSLA_PROCESS_STATE* State, int* Code) = 0;

    WSLAProcessFlags m_flags{};
};

class ClientRunningWSLAProcess : public RunningWSLAProcess
{
public:
    NON_COPYABLE(ClientRunningWSLAProcess);
    DEFAULT_MOVABLE(ClientRunningWSLAProcess);

    ClientRunningWSLAProcess(wil::com_ptr<IWSLAProcess>&& process, WSLAProcessFlags Flags);
    wil::unique_handle GetStdHandle(int Index) override;
    wil::unique_event GetExitEvent() override;
    IWSLAProcess& Get();

protected:
    void GetState(WSLA_PROCESS_STATE* State, int* Code) override;

private:
    wil::com_ptr<IWSLAProcess> m_process;
};
class WSLAProcessLauncher
{
public:
    NON_COPYABLE(WSLAProcessLauncher);
    NON_MOVABLE(WSLAProcessLauncher);

    WSLAProcessLauncher(
        const std::string& Executable,
        const std::vector<std::string>& Arguments,
        const std::vector<std::string>& Environment = {},
        WSLAProcessFlags = WSLAProcessFlagsNone);

    void SetTtySize(ULONG Rows, ULONG Columns);
    void SetWorkingDirectory(std::string&& WorkingDirectory);
    void SetUser(std::string&& User);

    std::tuple<HRESULT, int, std::optional<ClientRunningWSLAProcess>> LaunchNoThrow(IWSLASession& Session);
    std::tuple<HRESULT, int, std::optional<ClientRunningWSLAProcess>> LaunchNoThrow(IWSLAContainer& Container);

    template <typename T>
    auto Launch(T& Context)
    {
        auto [hresult, error, process] = LaunchNoThrow(Context);
        if (FAILED(hresult))
        {
            auto commandLine = wsl::shared::string::Join(m_arguments, ' ');
            THROW_HR_MSG(
                hresult, "Failed to launch process: %hs (commandline: %hs). Errno = %i", m_executable.c_str(), commandLine.c_str(), error);
        }

        return std::move(process.value());
    }

    std::string FormatResult(const RunningWSLAProcess::ProcessResult& result);
    std::string FormatResult(const int code);

protected:
    std::tuple<WSLA_PROCESS_OPTIONS, std::vector<const char*>, std::vector<const char*>> CreateProcessOptions();

    WSLAProcessFlags m_flags{};
    std::string m_executable;
    std::string m_workingDirectory;
    std::string m_user;
    std::vector<std::string> m_arguments;
    std::vector<std::string> m_environment;
    DWORD m_rows = 0;
    DWORD m_columns = 0;
};

} // namespace wsl::windows::common