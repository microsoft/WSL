/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    wsladiag.cpp

Abstract:

    Entry point for the wsladiag tool, performs WSL runtime initialization and parses list/shell/help.

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
using wsl::windows::common::WSLAProcessLauncher;

// Adding a helper to factor error handling between all the arguments.
static int ReportError(const std::wstring& context, HRESULT hr)
{
    const std::wstring hrMessage = wslutil::GetErrorString(hr);

    if (!hrMessage.empty())
    {
        wslutil::PrintMessage(std::format(L"{}: 0x{:08x} - {}", context, static_cast<uint32_t>(hr), hrMessage), stderr);
    }
    else
    {
        wslutil::PrintMessage(std::format(L"{}: 0x{:08x}", context, static_cast<uint32_t>(hr)), stderr);
    }

    return 1;
}

// Handler for `wsladiag shell <SessionName> [--verbose]` command - launches TTY-backed interactive shell.
static int RunShellCommand(std::wstring_view commandLine)
{
    std::wstring sessionName;
    bool verbose = false;

    ArgumentParser parser(std::wstring{commandLine}, L"wsladiag", 2); // Skip "wsladiag.exe shell" to parse shell-specific args
    parser.AddPositionalArgument(sessionName, 0);
    parser.AddArgument(verbose, L"--verbose", L'v');

    parser.Parse();

    if (sessionName.empty())
    {
        THROW_HR_WITH_USER_ERROR(
            E_INVALIDARG, wsl::shared::Localization::MessageMissingArgument(L"<SessionName>", L"wsladiag shell"));
    }

    const auto log = [&](std::wstring_view msg) {
        if (verbose)
        {
            wslutil::PrintMessage(std::wstring(msg), stdout);
        }
    };

    log(std::format(L"[diag] shell='{}'", sessionName));

    wil::com_ptr<IWSLAUserSession> userSession;
    THROW_IF_FAILED(CoCreateInstance(__uuidof(WSLAUserSession), nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&userSession)));
    wsl::windows::common::security::ConfigureForCOMImpersonation(userSession.get());

    wil::com_ptr<IWSLASession> session;
    HRESULT hr = userSession->OpenSessionByName(sessionName.c_str(), &session);
    if (FAILED(hr))
    {
        if (hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND))
        {
            wslutil::PrintMessage(Localization::MessageWslaSessionNotFound(sessionName.c_str()), stderr);
            return 1;
        }

        return ReportError(Localization::MessageWslaOpenSessionFailed(sessionName.c_str()), hr);
    }

    log(L"[diag] OpenSessionByName succeeded");

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

    log(std::format(L"[diag] tty rows={} cols={}", rows, cols));
    launcher.SetTtySize(rows, cols);

    log(L"[diag] launching shell process...");
    auto process = launcher.Launch(*session);
    log(L"[diag] shell launched (TTY)");

    auto ttyIn = process.GetStdHandle(0);
    auto ttyOut = process.GetStdHandle(1);

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

    auto restoreConsole = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&] {
        LOG_IF_WIN32_BOOL_FALSE(SetConsoleMode(consoleIn, originalInMode));
        LOG_IF_WIN32_BOOL_FALSE(SetConsoleMode(consoleOut, originalOutMode));
        LOG_IF_WIN32_BOOL_FALSE(SetConsoleOutputCP(originalOutCP));
        LOG_IF_WIN32_BOOL_FALSE(SetConsoleCP(originalInCP));
    });

    // Console mode for interactive terminal.
    DWORD inMode = originalInMode;
    WI_SetAllFlags(inMode, ENABLE_WINDOW_INPUT | ENABLE_VIRTUAL_TERMINAL_INPUT);
    WI_ClearAllFlags(inMode, ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT | ENABLE_INSERT_MODE | ENABLE_PROCESSED_INPUT);
    THROW_IF_WIN32_BOOL_FALSE(SetConsoleMode(consoleIn, inMode));

    DWORD outMode = originalOutMode;
    WI_SetAllFlags(outMode, ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING | DISABLE_NEWLINE_AUTO_RETURN);
    THROW_IF_WIN32_BOOL_FALSE(SetConsoleMode(consoleOut, outMode));

    THROW_LAST_ERROR_IF(!SetConsoleOutputCP(CP_UTF8));
    THROW_LAST_ERROR_IF(!SetConsoleCP(CP_UTF8));

    auto exitEvent = wil::unique_event(wil::EventOptions::ManualReset);

    auto ttyControl = process.GetStdHandle(2); // TerminalControl
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

    // Start input relay thread to forward console input to TTY
    // Runs in parallel with output relay (main thread)
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

    auto [exitCode, signalled] = process.GetExitState();

    std::wstring shellWide(shell.begin(), shell.end());
    wslutil::PrintMessage(Localization::MessageWslaShellExited(shellWide.c_str(), exitCode), stdout);

    return 0;
}

