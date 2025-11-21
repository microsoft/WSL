/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLAProcessLauncher.cpp

Abstract:

    WSLAProcessLauncher implementation.

--*/

#include <precomp.h>
#include "WSLAProcessLauncher.h"
#include "WSLAApi.h"

using wsl::windows::common::ClientRunningWSLAProcess;
using wsl::windows::common::RunningWSLAProcess;
using wsl::windows::common::WSLAProcessLauncher;

WSLAProcessLauncher::WSLAProcessLauncher(
    const std::string& Executable, const std::vector<std::string>& Arguments, const std::vector<std::string>& Environment, ProcessFlags Flags) :
    m_executable(Executable), m_arguments(Arguments), m_environment(Environment)
{
    // Add standard Fds.
    if (WI_IsFlagSet(Flags, ProcessFlags::Stdin))
    {
        m_fds.emplace_back(WSLA_PROCESS_FD{.Fd = 0, .Type = WSLAFdTypeDefault, .Path = nullptr});
    }

    if (WI_IsFlagSet(Flags, ProcessFlags::Stdout))
    {
        m_fds.emplace_back(WSLA_PROCESS_FD{.Fd = 1, .Type = WSLAFdTypeDefault, .Path = nullptr});
    }

    if (WI_IsFlagSet(Flags, ProcessFlags::Stderr))
    {
        m_fds.emplace_back(WSLA_PROCESS_FD{.Fd = 2, .Type = WSLAFdTypeDefault, .Path = nullptr});
    }
}

void WSLAProcessLauncher::AddFd(WSLA_PROCESS_FD Fd)
{
    WI_ASSERT(std::ranges::find_if(m_fds, [&](const auto& e) { return e.Fd == Fd.Fd; }) == m_fds.end());

    m_fds.push_back(Fd);
}

std::tuple<WSLA_PROCESS_OPTIONS, std::vector<const char*>, std::vector<const char*>> WSLAProcessLauncher::CreateProcessOptions()
{
    std::vector<const char*> commandLine;
    std::ranges::transform(m_arguments, std::back_inserter(commandLine), [](const std::string& e) { return e.c_str(); });

    std::vector<const char*> environment;
    std::ranges::transform(m_environment, std::back_inserter(environment), [](const std::string& e) { return e.c_str(); });

    WSLA_PROCESS_OPTIONS options{};
    options.Executable = m_executable.c_str();
    options.CommandLine = commandLine.data();
    options.CommandLineCount = static_cast<DWORD>(commandLine.size());
    options.Fds = m_fds.data();
    options.FdsCount = static_cast<DWORD>(m_fds.size());
    options.Environment = environment.data();
    options.EnvironmentCount = static_cast<DWORD>(environment.size());

    return std::make_tuple(options, std::move(commandLine), std::move(environment));
}

RunningWSLAProcess::RunningWSLAProcess(std::vector<WSLA_PROCESS_FD>&& fds) : m_fds(std::move(fds))
{
}

std::pair<int, bool> RunningWSLAProcess::GetExitState()
{
    WSLA_PROCESS_STATE state{};
    int code{};
    GetState(&state, &code);

    THROW_HR_IF_MSG(
        HRESULT_FROM_WIN32(ERROR_INVALID_STATE),
        state != WslaProcessStateSignalled && state != WslaProcessStateExited,
        "Process is not exited. State: %i",
        state);

    return {code, state == WslaProcessStateSignalled};
}

std::string WSLAProcessLauncher::FormatResult(const RunningWSLAProcess::ProcessResult& result)
{
    auto stdOut = result.Output.find(1);
    auto stdErr = result.Output.find(2);

    return std::format(
        "{} [{}] exited with: {}. Stdout: '{}', Stderr: '{}'",
        m_executable,
        wsl::shared::string::Join(m_arguments, ','),
        result.Code,
        stdOut != result.Output.end() ? stdOut->second : "<none>",
        stdErr != result.Output.end() ? stdErr->second : "<none>");
}

