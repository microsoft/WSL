/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCProcessLauncher.cpp

Abstract:

    WSLCProcessLauncher implementation.

--*/

#include <precomp.h>
#include "WSLCProcessLauncher.h"

using wsl::windows::common::ClientRunningWSLCProcess;
using wsl::windows::common::RunningWSLCProcess;
using wsl::windows::common::WSLCProcessLauncher;

WSLCProcessLauncher::WSLCProcessLauncher(
    const std::string& Executable, const std::vector<std::string>& Arguments, const std::vector<std::string>& Environment, WSLCProcessFlags Flags) :
    m_executable(Executable), m_arguments(Arguments), m_environment(Environment), m_flags(Flags)
{
}

void WSLCProcessLauncher::SetTtySize(ULONG Rows, ULONG Columns)
{
    m_rows = Rows;
    m_columns = Columns;
}

void WSLCProcessLauncher::SetWorkingDirectory(std::string&& WorkingDirectory)
{
    m_workingDirectory = std::move(WorkingDirectory);
}

void WSLCProcessLauncher::SetDetachKeys(std::string&& DetachKeys)
{
    m_detachKeys = std::move(DetachKeys);
}

void WSLCProcessLauncher::SetUser(std::string&& User)
{
    m_user = std::move(User);
}

std::tuple<WSLCProcessOptions, std::vector<const char*>, std::vector<const char*>> WSLCProcessLauncher::CreateProcessOptions()
{
    std::vector<const char*> commandLine;
    std::ranges::transform(m_arguments, std::back_inserter(commandLine), [](const std::string& e) { return e.c_str(); });

    std::vector<const char*> environment;
    std::ranges::transform(m_environment, std::back_inserter(environment), [](const std::string& e) { return e.c_str(); });

    WSLCProcessOptions options{};
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

RunningWSLCProcess::RunningWSLCProcess(WSLCProcessFlags Flags) : m_flags(Flags)
{
}

WSLCProcessFlags RunningWSLCProcess::Flags() const
{
    return m_flags;
}

int RunningWSLCProcess::GetExitCode()
{
    WSLCProcessState state{};
    int code{};
    GetState(&state, &code);

    THROW_HR_IF_MSG(
        HRESULT_FROM_WIN32(ERROR_INVALID_STATE),
        state != WslcProcessStateSignalled && state != WslcProcessStateExited,
        "Process is not exited. State: %i",
        state);

    return code;
}

WSLCProcessState RunningWSLCProcess::State()
{
    WSLCProcessState state{};
    int code{};
    GetState(&state, &code);

    return state;
}

std::string WSLCProcessLauncher::FormatResult(const RunningWSLCProcess::ProcessResult& result)
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

std::string WSLCProcessLauncher::FormatResult(const int code)
{
    return std::format("{} [{}] exited with: {}.", m_executable, wsl::shared::string::Join(m_arguments, ','), code);
}

int RunningWSLCProcess::Wait(DWORD TimeoutMs)
{
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_TIMEOUT), !GetExitEvent().wait(TimeoutMs));
    return GetExitCode();
}

RunningWSLCProcess::ProcessResult RunningWSLCProcess::WaitAndCaptureOutput(DWORD TimeoutMs, std::vector<std::unique_ptr<relay::OverlappedIOHandle>>&& ExtraHandles)
{
    RunningWSLCProcess::ProcessResult result;

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

    if (WI_IsFlagSet(m_flags, WSLCProcessFlagsTty))
    {
        addHandle(WSLCFDTty);
    }
    else
    {
        addHandle(WSLCFDStdout);
        addHandle(WSLCFDStderr);
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

std::tuple<HRESULT, std::optional<ClientRunningWSLCProcess>, int> WSLCProcessLauncher::LaunchNoThrow(IWSLCSession& Session)
{
    auto [options, commandLine, env] = CreateProcessOptions();

    wil::com_ptr<IWSLCProcess> process;
    int error = -1;
    auto result = Session.CreateRootNamespaceProcess(m_executable.c_str(), &options, &process, &error);
    if (FAILED(result))
    {
        return std::make_tuple(result, std::optional<ClientRunningWSLCProcess>(), error);
    }

    wsl::windows::common::security::ConfigureForCOMImpersonation(process.get());

    return {S_OK, ClientRunningWSLCProcess{std::move(process), m_flags}, 0};
}

std::tuple<HRESULT, std::optional<ClientRunningWSLCProcess>> WSLCProcessLauncher::LaunchNoThrow(IWSLCContainer& Container)
{
    auto [options, commandLine, env] = CreateProcessOptions();

    wil::com_ptr<IWSLCProcess> process;
    auto result = Container.Exec(&options, m_detachKeys.has_value() ? m_detachKeys->c_str() : nullptr, &process);
    if (FAILED(result))
    {
        return std::make_pair(result, std::optional<ClientRunningWSLCProcess>());
    }

    wsl::windows::common::security::ConfigureForCOMImpersonation(process.get());

    return {S_OK, ClientRunningWSLCProcess{std::move(process), m_flags}};
}

IWSLCProcess& ClientRunningWSLCProcess::Get()
{
    return *m_process.get();
}

ClientRunningWSLCProcess::ClientRunningWSLCProcess(wil::com_ptr<IWSLCProcess>&& process, WSLCProcessFlags Flags) :
    RunningWSLCProcess(Flags), m_process(std::move(process))
{
}

wil::unique_handle ClientRunningWSLCProcess::GetStdHandle(int Index)
{
    wslutil::COMOutputHandle handle;
    THROW_IF_FAILED_MSG(m_process->GetStdHandle(static_cast<WSLCFD>(Index), &handle), "Failed to get handle: %i", Index);

    return handle.Release();
}

wil::unique_event ClientRunningWSLCProcess::GetExitEvent()
{
    wil::unique_event event{};
    THROW_IF_FAILED(m_process->GetExitEvent(&event));

    return event;
}

void ClientRunningWSLCProcess::GetState(WSLCProcessState* State, int* Code)
{
    THROW_IF_FAILED(m_process->GetState(State, Code));
}
