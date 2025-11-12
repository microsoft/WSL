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

    RunningWSLAProcess(wil::com_ptr<IWSLAProcess>&& process, std::vector<WSLA_PROCESS_FD>&& fds);
    ProcessResult WaitAndCaptureOutput(DWORD TimeoutMs = INFINITE, std::vector<std::unique_ptr<relay::OverlappedIOHandle>>&& ExtraHandles = {});

    IWSLAProcess& Get();

private:
    wil::com_ptr<IWSLAProcess> m_process;
    std::vector<WSLA_PROCESS_FD> m_fds;
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

    // TODO: Add overloads for IWSLAContainer once implemented.
    RunningWSLAProcess Launch(IWSLASession& Session);
    std::tuple<HRESULT, int, std::optional<RunningWSLAProcess>> LaunchNoThrow(IWSLASession& Session);

private:
    std::tuple<WSLA_PROCESS_OPTIONS, std::vector<const char*>, std::vector<const char*>> CreateProcessOptions();

    std::vector<WSLA_PROCESS_FD> m_fds;
    std::string m_executable;
    std::vector<std::string> m_arguments;
    std::vector<std::string> m_environment;
};

} // namespace wsl::windows::common