RunningWSLAProcess::ProcessResult RunningWSLAProcess::WaitAndCaptureOutput(DWORD TimeoutMs, std::vector<std::unique_ptr<relay::OverlappedIOHandle>>&& ExtraHandles)
{
    RunningWSLAProcess::ProcessResult result;

    relay::MultiHandleWait io;

    // Add a callback on IO for each std handle.
    for (size_t i = 0; i < m_fds.size(); i++)
    {
        if (m_fds[i].Fd == 0 || m_fds[i].Type != WSLAFdTypeDefault)
        {
            continue; // Don't try to read from stdin or non hvsocket fds.
        }

        result.Output.emplace(m_fds[i].Fd, std::string{});

        auto stdHandle = GetStdHandle(m_fds[i].Fd);

        auto ioCallback = [Index = m_fds[i].Fd, &result](const gsl::span<char>& Content) {
            result.Output[Index].insert(result.Output[Index].end(), Content.begin(), Content.end());
        };

        io.AddHandle(std::make_unique<relay::ReadHandle>(std::move(stdHandle), std::move(ioCallback)));
    }

    for (auto& e : ExtraHandles)
    {
        io.AddHandle(std::move(e));
    }

    // Add a callback for when the process exits.
    auto exitCallback = [&]() { std::tie(result.Code, result.Signalled) = GetExitState(); };

    io.AddHandle(std::make_unique<relay::EventHandle>(GetExitEvent(), std::move(exitCallback)));

    io.Run(std::chrono::milliseconds(TimeoutMs));

    return result;
}

ClientRunningWSLAProcess WSLAProcessLauncher::Launch(IWSLASession& Session)
{
    auto [hresult, error, process] = LaunchNoThrow(Session);
    if (FAILED(hresult))
    {
        auto commandLine = wsl::shared::string::Join(m_arguments, ' ');
        THROW_HR_MSG(hresult, "Failed to launch process: %hs (commandline: %hs). Errno = %i", m_executable.c_str(), commandLine.c_str(), error);
    }

    return process.value();
}

std::tuple<HRESULT, int, std::optional<ClientRunningWSLAProcess>> WSLAProcessLauncher::LaunchNoThrow(IWSLASession& Session)
{
    auto [options, commandLine, env] = CreateProcessOptions();

    wil::com_ptr<IWSLAProcess> process;
    int error = -1;
    auto result = Session.CreateRootNamespaceProcess(&options, &process, &error);
    if (FAILED(result))
    {
        return std::make_tuple(result, error, std::optional<ClientRunningWSLAProcess>());
    }

    wsl::windows::common::security::ConfigureForCOMImpersonation(process.get());

    return {S_OK, 0, ClientRunningWSLAProcess{std::move(process), std::move(m_fds)}};
}

IWSLAProcess& ClientRunningWSLAProcess::Get()
{
    return *m_process.get();
}

ClientRunningWSLAProcess::ClientRunningWSLAProcess(wil::com_ptr<IWSLAProcess>&& process, std::vector<WSLA_PROCESS_FD>&& fds) :
    RunningWSLAProcess(std::move(fds)), m_process(std::move(process))
{
}

wil::unique_handle ClientRunningWSLAProcess::GetStdHandle(int Index)
{
    wil::unique_handle handle;
    THROW_IF_FAILED_MSG(m_process->GetStdHandle(Index, reinterpret_cast<ULONG*>(&handle)), "Failed to get handle: %i", Index);

    return handle;
}

wil::unique_event ClientRunningWSLAProcess::GetExitEvent()
{
    wil::unique_event event;
    THROW_IF_FAILED(m_process->GetExitEvent(reinterpret_cast<ULONG*>(&event)));

    return event;
}

void ClientRunningWSLAProcess::GetState(WSLA_PROCESS_STATE* State, int* Code)
{
    THROW_IF_FAILED(m_process->GetState(State, Code));
}
