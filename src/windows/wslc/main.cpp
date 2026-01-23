/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    main.cpp

Abstract:

    Entry point for the wslc CLI.

--*/

#include "precomp.h"
#include "CommandLine.h"
#include "wslutil.h"
#include "wslaservice.h"
#include "WslSecurity.h"
#include "WSLAProcessLauncher.h"
#include "ExecutionContext.h"
#include <thread>
#include <format>

using namespace wsl::shared;
namespace wslutil = wsl::windows::common::wslutil;
using wsl::windows::common::ClientRunningWSLAProcess;
using wsl::windows::common::Context;
using wsl::windows::common::ExecutionContext;
using wsl::windows::common::WSLAProcessLauncher;
using wsl::windows::common::relay::EventHandle;
using wsl::windows::common::relay::MultiHandleWait;
using wsl::windows::common::relay::ReadHandle;
using wsl::windows::common::relay::RelayHandle;
using wsl::windows::common::wslutil::WSLAErrorDetails;

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

// Handler for `wslc shell <SessionName>` command.
static int RunShellCommand(std::wstring_view commandLine)
{
    std::wstring sessionName;
    bool verbose = false;

    ArgumentParser parser(std::wstring{commandLine}, L"wslc", 2, false);
    parser.AddPositionalArgument(sessionName, 0);
    parser.AddArgument(verbose, L"--verbose", L'v');

    parser.Parse();

    if (sessionName.empty())
    {
        THROW_HR_WITH_USER_ERROR(E_INVALIDARG, wsl::shared::Localization::MessageMissingArgument(L"<SessionName>", L"wslc shell"));
    }

    wil::com_ptr<IWSLASessionManager> sessionManager;
    THROW_IF_FAILED(CoCreateInstance(__uuidof(WSLASessionManager), nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&sessionManager)));
    wsl::windows::common::security::ConfigureForCOMImpersonation(sessionManager.get());

    wil::com_ptr<IWSLASession> session;
    HRESULT hr = sessionManager->OpenSessionByName(sessionName.c_str(), &session);
    if (FAILED(hr))
    {
        if (hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND))
        {
            wslutil::PrintMessage(Localization::MessageWslaSessionNotFound(sessionName.c_str()), stderr);
            return 1;
        }

        return ReportError(Localization::MessageWslaOpenSessionFailed(sessionName.c_str()), hr);
    }

    if (verbose)
    {
        wslutil::PrintMessage(std::format(L"[wslc] Session opened: '{}'", sessionName), stdout);
    }

    // Console size for TTY.
    CONSOLE_SCREEN_BUFFER_INFO info{};
    THROW_LAST_ERROR_IF(!GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info));
    const ULONG rows = static_cast<ULONG>(info.srWindow.Bottom - info.srWindow.Top + 1);
    const ULONG cols = static_cast<ULONG>(info.srWindow.Right - info.srWindow.Left + 1);

    const std::string shell = "/bin/sh";

    // Launch with terminal fds (PTY).
    wsl::windows::common::WSLAProcessLauncher launcher{
        shell, {shell, "--login"}, {"TERM=xterm-256color"}, wsl::windows::common::ProcessFlags::None};

    launcher.AddFd(WSLA_PROCESS_FD{.Fd = 0, .Type = WSLAFdTypeTerminalInput, .Path = nullptr});
    launcher.AddFd(WSLA_PROCESS_FD{.Fd = 1, .Type = WSLAFdTypeTerminalOutput, .Path = nullptr});
    launcher.AddFd(WSLA_PROCESS_FD{.Fd = 2, .Type = WSLAFdTypeTerminalControl, .Path = nullptr});

    launcher.SetTtySize(rows, cols);

    auto process = launcher.Launch(*session);

    if (verbose)
    {
        wslutil::PrintMessage(L"[wslc] Shell process launched", stdout);
    }

    auto ttyIn = process.GetStdHandle(0);
    auto ttyOut = process.GetStdHandle(1);

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
                GetStdHandle(STD_INPUT_HANDLE), ttyIn.get(), updateTerminalSize, exitEvent.get());
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
    wsl::windows::common::relay::InterruptableRelay(ttyOut.get(), GetStdHandle(STD_OUTPUT_HANDLE), exitEvent.get());

    process.GetExitEvent().wait();

    auto exitCode = process.GetExitCode();

    std::wstring shellWide(shell.begin(), shell.end());
    wslutil::PrintMessage(wsl::shared::Localization::MessageWslaShellExited(shellWide.c_str(), static_cast<int>(exitCode)), stdout);

    return static_cast<int>(exitCode);
}

