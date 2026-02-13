/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    DiagTasks.cpp

Abstract:

    Implementation of diag command related execution logic.

--*/
#include "pch.h"
#include "CLIExecutionContext.h"
#include "TaskBase.h"
#include "CommonTasks.h"

using namespace wsl::shared;
using namespace wsl::windows::wslc::execution;
using namespace wsl::windows::common::relay;
namespace wslutil = wsl::windows::common::wslutil;
using wsl::windows::common::ClientRunningWSLAProcess;
using wsl::windows::common::Context;
using wsl::windows::common::ExecutionContext;
using wsl::windows::common::WSLAProcessLauncher;
using wsl::windows::common::relay::EventHandle;
using wsl::windows::common::relay::MultiHandleWait;
using wsl::windows::common::relay::ReadHandle;
using wsl::windows::common::relay::RelayHandle;

namespace wsl::windows::wslc::task {

namespace { // Anonymous namespace for local helper functions.
    class ChangeTerminalMode
    {
    public:
        NON_COPYABLE(ChangeTerminalMode);
        NON_MOVABLE(ChangeTerminalMode);

        ChangeTerminalMode(HANDLE Console, bool CursorVisible) : m_console(Console)
        {
            THROW_IF_WIN32_BOOL_FALSE(GetConsoleCursorInfo(Console, &m_originalCursorInfo));
            CONSOLE_CURSOR_INFO newCursorInfo = m_originalCursorInfo;
            newCursorInfo.bVisible = CursorVisible;

            THROW_IF_WIN32_BOOL_FALSE(SetConsoleCursorInfo(Console, &newCursorInfo));
        }

        ~ChangeTerminalMode()
        {
            LOG_IF_WIN32_BOOL_FALSE(SetConsoleCursorInfo(m_console, &m_originalCursorInfo));
        }

    private:
        HANDLE m_console{};
        CONSOLE_CURSOR_INFO m_originalCursorInfo{};
    };

    static int ReportError(const std::wstring& context, HRESULT hr)
    {
        auto errorString = wsl::windows::common::wslutil::ErrorCodeToString(hr);
        wslutil::PrintMessage(Localization::MessageErrorCode(context, errorString), stderr);
        return 1;
    }

    DEFINE_ENUM_FLAG_OPERATORS(WSLASessionFlags);
    DEFINE_ENUM_FLAG_OPERATORS(WSLALogsFlags);

    static wil::com_ptr<IWSLASession> OpenCLISession()
    {
        wil::com_ptr<IWSLASessionManager> manager;
        THROW_IF_FAILED(CoCreateInstance(__uuidof(WSLASessionManager), nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&manager)));
        wsl::windows::common::security::ConfigureForCOMImpersonation(manager.get());

        auto dataFolder = std::filesystem::path(wsl::windows::common::filesystem::GetLocalAppDataPath(nullptr)) / "wsla";

        // TODO: Have a configuration file for those.
        WSLA_SESSION_SETTINGS settings{};
        settings.DisplayName = L"wsla-cli";
        settings.CpuCount = 4;
        settings.MemoryMb = 2024;
        settings.BootTimeoutMs = 30 * 1000;
        settings.StoragePath = dataFolder.c_str();
        settings.MaximumStorageSizeMb = 10000; // 10GB.
        settings.NetworkingMode = WSLANetworkingModeVirtioProxy;

        wil::com_ptr<IWSLASession> session;
        THROW_IF_FAILED(manager->CreateSession(&settings, WSLASessionFlagsPersistent | WSLASessionFlagsOpenExisting, &session));
        wsl::windows::common::security::ConfigureForCOMImpersonation(session.get());

        return session;
    }

