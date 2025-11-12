#include <precomp.h>
#include "WSLAProcessWrapper.h"
#include "WSLAApi.h"

using wsl::windows::common::RunningWSLAProcess;
using wsl::windows::common::WSLAProcessLauncher;

WSLAProcessLauncher::WSLAProcessLauncher(
    const std::string& Executable, const std::vector<std::string>& Arguments, const std::vector<std::string>& Environment, ProcessFlags Flags) :
    m_executable(Executable), m_arguments(Arguments), m_environment(Environment)
{
    // Add standard Fds.
    if (WI_IsFlagSet(Flags, ProcessFlags::Stdin))
    {
        m_fds.emplace_back(WSLA_PROCESS_FD{.Fd = 0, .Type = WslFdTypeDefault, .Path = nullptr});
    }

    if (WI_IsFlagSet(Flags, ProcessFlags::Stdout))
    {
        m_fds.emplace_back(WSLA_PROCESS_FD{.Fd = 1, .Type = WslFdTypeDefault, .Path = nullptr});
    }

    if (WI_IsFlagSet(Flags, ProcessFlags::Stdout))
    {
        m_fds.emplace_back(WSLA_PROCESS_FD{.Fd = 2, .Type = WslFdTypeDefault, .Path = nullptr});
    }
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

RunningWSLAProcess WSLAProcessLauncher::Launch(IWSLASession& Session)
{
    auto [hresult, error, process] = LaunchNoThrow(Session);
    if (FAILED(hresult))
    {
        auto commandLine = wsl::shared::string::Join(m_arguments, ' ');
        THROW_HR_MSG(hresult, "Failed to launch process: %hs (commandline: %hs). Errno = %i", m_executable.c_str(), commandLine.c_str(), error);
    }

    return process.value();
}

std::tuple<HRESULT, int, std::optional<RunningWSLAProcess>> WSLAProcessLauncher::LaunchNoThrow(IWSLASession& Session)
{
    auto [options, commandLine, env] = CreateProcessOptions();
    // TODO: Environment support

    wil::com_ptr<IWSLAProcess> process;
    int error = -1;
    auto result = Session.CreateRootNamespaceProcess(&options, &process, &error);
    if (FAILED(result))
    {
        return std::make_tuple(result, error, std::optional<RunningWSLAProcess>());
    }

    wsl::windows::common::security::ConfigureForCOMImpersonation(process.get());

    return {S_OK, 0, RunningWSLAProcess{std::move(process), std::move(m_fds)}};
}

IWSLAProcess& RunningWSLAProcess::Get()
{
    return *m_process.get();
}

RunningWSLAProcess::RunningWSLAProcess(wil::com_ptr<IWSLAProcess>&& process, std::vector<WSLA_PROCESS_FD>&& fds) :
    m_process(std::move(process)), m_fds(std::move(fds))
{
}

RunningWSLAProcess::ProcessResult RunningWSLAProcess::WaitAndCaptureOutput(DWORD TimeoutMs, std::vector<std::unique_ptr<relay::OverlappedIOHandle>>&& ExtraHandles)
{
    RunningWSLAProcess::ProcessResult result;

    relay::MultiHandleWait io;

    // Add a callback on IO for each std handle.
    for (size_t i = 0; i < m_fds.size(); i++)
    {
        if (m_fds[i].Fd == 0)
        {
            continue; // Don't try to read from stdin
        }

        result.Output.emplace(m_fds[i].Fd, std::string{});

        wil::unique_handle stdHandle;
        THROW_IF_FAILED(m_process->GetStdHandle(m_fds[i].Fd, reinterpret_cast<ULONG*>(&stdHandle)));

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
    wil::unique_handle exitEvent;
    THROW_IF_FAILED(m_process->GetExitEvent(reinterpret_cast<ULONG*>(&exitEvent)));

    auto exitCallback = [&]() {
        WSLA_PROCESS_STATE state{};
        THROW_IF_FAILED(m_process->GetState(&state, &result.Code));

        if (state == WslaProcessStateExited)
        {
            result.Signalled = false;
        }
        else if (state == WslaProcessStateSignalled)
        {
            result.Signalled = true;
        }
        else
        {
            THROW_HR_MSG(E_UNEXPECTED, "Unexpected process state: %i", state);
        }
    };

    io.AddHandle(std::make_unique<relay::EventHandle>(std::move(exitEvent), std::move(exitCallback)));

    io.Run(std::chrono::milliseconds(TimeoutMs));

    return result;
}