// Handler for `wslc list` command.
static int RunListCommand(std::wstring_view commandLine)
{
    bool verbose = false;

    ArgumentParser parser(std::wstring{commandLine}, L"wslc", 2, false);
    parser.AddArgument(verbose, L"--verbose", L'v');

    try
    {
        parser.Parse();
    }
    catch (...)
    {
        THROW_HR_WITH_USER_ERROR(E_INVALIDARG, wsl::shared::Localization::MessageWslcUsage());
    }

    wil::com_ptr<IWSLASessionManager> sessionManager;
    THROW_IF_FAILED(CoCreateInstance(__uuidof(WSLASessionManager), nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&sessionManager)));
    wsl::windows::common::security::ConfigureForCOMImpersonation(sessionManager.get());

    wil::unique_cotaskmem_array_ptr<WSLA_SESSION_INFORMATION> sessions;
    THROW_IF_FAILED(sessionManager->ListSessions(&sessions, sessions.size_address<ULONG>()));

    if (verbose)
    {
        const wchar_t* plural = sessions.size() == 1 ? L"" : L"s";
        wslutil::PrintMessage(std::format(L"[wslc] Found {} session{}", sessions.size(), plural), stdout);
    }

    if (sessions.size() == 0)
    {
        wslutil::PrintMessage(Localization::MessageWslaNoSessionsFound(), stdout);
        return 0;
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

    return 0;
}

DEFINE_ENUM_FLAG_OPERATORS(WSLASessionFlags);

static wil::com_ptr<IWSLASession> OpenCLISession()
{
    wil::com_ptr<IWSLASessionManager> sessionManager;
    THROW_IF_FAILED(CoCreateInstance(__uuidof(WSLASessionManager), nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&sessionManager)));
    wsl::windows::common::security::ConfigureForCOMImpersonation(sessionManager.get());

    auto dataFolder = std::filesystem::path(wsl::windows::common::filesystem::GetLocalAppDataPath(nullptr)) / "wsla";

    // TODO: Have a configuration file for those.
    WSLA_SESSION_SETTINGS settings{};
    settings.DisplayName = L"wsla-cli";
    settings.CpuCount = 4;
    settings.MemoryMb = 2048;
    settings.BootTimeoutMs = 30 * 1000;
    settings.StoragePath = dataFolder.c_str();
    settings.MaximumStorageSizeMb = 10000; // 10GB.
    settings.NetworkingMode = WSLANetworkingModeNAT;

    wil::com_ptr<IWSLASession> session;
    THROW_IF_FAILED(sessionManager->CreateSession(&settings, WSLASessionFlagsPersistent | WSLASessionFlagsOpenExisting, &session));
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
    WSLAErrorDetails error{};
    auto result = session->PullImage(Image.c_str(), nullptr, &callback, &error.Error);
    error.ThrowIfFailed(result);
}

static int Pull(std::wstring_view commandLine)
{
    ArgumentParser parser(std::wstring{commandLine}, L"wslc", 2);

    std::string image;
    parser.AddPositionalArgument(Utf8String{image}, 0);

    parser.Parse();
    THROW_HR_IF(E_INVALIDARG, image.empty());

    PullImpl(*OpenCLISession(), image);

    return 0;
}

