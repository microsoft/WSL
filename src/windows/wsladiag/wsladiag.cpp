/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    wsladiag.cpp

Abstract:

    Entry point for the wsladiag tool, performs WSL runtime initialization and parses --list/--help.


--*/

#include "precomp.h"
#include "CommandLine.h"
#include "wslutil.h"
#include "wslaservice.h"
#include "WslSecurity.h"
#include "WSLAProcessLauncher.h"
#include <thread>
#include <format>

using namespace wsl::shared;
namespace wslutil = wsl::windows::common::wslutil;
using wsl::windows::common::WSLAProcessLauncher;

// Adding a helper to factor error handling between all the arguments.
static int ReportError(const std::wstring& context, HRESULT hr)
{
    const std::wstring hrMessage = wslutil::ErrorCodeToString(hr);

    if (!hrMessage.empty())
    {
        wslutil::PrintMessage(std::format(L"{}: 0x{:08x} - {}\n", context, static_cast<unsigned int>(hr), hrMessage), stderr);
    }
    else
    {
        wslutil::PrintMessage(std::format(L"{}: 0x{:08x}\n", context, static_cast<unsigned int>(hr)), stderr);
    }

    return 1;
}

// Function to handle the --shell argument (so it can grow later).
static int RunShellCommand(const std::wstring& sessionName)
{
    wslutil::PrintMessage(std::format(L"[diag] shell='{}'\n", sessionName), stdout);

    try
    {
        wil::com_ptr<IWSLAUserSession> userSession;
        THROW_IF_FAILED(CoCreateInstance(__uuidof(WSLAUserSession), nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&userSession)));

        wsl::windows::common::security::ConfigureForCOMImpersonation(userSession.get());

        wil::com_ptr<IWSLASession> session;

        // Open the existing session by name/ID from the --shell argument.
        THROW_IF_FAILED(userSession->OpenSessionByName(sessionName.c_str(), &session));

        wslutil::PrintMessage(L"[diag] OpenSessionByName succeeded\n", stdout);

        std::string shell = "/bin/sh";

        WSLAProcessLauncher launcher{shell, {shell}, {"TERM=xterm-256color"}};
        launcher.AddFd(WSLA_PROCESS_FD{.Fd = 0, .Type = WSLAFdTypeTerminalInput});
        launcher.AddFd(WSLA_PROCESS_FD{.Fd = 1, .Type = WSLAFdTypeTerminalOutput});
        launcher.AddFd(WSLA_PROCESS_FD{.Fd = 2, .Type = WSLAFdTypeTerminalControl});

        wslutil::PrintMessage(L"[diag] launching shell process...\n", stdout);

        auto process = launcher.Launch(*session);

        // Interactive console wiring.

        const HANDLE Stdin = GetStdHandle(STD_INPUT_HANDLE);
        const HANDLE Stdout = GetStdHandle(STD_OUTPUT_HANDLE);
        THROW_LAST_ERROR_IF(!Stdin || Stdin == INVALID_HANDLE_VALUE);
        THROW_LAST_ERROR_IF(!Stdout || Stdout == INVALID_HANDLE_VALUE);

        // Save original console modes so they can be restored on exit.
        DWORD OriginalInputMode{};
        DWORD OriginalOutputMode{};
        UINT OriginalOutputCP = GetConsoleOutputCP();
        THROW_LAST_ERROR_IF(!::GetConsoleMode(Stdin, &OriginalInputMode));
        THROW_LAST_ERROR_IF(!::GetConsoleMode(Stdout, &OriginalOutputMode));

        auto restoreConsoleMode = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&] {
            SetConsoleMode(Stdin, OriginalInputMode);
            SetConsoleMode(Stdout, OriginalOutputMode);
            SetConsoleOutputCP(OriginalOutputCP);
        });

        // Configure console for interactive usage.
        DWORD InputMode = OriginalInputMode;
        WI_SetAllFlags(InputMode, (ENABLE_WINDOW_INPUT | ENABLE_VIRTUAL_TERMINAL_INPUT));
        WI_ClearAllFlags(InputMode, (ENABLE_ECHO_INPUT | ENABLE_INSERT_MODE | ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT));
        THROW_IF_WIN32_BOOL_FALSE(::SetConsoleMode(Stdin, InputMode));

        DWORD OutputMode = OriginalOutputMode;
        WI_SetAllFlags(OutputMode, ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING | DISABLE_NEWLINE_AUTO_RETURN);
        THROW_IF_WIN32_BOOL_FALSE(::SetConsoleMode(Stdout, OutputMode));

        THROW_LAST_ERROR_IF(!::SetConsoleOutputCP(CP_UTF8));

        // Create a thread to relay stdin to the process.
        auto exitEvent = wil::unique_event(wil::EventOptions::ManualReset);

        wsl::shared::SocketChannel controlChannel{
            wil::unique_socket{(SOCKET)process.GetStdHandle(2).release()}, "TerminalControl", exitEvent.get()};

        std::thread inputThread([&]() {
            auto updateTerminal = [&controlChannel, &Stdout]() {
                CONSOLE_SCREEN_BUFFER_INFOEX info{};
                info.cbSize = sizeof(info);

                THROW_IF_WIN32_BOOL_FALSE(GetConsoleScreenBufferInfoEx(Stdout, &info));

                WSLA_TERMINAL_CHANGED message{};
                message.Columns = info.srWindow.Right - info.srWindow.Left + 1;
                message.Rows = info.srWindow.Bottom - info.srWindow.Top + 1;

                controlChannel.SendMessage(message);
            };

            wsl::windows::common::relay::StandardInputRelay(Stdin, process.GetStdHandle(0).get(), updateTerminal, exitEvent.get());
        });

        auto joinThread = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
            exitEvent.SetEvent();
            inputThread.join();
        });

        // Relay the process stdout to the console.
        wsl::windows::common::relay::InterruptableRelay(process.GetStdHandle(1).get(), Stdout);

        process.GetExitEvent().wait();

        auto [code, signalled] = process.GetExitState();

        const std::wstring shellW{shell.begin(), shell.end()};
        wslutil::PrintMessage(std::format(L"{} exited with: {}{}\n", shellW, code, signalled ? L" (signalled)" : L""), stdout);

        return 0;
    }
    catch (...)
    {
        const auto hr = wil::ResultFromCaughtException();
        return ReportError(std::format(L"Error opening shell for '{}'", sessionName), hr);
    }
}