static int RunListCommand()
{
    wil::com_ptr<IWSLAUserSession> userSession;
    THROW_IF_FAILED(CoCreateInstance(__uuidof(WSLAUserSession), nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&userSession)));
    wsl::windows::common::security::ConfigureForCOMImpersonation(userSession.get());

    wil::unique_cotaskmem_array_ptr<WSLA_SESSION_INFORMATION> sessions;
    THROW_IF_FAILED(userSession->ListSessions(&sessions, sessions.size_address<ULONG>()));

    if (sessions.size() == 0)
    {
        wslutil::PrintMessage(Localization::MessageWslaNoSessionsFound(), stdout);
        return 0;
    }

    wslutil::PrintMessage(Localization::MessageWslaSessionsFound(sessions.size(), sessions.size() == 1 ? L"" : L"s"), stdout);
    // Compute column widths from headers + data.
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

static void PrintUsage()
{
    wslutil::PrintMessage(Localization::MessageWsladiagUsage(), stderr);
}

int wsladiag_main(std::wstring_view commandLine)
{
    // Basic initialization that was previously in this function.
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

    // Enable contextualized error collection and create an ExecutionContext so
    // THROW_HR_WITH_USER_ERROR will save a user-visible message.
    wsl::windows::common::EnableContextualizedErrors(false);

    FILE* warningsFile = nullptr;
    const char* disableWarnings = getenv("WSL_DISABLE_WARNINGS");
    if (disableWarnings == nullptr || strcmp(disableWarnings, "1") != 0)
    {
        warningsFile = stderr;
    }

    std::optional<wsl::windows::common::ExecutionContext> context;
    context.emplace(wsl::windows::common::Context::Wsl, warningsFile);

    int exitCode = 0;
    HRESULT result = S_OK;

    try
    {
        ArgumentParser parser(std::wstring{commandLine}, L"wsladiag", 1, true);

        bool help = false;
        std::wstring verb;

        parser.AddPositionalArgument(verb, 0);
        parser.AddArgument(help, L"--help", L'h');

        parser.Parse(); // Let exceptions propagate to this try/catch

        if (help || verb.empty())
        {
            PrintUsage();
            exitCode = 0;
        }
        else if (verb == L"list")
        {
            exitCode = RunListCommand();
        }
        else if (verb == L"shell")
        {
            exitCode = RunShellCommand(commandLine);
        }
        else
        {
            wslutil::PrintMessage(std::format(L"Unknown command: '{}'", verb), stderr);
            PrintUsage();
            exitCode = 1;
        }
    }
    catch (...)
    {
        // Capture the exception HRESULT and fall through to printing contextualized error.
        result = wil::ResultFromCaughtException();
        // Default nonzero exit code on failure.
        exitCode = 1;
    }

    wslutil::PrintMessage(Localization::MessageWslaUnknownCommand(verb.c_str()), stderr);
    PrintUsage();
    return 1;

    // If there was a failure, attempt to print a contextualized error message collected
    // by the ExecutionContext. Otherwise fall back to the HRESULT -> string path.
    if (FAILED(result))
    {
        try
        {
            std::wstring errorString{};
            if (context.has_value() && context->ReportedError().has_value())
            {
                auto strings = wsl::windows::common::wslutil::ErrorToString(context->ReportedError().value());

                // For most errors, show both message and code
                errorString = wsl::shared::Localization::MessageErrorCode(strings.Message, strings.Code);

                // Log telemetry for user-visible errors (matches other tools).
                WSL_LOG("UserVisibleError", TraceLoggingValue(strings.Code.c_str(), "ErrorCode"));
            }
            else
            {
                // Fallback: show basic HRESULT string.
                errorString = wslutil::ErrorCodeToString(result);
            }

            wslutil::PrintMessage(errorString, stderr);
        }
        catch (...)
        {
            LOG_CAUGHT_EXCEPTION();
        }

    }

    return exitCode;
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