/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLAProcessLauncher.cpp

Abstract:

    WSLAProcessLauncher implementation.

--*/

#include <precomp.h>
#include "WSLAProcessLauncher.h"

using wsl::windows::common::ClientRunningWSLAProcess;
using wsl::windows::common::RunningWSLAProcess;
using wsl::windows::common::WSLAProcessLauncher;

WSLAProcessLauncher::WSLAProcessLauncher(
    const std::string& Executable, const std::vector<std::string>& Arguments, const std::vector<std::string>& Environment, WSLAProcessFlags Flags) :
    m_executable(Executable), m_arguments(Arguments), m_environment(Environment), m_flags(Flags)
{
}

void WSLAProcessLauncher::SetTtySize(ULONG Rows, ULONG Columns)
{
    m_rows = Rows;
    m_columns = Columns;
}

void WSLAProcessLauncher::SetWorkingDirectory(std::string&& WorkingDirectory)
{
    m_workingDirectory = std::move(WorkingDirectory);
}

void WSLAProcessLauncher::SetUser(std::string&& User)
{
    m_user = std::move(User);
}

std::tuple<WSLA_PROCESS_OPTIONS, std::vector<const char*>, std::vector<const char*>> WSLAProcessLauncher::CreateProcessOptions()
{
    std::vector<const char*> commandLine;
    std::ranges::transform(m_arguments, std::back_inserter(commandLine), [](const std::string& e) { return e.c_str(); });

    std::vector<const char*> environment;
    std::ranges::transform(m_environment, std::back_inserter(environment), [](const std::string& e) { return e.c_str(); });

    WSLA_PROCESS_OPTIONS options{};
    options.CommandLine = {.Values = commandLine.data(), .Count = static_cast<DWORD>(commandLine.size())};
    options.Environment = {.Values = environment.data(), .Count = static_cast<DWORD>(environment.size())};
    options.TtyColumns = m_columns;
    options.TtyRows = m_rows;
    options.Flags = m_flags;

    if (!m_workingDirectory.empty())
    {
        options.CurrentDirectory = m_workingDirectory.c_str();
    }

    if (!m_user.empty())
    {
        options.User = m_user.c_str();
    }

    return std::make_tuple(options, std::move(commandLine), std::move(environment));
}

RunningWSLAProcess::RunningWSLAProcess(WSLAProcessFlags Flags) : m_flags(Flags)
{
}

WSLAProcessFlags RunningWSLAProcess::Flags() const
{
    return m_flags;
}

int RunningWSLAProcess::GetExitCode()
{
    WSLA_PROCESS_STATE state{};
    int code{};
    GetState(&state, &code);

    THROW_HR_IF_MSG(
        HRESULT_FROM_WIN32(ERROR_INVALID_STATE),
        state != WslaProcessStateSignalled && state != WslaProcessStateExited,
        "Process is not exited. State: %i",
        state);

    return code;
}

WSLA_PROCESS_STATE RunningWSLAProcess::State()
{
    WSLA_PROCESS_STATE state{};
    int code{};
    GetState(&state, &code);

    return state;
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

std::string WSLAProcessLauncher::FormatResult(const int code)
{
    return std::format("{} [{}] exited with: {}.", m_executable, wsl::shared::string::Join(m_arguments, ','), code);
}

int RunningWSLAProcess::Wait(DWORD TimeoutMs)
{
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_TIMEOUT), !GetExitEvent().wait(TimeoutMs));
    return GetExitCode();
}

RunningWSLAProcess::ProcessResult RunningWSLAProcess::WaitAndCaptureOutput(DWORD TimeoutMs, std::vector<std::unique_ptr<relay::OverlappedIOHandle>>&& ExtraHandles)
{
    RunningWSLAProcess::ProcessResult result;

    relay::MultiHandleWait io;

    // Add a callback on IO for each std handle.

    auto addHandle = [&](int fd) {
        result.Output.emplace(fd, std::string{});

        auto stdHandle = GetStdHandle(fd);
        auto ioCallback = [Index = fd, &result](const gsl::span<char>& Content) {
            result.Output[Index].insert(result.Output[Index].end(), Content.begin(), Content.end());
        };

        io.AddHandle(std::make_unique<relay::ReadHandle>(std::move(stdHandle), std::move(ioCallback)));
    };

    if (WI_IsFlagSet(m_flags, WSLAProcessFlagsTty))
    {
        addHandle(WSLAFDTty);
    }
    else
    {
        addHandle(WSLAFDStdout);
        addHandle(WSLAFDStderr);
    }

    for (auto& e : ExtraHandles)
    {
        io.AddHandle(std::move(e));
    }

    // Add a callback for when the process exits.
    auto exitCallback = [&]() { result.Code = GetExitCode(); };

    io.AddHandle(std::make_unique<relay::EventHandle>(GetExitEvent(), std::move(exitCallback)));

    io.Run(std::chrono::milliseconds(TimeoutMs));

    return result;
}

std::tuple<HRESULT, std::optional<ClientRunningWSLAProcess>, int> WSLAProcessLauncher::LaunchNoThrow(IWSLASession& Session)
{
    auto [options, commandLine, env] = CreateProcessOptions();

    wil::com_ptr<IWSLAProcess> process;
    int error = -1;
    auto result = Session.CreateRootNamespaceProcess(m_executable.c_str(), &options, &process, &error);
    if (FAILED(result))
    {
        return std::make_tuple(result, std::optional<ClientRunningWSLAProcess>(), error);
    }

    wsl::windows::common::security::ConfigureForCOMImpersonation(process.get());

    return {S_OK, ClientRunningWSLAProcess{std::move(process), m_flags}, 0};
}

std::tuple<HRESULT, std::optional<ClientRunningWSLAProcess>> WSLAProcessLauncher::LaunchNoThrow(IWSLAContainer& Container)
{
    auto [options, commandLine, env] = CreateProcessOptions();

    wil::com_ptr<IWSLAProcess> process;
    auto result = Container.Exec(&options, &process);
    if (FAILED(result))
    {
        return std::make_pair(result, std::optional<ClientRunningWSLAProcess>());
    }

    wsl::windows::common::security::ConfigureForCOMImpersonation(process.get());

    return {S_OK, ClientRunningWSLAProcess{std::move(process), m_flags}};
}

IWSLAProcess& ClientRunningWSLAProcess::Get()
{
    return *m_process.get();
}

ClientRunningWSLAProcess::ClientRunningWSLAProcess(wil::com_ptr<IWSLAProcess>&& process, WSLAProcessFlags Flags) :
    RunningWSLAProcess(Flags), m_process(std::move(process))
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