static int InteractiveShell(ClientRunningWSLAProcess&& Process, bool Tty)
{
    auto exitEvent = Process.GetExitEvent();

    if (Tty)
    {
        // Configure console for interactive usage.
        wsl::windows::common::ConsoleState console;
        auto processTty = Process.GetStdHandle(WSLAFDTty);

        // Create a thread to relay stdin to the pipe.
        std::thread inputThread([&]() {
            auto updateTerminal = [&console, &Process]() {
                const auto windowSize = console.GetWindowSize();
                LOG_IF_FAILED(Process.Get().ResizeTty(windowSize.Y, windowSize.X));
            };

            wsl::windows::common::relay::StandardInputRelay(
                GetStdHandle(STD_INPUT_HANDLE), processTty.get(), updateTerminal, exitEvent.get());
        });

        auto joinThread = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
            exitEvent.SetEvent();
            inputThread.join();
        });

        // Relay the contents of the pipe to stdout.
        wsl::windows::common::relay::InterruptableRelay(processTty.get(), GetStdHandle(STD_OUTPUT_HANDLE));

        // Wait for the process to exit.
        THROW_LAST_ERROR_IF(WaitForSingleObject(exitEvent.get(), INFINITE) != WAIT_OBJECT_0);
    }
    else
    {
        wsl::windows::common::relay::MultiHandleWait io;

        // Create a thread to relay stdin to the pipe.

        std::thread inputThread;

        auto joinThread = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
            if (inputThread.joinable())
            {
                exitEvent.SetEvent();
                inputThread.join();
            }
        });

        // Required because ReadFile() blocks if stdin is a tty.
        if (wsl::windows::common::wslutil::IsInteractiveConsole())
        {
            // TODO: Will output CR instead of LF's which can confuse the linux app.
            // Consider a custom relay logic to fix this.
            inputThread = std::thread{[&]() {
                wsl::windows::common::relay::InterruptableRelay(
                    GetStdHandle(STD_INPUT_HANDLE), Process.GetStdHandle(0).get(), exitEvent.get());
            }};
        }
        else
        {
            io.AddHandle(std::make_unique<RelayHandle<ReadHandle>>(GetStdHandle(STD_INPUT_HANDLE), Process.GetStdHandle(0)));
        }

        io.AddHandle(std::make_unique<RelayHandle<ReadHandle>>(Process.GetStdHandle(1), GetStdHandle(STD_OUTPUT_HANDLE)));
        io.AddHandle(std::make_unique<RelayHandle<ReadHandle>>(Process.GetStdHandle(2), GetStdHandle(STD_ERROR_HANDLE)));
        io.AddHandle(std::make_unique<EventHandle>(exitEvent.get()));

        io.Run({});
    }

    int exitCode = Process.GetExitCode();

    return exitCode;
}

static int Run(std::wstring_view commandLine)
{
    ArgumentParser parser(std::wstring{commandLine}, L"wslc", 2, true);

    bool interactive{};
    bool tty{};
    std::string image;
    parser.AddPositionalArgument(Utf8String{image}, 0);
    parser.AddArgument(interactive, L"--interactive", 'i');
    parser.AddArgument(tty, L"--tty", 't');

    parser.Parse();
    THROW_HR_IF(E_INVALIDARG, image.empty());

    auto session = OpenCLISession();

    WSLA_CONTAINER_OPTIONS options{};
    options.Image = image.c_str();

    std::vector<WSLA_PROCESS_FD> fds;
    HANDLE Stdout = GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE Stdin = GetStdHandle(STD_INPUT_HANDLE);

    if (tty)
    {
        CONSOLE_SCREEN_BUFFER_INFOEX Info{};
        Info.cbSize = sizeof(Info);
        THROW_IF_WIN32_BOOL_FALSE(::GetConsoleScreenBufferInfoEx(Stdout, &Info));

        fds.emplace_back(WSLA_PROCESS_FD{.Fd = 0, .Type = WSLAFdTypeTerminalInput});
        fds.emplace_back(WSLA_PROCESS_FD{.Fd = 1, .Type = WSLAFdTypeTerminalOutput});
        fds.emplace_back(WSLA_PROCESS_FD{.Fd = 2, .Type = WSLAFdTypeTerminalControl});

        options.InitProcessOptions.TtyColumns = Info.srWindow.Right - Info.srWindow.Left + 1;
        options.InitProcessOptions.TtyRows = Info.srWindow.Bottom - Info.srWindow.Top + 1;
    }
    else
    {
        if (interactive)
        {
            fds.emplace_back(WSLA_PROCESS_FD{.Fd = 0, .Type = WSLAFdTypeDefault});
        }

        fds.emplace_back(WSLA_PROCESS_FD{.Fd = 1, .Type = WSLAFdTypeDefault});
        fds.emplace_back(WSLA_PROCESS_FD{.Fd = 2, .Type = WSLAFdTypeDefault});
    }

    std::vector<std::string> argsStorage;
    std::vector<const char*> args;
    for (size_t i = parser.ParseIndex(); i < parser.Argc(); i++)
    {
        argsStorage.emplace_back(wsl::shared::string::WideToMultiByte(parser.Argv(i)));
    }

    for (const auto& e : argsStorage)
    {
        args.emplace_back(e.c_str());
    }

    options.InitProcessOptions.CommandLine = args.data();
    options.InitProcessOptions.CommandLineCount = static_cast<ULONG>(args.size());
    options.InitProcessOptions.Fds = fds.data();
    options.InitProcessOptions.FdsCount = static_cast<ULONG>(fds.size());

    wil::com_ptr<IWSLAContainer> container;
    WSLAErrorDetails error{};
    auto result = session->CreateContainer(&options, &container, &error.Error);
    if (result == WSLA_E_IMAGE_NOT_FOUND)
    {
        wslutil::PrintMessage(std::format(L"Image '{}' not found, pulling", image), stderr);

        PullImpl(*session.get(), image);

        error.Reset();
        result = session->CreateContainer(&options, &container, &error.Error);
    }

    error.ThrowIfFailed(result);

    THROW_IF_FAILED(container->Start()); // TODO: Error message

    wil::com_ptr<IWSLAProcess> process;
    THROW_IF_FAILED(container->GetInitProcess(&process));

    return InteractiveShell(ClientRunningWSLAProcess(std::move(process), std::move(fds)), tty);
}

