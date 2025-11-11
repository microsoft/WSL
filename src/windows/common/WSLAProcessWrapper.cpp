#include <precomp.h>
#include "WSLAProcessWrapper.h"
#include "WSLAApi.h"

using wsl::windows::common::WSLAProcessWrapper;

WSLAProcessWrapper::WSLAProcessWrapper(IWSLASession* Session, std::string&& Executable, std::vector<std::string>&& Arguments, FDFlags Flags) :
    m_executable(std::move(Executable)), m_arguments(std::move(Arguments))
{
    m_launch = [Session](const WSLA_PROCESS_OPTIONS* Options) {
        Microsoft::WRL::ComPtr<IWSLAProcess> process;
        THROW_IF_FAILED(Session->CreateRootNamespaceProcess(Options, &process));

        return process;
    };

    // Add standard Fds.
    if (WI_IsFlagSet(Flags, FDFlags::Stdin))
    {
        m_fds.emplace_back(WSLA_PROCESS_FD{.Fd = 0, .Type = WslFdTypeDefault, .Path = nullptr});
    }

    if (WI_IsFlagSet(Flags, FDFlags::Stdout))
    {
        m_fds.emplace_back(WSLA_PROCESS_FD{.Fd = 1, .Type = WslFdTypeDefault, .Path = nullptr});
    }

    if (WI_IsFlagSet(Flags, FDFlags::Stdout))
    {
        m_fds.emplace_back(WSLA_PROCESS_FD{.Fd = 2, .Type = WslFdTypeDefault, .Path = nullptr});
    }
}

IWSLAProcess& WSLAProcessWrapper::Launch()
{
    std::vector<const char*> commandLine;
    std::ranges::transform(m_arguments, std::back_inserter(commandLine), [](const std::string& e) { return e.c_str(); });

    WSLA_PROCESS_OPTIONS options{};
    options.Executable = m_executable.c_str();
    options.CommandLine = commandLine.data();
    options.CommandLineCount = static_cast<DWORD>(commandLine.size());
    options.Fds = m_fds.data();
    options.FdsCount = static_cast<DWORD>(m_fds.size());

    // TODO: Environment support

    m_process = m_launch(&options);
    wsl::windows::common::security::ConfigureForCOMImpersonation(m_process.Get());

    return *m_process.Get();
}

WSLAProcessWrapper::ProcessResult WSLAProcessWrapper::WaitAndCaptureOutput(DWORD TimeoutMs, std::vector<std::unique_ptr<relay::IOHandle>>&& ExtraHandles)
{
    THROW_HR_IF(E_UNEXPECTED, !m_process);

    WSLAProcessWrapper::ProcessResult result;

    relay::MultiHandleWait io;

    // Add a callback on IO for each std handle.
    for (size_t i = 0; i < m_fds.size(); i++)
    {
        if (m_fds[i].Fd == 0)
        {
            continue; // Don't try to read from stdin
        }

        result.Output.emplace_back();

        wil::unique_handle stdHandle;
        THROW_IF_FAILED(m_process->GetStdHandle(m_fds[i].Fd, reinterpret_cast<ULONG*>(&stdHandle)));

        auto ioCallback = [Index = result.Output.size() - 1, &result](const gsl::span<char>& Content) {
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

WSLAProcessWrapper::ProcessResult WSLAProcessWrapper::LaunchAndCaptureOutput(DWORD TimeoutMs)
{
    Launch();
    return WaitAndCaptureOutput(TimeoutMs);
}