    static void PullImpl(IWSLASession& Session, const std::string& Image)
    {
        // Configure console for interactive usage.
        wsl::windows::common::ConsoleState console;

        // TODO: Handle terminal resizes.
        class DECLSPEC_UUID("7A1D3376-835A-471A-8DC9-23653D9962D0") Callback
            : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IProgressCallback, IFastRundown>
        {
        public:
            auto MoveToLine(SHORT Line, bool Revert = true)
            {
                if (Line > 0)
                {
                    wprintf(L"\033[%iA", Line);
                }

                return wil::scope_exit([Line = Line]() {
                    if (Line > 1)
                    {
                        wprintf(L"\033[%iB", Line - 1);
                    }
                });
            }

            HRESULT OnProgress(LPCSTR Status, LPCSTR Id, ULONGLONG Current, ULONGLONG Total) override
            try
            {
                if (Id == nullptr || *Id == '\0') // Print all 'global' statuses on their own line
                {
                    wprintf(L"%hs\n", Status);
                    m_currentLine++;
                    return S_OK;
                }

                auto info = Info();

                auto it = m_statuses.find(Id);
                if (it == m_statuses.end())
                {
                    // If this is the first time we see this ID, create a new line for it.
                    m_statuses.emplace(Id, m_currentLine);
                    wprintf(L"%ls\n", GenerateStatusLine(Status, Id, Current, Total, info).c_str());
                    m_currentLine++;
                }
                else
                {
                    auto revert = MoveToLine(m_currentLine - it->second);
                    wprintf(L"%ls\n", GenerateStatusLine(Status, Id, Current, Total, info).c_str());
                }

                return S_OK;
            }
            CATCH_RETURN();

        private:
            static CONSOLE_SCREEN_BUFFER_INFO Info()
            {
                CONSOLE_SCREEN_BUFFER_INFO info{};
                THROW_IF_WIN32_BOOL_FALSE(GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info));

                return info;
            }

            std::wstring GenerateStatusLine(LPCSTR Status, LPCSTR Id, ULONGLONG Current, ULONGLONG Total, const CONSOLE_SCREEN_BUFFER_INFO& Info)
            {
                std::wstring line;
                if (Total != 0)
                {
                    line = std::format(L"{} '{}': {}%", Status, Id, Current * 100 / Total);
                }
                else if (Current != 0)
                {
                    line = std::format(L"{} '{}': {}s", Status, Id, Current);
                }
                else
                {
                    line = std::format(L"{} '{}'", Status, Id);
                }

                // Erase any previously written char on that line.
                while (line.size() < Info.dwSize.X)
                {
                    line += L' ';
                }

                return line;
            }

            std::map<std::string, SHORT> m_statuses;
            SHORT m_currentLine = 0;
            ChangeTerminalMode m_terminalMode{GetStdHandle(STD_OUTPUT_HANDLE), false};
        };

        wil::com_ptr<IWSLASession> session = OpenCLISession();

        Callback callback;
        THROW_IF_FAILED(session->PullImage(Image.c_str(), nullptr, &callback));
    }

    static void RelayInteractiveTty(ClientRunningWSLAProcess& Process, HANDLE Tty, bool triggerRefresh = false)
    {
        // Configure console for interactive usage.
        wsl::windows::common::ConsoleState console;

        if (triggerRefresh)
        {
            // In the case of an Attach, force a terminal resize to force the tty to refresh its display.
            // The docker client uses the same trick.

            auto size = console.GetWindowSize();

            LOG_IF_FAILED(Process.Get().ResizeTty(size.Y + 1, size.X + 1));
            LOG_IF_FAILED(Process.Get().ResizeTty(size.Y, size.X));
        }

        wil::unique_event exitEvent(wil::EventOptions::ManualReset);

        // Create a thread to relay stdin to the pipe.
        std::thread inputThread([&]() {
            auto updateTerminal = [&console, &Process]() {
                const auto windowSize = console.GetWindowSize();
                LOG_IF_FAILED(Process.Get().ResizeTty(windowSize.Y, windowSize.X));
            };

            wsl::windows::common::relay::StandardInputRelay(GetStdHandle(STD_INPUT_HANDLE), Tty, updateTerminal, exitEvent.get());
        });

        auto joinThread = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
            exitEvent.SetEvent();
            inputThread.join();
        });

        // Relay the contents of the pipe to stdout.
        wsl::windows::common::relay::InterruptableRelay(Tty, GetStdHandle(STD_OUTPUT_HANDLE), exitEvent.get());
    }

    static void RelayNonTtyProcess(wil::unique_handle&& Stdin, wil::unique_handle&& Stdout, wil::unique_handle&& Stderr)
    {
        wsl::windows::common::relay::MultiHandleWait io;

        // Create a thread to relay stdin to the pipe.
        wil::unique_event exitEvent(wil::EventOptions::ManualReset);

        std::thread inputThread;

        auto joinThread = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
            if (inputThread.joinable())
            {
                exitEvent.SetEvent();
                inputThread.join();
            }
        });

        if (Stdin.is_valid())
        {
            // Required because ReadFile() blocks if stdin is a tty.
            if (wsl::windows::common::wslutil::IsInteractiveConsole())
            {
                // TODO: Will output CR instead of LF's which can confuse the linux app.
                // Consider a custom relay logic to fix this.
                inputThread = std::thread{[&]() {
                    try
                    {
                        wsl::windows::common::relay::InterruptableRelay(GetStdHandle(STD_INPUT_HANDLE), Stdin.get(), exitEvent.get());
                    }
                    CATCH_LOG();

                    Stdin.reset();
                }};
            }
            else
            {
                io.AddHandle(std::make_unique<RelayHandle<ReadHandle>>(GetStdHandle(STD_INPUT_HANDLE), std::move(Stdin)));
            }
        }

        io.AddHandle(std::make_unique<RelayHandle<ReadHandle>>(std::move(Stdout), GetStdHandle(STD_OUTPUT_HANDLE)));
        io.AddHandle(std::make_unique<RelayHandle<ReadHandle>>(std::move(Stderr), GetStdHandle(STD_ERROR_HANDLE)));

        io.Run({});
    }

    static int InteractiveShell(ClientRunningWSLAProcess&& Process)
    {
        if (WI_IsFlagSet(Process.Flags(), WSLAProcessFlagsTty))
        {
            RelayInteractiveTty(Process, Process.GetStdHandle(WSLAFDTty).get());
        }
        else
        {
            wil::unique_handle stdinHandle;
            if (WI_IsFlagSet(Process.Flags(), WSLAProcessFlagsStdin))
            {
                stdinHandle = Process.GetStdHandle(WSLAFDStdin);
            }

            RelayNonTtyProcess(std::move(stdinHandle), Process.GetStdHandle(WSLAFDStdout), Process.GetStdHandle(WSLAFDStderr));
        }

        return Process.Wait();
    }
} // namespace

