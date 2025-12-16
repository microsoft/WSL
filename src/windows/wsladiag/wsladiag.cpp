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

// Handler for `wsladiag shell <SessionName>` (TTY-backed interactive shell).
static int RunShellCommand(const std::wstring& sessionName, bool verbose)
{
    const auto log = [&](std::wstring_view msg) {
        if (verbose)
        {
            wslutil::PrintMessage(std::wstring(msg), stdout);
        }
    };

    log(std::format(L"[diag] shell='{}'\n", sessionName));

    wil::com_ptr<IWSLAUserSession> userSession;
    THROW_IF_FAILED(CoCreateInstance(__uuidof(WSLAUserSession), nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&userSession)));
    wsl::windows::common::security::ConfigureForCOMImpersonation(userSession.get());

    wil::com_ptr<IWSLASession> session;
    THROW_IF_FAILED(userSession->OpenSessionByName(sessionName.c_str(), &session));
    log(L"[diag] OpenSessionByName succeeded\n");

    // Console size for TTY.
    CONSOLE_SCREEN_BUFFER_INFO info{};
    THROW_LAST_ERROR_IF(!GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info));
    const ULONG rows = static_cast<ULONG>(info.srWindow.Bottom - info.srWindow.Top + 1);
    const ULONG cols = static_cast<ULONG>(info.srWindow.Right - info.srWindow.Left + 1);

    const std::string shell = "/bin/sh";

    // Launch with terminal fds (PTY).
    wsl::windows::common::WSLAProcessLauncher launcher{
        shell, {shell, "--login"}, {"TERM=xterm-256color"}, wsl::windows::common::ProcessFlags::None};

    launcher.AddFd(WSLA_PROCESS_FD{.Fd = 0, .Type = WSLAFdTypeTerminalInput});
    launcher.AddFd(WSLA_PROCESS_FD{.Fd = 1, .Type = WSLAFdTypeTerminalOutput});
    launcher.AddFd(WSLA_PROCESS_FD{.Fd = 2, .Type = WSLAFdTypeTerminalControl});
    launcher.SetTtySize(rows, cols);

    log(L"[diag] launching shell process...\n");
    auto process = launcher.Launch(*session);
    log(L"[diag] shell launched (TTY)\n");

    auto ttyIn = process.GetStdHandle(0);
    auto ttyOut = process.GetStdHandle(1);
    auto ttyControl = process.GetStdHandle(2);

    // Console handles.
    wil::unique_hfile conin{
        CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr)};
    wil::unique_hfile conout{
        CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr)};
    THROW_LAST_ERROR_IF(!conin);
    THROW_LAST_ERROR_IF(!conout);

    const HANDLE consoleIn = conin.get();
    const HANDLE consoleOut = conout.get();

    // Save/restore console state.
    DWORD originalInMode{};
    DWORD originalOutMode{};

    THROW_LAST_ERROR_IF(!GetConsoleMode(consoleIn, &originalInMode));
    THROW_LAST_ERROR_IF(!GetConsoleMode(consoleOut, &originalOutMode));

    const UINT originalOutCP = GetConsoleOutputCP();
    const UINT originalInCP = GetConsoleCP();

    auto restoreConsole = wil::scope_exit([&] {
        SetConsoleMode(consoleIn, originalInMode);
        SetConsoleMode(consoleOut, originalOutMode);
        SetConsoleOutputCP(originalOutCP);
        SetConsoleCP(originalInCP);
    });

    // Console mode for interactive terminal.
    DWORD inMode = originalInMode;
    WI_SetAllFlags(inMode, ENABLE_WINDOW_INPUT | ENABLE_VIRTUAL_TERMINAL_INPUT);
    WI_ClearAllFlags(inMode, ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT | ENABLE_INSERT_MODE);
    WI_SetFlag(inMode, ENABLE_PROCESSED_INPUT);
    THROW_IF_WIN32_BOOL_FALSE(SetConsoleMode(consoleIn, inMode));

    DWORD outMode = originalOutMode;
    WI_SetAllFlags(outMode, ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING | DISABLE_NEWLINE_AUTO_RETURN);
    THROW_IF_WIN32_BOOL_FALSE(SetConsoleMode(consoleOut, outMode));

    THROW_LAST_ERROR_IF(!SetConsoleOutputCP(CP_UTF8));
    THROW_LAST_ERROR_IF(!SetConsoleCP(CP_UTF8));

    // Keep terminal control socket alive.
    auto exitEvent = wil::unique_event(wil::EventOptions::ManualReset);
    wsl::shared::SocketChannel controlChannel{wil::unique_socket{(SOCKET)ttyControl.release()}, "TerminalControl", exitEvent.get()};

    auto updateTerminalSize = [&]() {
        CONSOLE_SCREEN_BUFFER_INFOEX infoEx{};
        infoEx.cbSize = sizeof(infoEx);
        THROW_IF_WIN32_BOOL_FALSE(GetConsoleScreenBufferInfoEx(consoleOut, &infoEx));

        WSLA_TERMINAL_CHANGED message{};
        message.Columns = infoEx.srWindow.Right - infoEx.srWindow.Left + 1;
        message.Rows = infoEx.srWindow.Bottom - infoEx.srWindow.Top + 1;

        controlChannel.SendMessage(message);
    };

    // Relay console -> tty input.
    std::thread inputThread([&] {
        try
        {
            wsl::windows::common::relay::StandardInputRelay(consoleIn, ttyIn.get(), updateTerminalSize, exitEvent.get());
        }
        catch (...)
        {
            exitEvent.SetEvent();
        }
    });

    auto joinInput = wil::scope_exit([&] {
        exitEvent.SetEvent();
        if (inputThread.joinable())
        {
            inputThread.join();
        }
    });

    // Relay tty output -> console (blocks until output ends).
    wsl::windows::common::relay::InterruptableRelay(ttyOut.get(), consoleOut, exitEvent.get());

    process.GetExitEvent().wait();
    auto [code, signalled] = process.GetExitState();

    wslutil::PrintMessage(std::format(L"{} exited with: {}{}\n", shell, code, signalled ? L" (signalled)" : L""), stdout);

    return 0;
}