static void PrintUsage()
{
    wslutil::PrintMessage(Localization::MessageWslcUsage(), stderr);
}

int wslc_main(std::wstring_view commandLine)
{
    // Initialize runtime and COM.
    wslutil::ConfigureCrt();
    wslutil::InitializeWil();

    WslTraceLoggingInitialize(WslaTelemetryProvider, !wsl::shared::OfficialBuild);
    auto cleanupTelemetry = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, []() { WslTraceLoggingUninitialize(); });

    wslutil::SetCrtEncoding(_O_U8TEXT);

    auto coInit = wil::CoInitializeEx(COINIT_MULTITHREADED);
    wslutil::CoInitializeSecurity();

    WSADATA data{};
    THROW_IF_WIN32_ERROR(WSAStartup(MAKEWORD(2, 2), &data));
    auto wsaCleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, []() { WSACleanup(); });

    // Parse the top-level verb (list, shell, --help).
    ArgumentParser parser(std::wstring{commandLine}, L"wslc", 1, true);

    bool help = false;
    std::wstring verb;

    parser.AddPositionalArgument(verb, 0);
    parser.AddArgument(help, L"--help", L'h');

    parser.Parse();

    if (help || verb.empty())
    {
        PrintUsage();
        return 0;
    }
    else if (verb == L"list")
    {
        return RunListCommand(commandLine);
    }
    else if (verb == L"shell")
    {
        return RunShellCommand(commandLine);
    }
    else if (verb == L"pull")
    {
        return Pull(commandLine);
    }
    else if (verb == L"run")
    {
        return Run(commandLine);
    }
    else
    {
        wslutil::PrintMessage(Localization::MessageWslaUnknownCommand(verb.c_str()), stderr);
        PrintUsage();

        // Unknown verb - show usage and fail.
        return 1;
    }
}

int wmain(int, wchar_t**)
{
    wsl::windows::common::EnableContextualizedErrors(false);

    ExecutionContext context{Context::WslC};
    int exitCode = 1;
    HRESULT result = S_OK;

    try
    {
        exitCode = wslc_main(GetCommandLineW());
    }
    catch (...)
    {
        result = wil::ResultFromCaughtException();
    }

    if (FAILED(result))
    {
        if (const auto& reported = context.ReportedError())
        {
            auto strings = wsl::windows::common::wslutil::ErrorToString(*reported);
            auto errorMessage = strings.Message.empty() ? strings.Code : strings.Message;
            wslutil::PrintMessage(Localization::MessageErrorCode(errorMessage, wslutil::ErrorCodeToString(result)), stderr);
        }
        else
        {
            // Fallback for errors without context
            wslutil::PrintMessage(Localization::MessageErrorCode("", wslutil::ErrorCodeToString(result)), stderr);
        }
    }

    return exitCode;
}