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

enum class ProcessFlags
{
    None = 0,
    Stdin = 1,
    Stdout = 2,
    Stderr = 4,
};

DEFINE_ENUM_FLAG_OPERATORS(ProcessFlags);

class RunningWSLAProcess
{
public:
    struct ProcessResult
    {
        int Code;
        bool Signalled;
        std::map<int, std::string> Output;
    };

    RunningWSLAProcess(std::vector<WSLA_PROCESS_FD>&& fds);
    NON_COPYABLE(RunningWSLAProcess);
    DEFAULT_MOVABLE(RunningWSLAProcess);

    ProcessResult WaitAndCaptureOutput(DWORD TimeoutMs = INFINITE, std::vector<std::unique_ptr<relay::OverlappedIOHandle>>&& ExtraHandles = {});
    std::pair<int, bool> Wait(DWORD TimeoutMs = INFINITE);
    virtual wil::unique_handle GetStdHandle(int Index) = 0;
    virtual wil::unique_event GetExitEvent() = 0;
    std::pair<int, bool> GetExitState();
    WSLA_PROCESS_STATE State();

protected:
    virtual void GetState(WSLA_PROCESS_STATE* State, int* Code) = 0;

    std::vector<WSLA_PROCESS_FD> m_fds;
};

class ClientRunningWSLAProcess : public RunningWSLAProcess
{
public:
    NON_COPYABLE(ClientRunningWSLAProcess);
    DEFAULT_MOVABLE(ClientRunningWSLAProcess);

    ClientRunningWSLAProcess(wil::com_ptr<IWSLAProcess>&& process, std::vector<WSLA_PROCESS_FD>&& fds);
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
        ProcessFlags Flags = ProcessFlags::Stdout | ProcessFlags::Stderr);

    void AddFd(WSLA_PROCESS_FD Fd);
    void SetTtySize(ULONG Rows, ULONG Columns);

    std::tuple<HRESULT, int, std::optional<ClientRunningWSLAProcess>> LaunchNoThrow(IWSLASession& Session);
    std::tuple<HRESULT, int, std::optional<ClientRunningWSLAProcess>> LaunchNoThrow(IWSLAContainer& Container);
    std::string FormatResult(const RunningWSLAProcess::ProcessResult& IWSLAContainer);

    template <typename T>
    ClientRunningWSLAProcess Launch(T& Session)
    {
        auto [hresult, error, process] = LaunchNoThrow(Session);
        if (FAILED(hresult))
        {
            auto commandLine = wsl::shared::string::Join(m_arguments, ' ');
            THROW_HR_MSG(
                hresult, "Failed to launch process: %hs (commandline: %hs). Errno = %i", m_executable.c_str(), commandLine.c_str(), error);
        }

        return std::move(process.value());
    }

protected:
    std::tuple<WSLA_PROCESS_OPTIONS, std::vector<const char*>, std::vector<const char*>> CreateProcessOptions();

    std::vector<WSLA_PROCESS_FD> m_fds;
    std::string m_executable;
    std::vector<std::string> m_arguments;
    std::vector<std::string> m_environment;
    DWORD m_rows = 0;
    DWORD m_columns = 0;
};

} // namespace wsl::windows::common