int wsladiag_main(std::wstring_view commandLine)
{
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

    // Command-line parsing using ArgumentParser.
    ArgumentParser parser(std::wstring{commandLine}, L"wsladiag");

    bool help = false;
    bool list = false;

    std::wstring shellSession;

    parser.AddArgument(list, L"--list");
    parser.AddArgument(help, L"--help", L'h'); // short option is a single wide char
    parser.AddArgument(shellSession, L"--shell");

    parser.Parse();

    auto printUsage = []() {
        wslutil::PrintMessage(
            L"wsladiag - WSLA diagnostics tool\n"
            L"Usage:\n"
            L"  wsladiag --list                  List WSLA sessions\n"
            L"  wsladiag --shell <SessionName>   Open a shell in an existing WSLA session\n"
            L"  wsladiag --help                  Show this help\n",
            stderr);
    };

    // If '--help' was requested, print usage and exit.
    if (help)
    {
        printUsage();
        return 0;
    }

    if (!shellSession.empty())
    {
        return RunShellCommand(shellSession);
    }

    if (!list)
    {
        // No recognized command → show usage
        printUsage();
        return 0;
    }

    // --list: Call WSLA service COM interface to retrieve and display sessions.
    try
    {
        wil::com_ptr<IWSLAUserSession> userSession;
        THROW_IF_FAILED(CoCreateInstance(__uuidof(WSLAUserSession), nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&userSession)));

        wsl::windows::common::security::ConfigureForCOMImpersonation(userSession.get());

        wil::unique_cotaskmem_array_ptr<WSLA_SESSION_INFORMATION> sessions;

        THROW_IF_FAILED(userSession->ListSessions(&sessions, sessions.size_address<ULONG>()));

        if (sessions.size() == 0)
        {
            wslutil::PrintMessage(L"No WSLA sessions found.\n", stdout);
        }
        else
        {
            wslutil::PrintMessage(std::format(L"Found {} WSLA session{}:\n", sessions.size(), sessions.size() > 1 ? L"s" : L""), stdout);

            wslutil::PrintMessage(L"ID\tCreator PID\tDisplay Name\n", stdout);
            wslutil::PrintMessage(L"--\t-----------\t------------\n", stdout);

            for (const auto& session : sessions)
            {
                const auto* displayName = session.DisplayName;

                wslutil::PrintMessage(std::format(L"{}\t{}\t{}\n", session.SessionId, session.CreatorPid, displayName), stdout);
            }
        }

        return 0;
    }
    catch (...)
    {
        const auto hr = wil::ResultFromCaughtException();
        return ReportError(L"Error listing WSLA sessions", hr);
    }
}

int wmain(int /*argc*/, wchar_t** /*argv*/)
{
    try
    {
        // Use raw Unicode command line so ArgumentParser gets original input.
        return wsladiag_main(GetCommandLineW());
    }
    CATCH_RETURN();
}