// Sample execution task using wsladiag's List implementation.
void ListContainers(CLIExecutionContext& context)
{
    // This would probably be in another task or wrapper, as working with sessions is common code, and
    // there is a common --session argument to reuse sessions. But including it here for simplicity of the sample.
    wil::com_ptr<IWSLASessionManager> manager;
    THROW_IF_FAILED(CoCreateInstance(__uuidof(WSLASessionManager), nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&manager)));
    wsl::windows::common::security::ConfigureForCOMImpersonation(manager.get());

    wil::unique_cotaskmem_array_ptr<WSLA_SESSION_INFORMATION> sessions;
    THROW_IF_FAILED(manager->ListSessions(&sessions, sessions.size_address<ULONG>()));

    // For flag args, just its presence is equivalent to testing the value, so simple arg containment check.
    if (context.Args.Contains(ArgType::Verbose))
    {
        const wchar_t* plural = sessions.size() == 1 ? L"" : L"s";
        wslutil::PrintMessage(std::format(L"[diag] Found {} session{}", sessions.size(), plural), stdout);
    }

    if (sessions.size() == 0)
    {
        wslutil::PrintMessage(Localization::MessageWslaNoSessionsFound(), stdout);
        return;
    }

    wslutil::PrintMessage(Localization::MessageWslaSessionsFound(sessions.size(), sessions.size() == 1 ? L"" : L"s"), stdout);

    // Use localized headers
    const auto idHeader = Localization::MessageWslaHeaderId();
    const auto pidHeader = Localization::MessageWslaHeaderCreatorPid();
    const auto nameHeader = Localization::MessageWslaHeaderDisplayName();

    size_t idWidth = idHeader.size();
    size_t pidWidth = pidHeader.size();
    size_t nameWidth = nameHeader.size();

    for (const auto& s : sessions)
    {
        idWidth = std::max(idWidth, std::to_wstring(s.SessionId).size());
        pidWidth = std::max(pidWidth, std::to_wstring(s.CreatorPid).size());
        nameWidth = std::max(nameWidth, static_cast<size_t>(s.DisplayName ? wcslen(s.DisplayName) : 0));
    }

    // Header
    wprintf(
        L"%-*ls  %-*ls  %-*ls\n",
        static_cast<int>(idWidth),
        idHeader.c_str(),
        static_cast<int>(pidWidth),
        pidHeader.c_str(),
        static_cast<int>(nameWidth),
        nameHeader.c_str());

    // Underline
    std::wstring idDash(idWidth, L'-');
    std::wstring pidDash(pidWidth, L'-');
    std::wstring nameDash(nameWidth, L'-');

    wprintf(
        L"%-*ls  %-*ls  %-*ls\n",
        static_cast<int>(idWidth),
        idDash.c_str(),
        static_cast<int>(pidWidth),
        pidDash.c_str(),
        static_cast<int>(nameWidth),
        nameDash.c_str());

    // Rows
    for (const auto& s : sessions)
    {
        const wchar_t* displayName = s.DisplayName ? s.DisplayName : L"";
        wprintf(
            L"%-*lu  %-*lu  %-*ls\n",
            static_cast<int>(idWidth),
            static_cast<unsigned long>(s.SessionId),
            static_cast<int>(pidWidth),
            static_cast<unsigned long>(s.CreatorPid),
            static_cast<int>(nameWidth),
            displayName);
    }
}