static int RunListCommand(bool /*verbose*/)
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

        wslutil::PrintMessage(L"\nID\tCreator PID\tDisplay Name\n", stdout);
        wslutil::PrintMessage(L"--\t-----------\t------------\n", stdout);

        for (const auto& s : sessions)
        {
            wslutil::PrintMessage(std::format(L"{}\t{}\t{}\n", s.SessionId, s.CreatorPid, s.DisplayName), stdout);
        }
    }

    return 0;
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

    ArgumentParser parser(std::wstring{commandLine}, L"wsladiag");

    bool help = false;
    bool verbose = false;
    std::wstring verb;
    std::wstring shellSession;

    parser.AddPositionalArgument(verb, 0);         // "list" or "shell"
    parser.AddPositionalArgument(shellSession, 1); // session name for "shell"
    parser.AddArgument(help, L"--help", L'h');
    parser.AddArgument(verbose, L"--verbose", L'v');

    parser.Parse();

    auto printUsage = []() {
        wslutil::PrintMessage(
            L"wsladiag - WSLA diagnostics tool\n"
            L"Usage:\n"
            L"  wsladiag list\n"
            L"  wsladiag shell <SessionName> [--verbose]\n"
            L"  wsladiag --help\n",
            stderr);
    };

    if (help || verb.empty())
    {
        printUsage();
        return 0;
    }

    if (verb == L"list")
    {
        return RunListCommand(verbose);
    }

    if (verb == L"shell")
    {
        if (shellSession.empty())
        {
            printUsage();
            return 1;
        }
        return RunShellCommand(shellSession, verbose);
    }

    printUsage();
    return 1;
}

int wmain(int, wchar_t**)
{
    try
    {
        return wsladiag_main(GetCommandLineW());
    }
    catch (...)
    {
        const auto hr = wil::ResultFromCaughtException();
        return ReportError(L"wsladiag failed", hr);
    }
}