void RunShellCommand(CLIExecutionContext& context)
{
    wil::com_ptr<IWSLASessionManager> manager;
    THROW_IF_FAILED(CoCreateInstance(__uuidof(WSLASessionManager), nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&manager)));
    wsl::windows::common::security::ConfigureForCOMImpersonation(manager.get());

    auto sessionName = context.Args.Get<ArgType::SessionId>();
    bool verbose = context.Args.Contains(ArgType::Verbose);

    wil::com_ptr<IWSLASession> session;
    HRESULT hr = manager->OpenSessionByName(sessionName.c_str(), &session);
    if (FAILED(hr))
    {
        if (hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND))
        {
            wslutil::PrintMessage(Localization::MessageWslaSessionNotFound(sessionName.c_str()), stderr);
            WSLC_TERMINATE_CONTEXT(hr);
        }

        wslutil::PrintMessage(Localization::MessageWslaOpenSessionFailed(sessionName.c_str()), stderr);
        WSLC_TERMINATE_CONTEXT(hr);
    }

    wsl::windows::common::security::ConfigureForCOMImpersonation(session.get());

    if (verbose)
    {
        wslutil::PrintMessage(std::format(L"[diag] Session opened: '{}'", sessionName), stdout);
    }

    // Console size for TTY.
    CONSOLE_SCREEN_BUFFER_INFO info{};
    THROW_LAST_ERROR_IF(!GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info));
    const ULONG rows = static_cast<ULONG>(info.srWindow.Bottom - info.srWindow.Top + 1);
    const ULONG cols = static_cast<ULONG>(info.srWindow.Right - info.srWindow.Left + 1);

    const std::string shell = "/bin/sh";

    // Launch with terminal fds (PTY).
    wsl::windows::common::WSLAProcessLauncher launcher{shell, {shell, "--login"}, {"TERM=xterm-256color"}, WSLAProcessFlagsTty | WSLAProcessFlagsStdin};
    launcher.SetTtySize(rows, cols);

    auto process = launcher.Launch(*session);

    if (verbose)
    {
        wslutil::PrintMessage(L"[diag] Shell process launched", stdout);
    }

    auto tty = process.GetStdHandle(WSLAFDTty);

    // Configure console for interactive usage.
    wsl::windows::common::ConsoleState console;
    auto updateTerminalSize = [&]() {
        const auto windowSize = console.GetWindowSize();
        LOG_IF_FAILED(process.Get().ResizeTty(windowSize.Y, windowSize.X));
    };

    // Start input relay thread to forward console input to TTY
    // Runs in parallel with output relay (main thread)
    auto exitEvent = wil::unique_event(wil::EventOptions::ManualReset);
    std::thread inputThread([&] {
        try
        {
            wsl::windows::common::relay::StandardInputRelay(
                GetStdHandle(STD_INPUT_HANDLE), tty.get(), updateTerminalSize, exitEvent.get());
        }
        catch (...)
        {
            exitEvent.SetEvent();
        }
    });

    auto joinInput = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&] {
        exitEvent.SetEvent();
        if (inputThread.joinable())
        {
            inputThread.join();
        }
    });

    // Relay tty output -> console (blocks until output ends).
    wsl::windows::common::relay::InterruptableRelay(tty.get(), GetStdHandle(STD_OUTPUT_HANDLE), exitEvent.get());

    process.GetExitEvent().wait();

    auto exitCode = process.GetExitCode();

    std::wstring shellWide(shell.begin(), shell.end());
    wslutil::PrintMessage(wsl::shared::Localization::MessageWslaShellExited(shellWide.c_str(), static_cast<int>(exitCode)), stdout);

    WSLC_TERMINATE_CONTEXT(HRESULT_FROM_WIN32(exitCode));
}

void LogsCommand(CLIExecutionContext& context)
{
    WSLALogsFlags flags = WSLALogsFlagsNone;
    if (context.Args.Contains(ArgType::Follow))
    {
        flags |= WSLALogsFlagsFollow;
    }

    auto id = context.Args.Get<ArgType::ContainerId>();
    auto utf8id = wsl::windows::common::string::WideToMultiByte(id);

    auto session = OpenCLISession();

    wil::com_ptr<IWSLAContainer> container;
    THROW_IF_FAILED(session->OpenContainer(utf8id.c_str(), &container)); // TODO: nicer user error if not found.

    wil::unique_handle stdoutLogs;
    wil::unique_handle stderrLogs;

    THROW_IF_FAILED(container->Logs(flags, reinterpret_cast<ULONG*>(&stdoutLogs), reinterpret_cast<ULONG*>(&stderrLogs), 0, 0, 0));

    wsl::windows::common::relay::MultiHandleWait io;

    io.AddHandle(std::make_unique<RelayHandle<ReadHandle>>(std::move(stdoutLogs), GetStdHandle(STD_OUTPUT_HANDLE)));
    if (stderrLogs) // This handle is only used for non-tty processes.
    {
        io.AddHandle(std::make_unique<RelayHandle<ReadHandle>>(std::move(stderrLogs), GetStdHandle(STD_ERROR_HANDLE)));
    }

    // TODO: Handle ctrl-c.
    io.Run({});
}
} // namespace wsl::windows::wslc::task