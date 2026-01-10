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

// Handler for `wsladiag shell <SessionName>` (TTY-backed interactive shell).
static int RunShellCommand(const std::wstring& sessionName, bool verbose)
{
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

    launcher.AddFd(WSLA_PROCESS_FD{.Fd = 0, .Type = WSLAFdTypeTerminalInput});
    launcher.AddFd(WSLA_PROCESS_FD{.Fd = 1, .Type = WSLAFdTypeTerminalOutput});
    launcher.AddFd(WSLA_PROCESS_FD{.Fd = 2, .Type = WSLAFdTypeTerminalControl});
    launcher.SetTtySize(rows, cols);

    log(L"[diag] launching shell process...");
    auto process = launcher.Launch(*session);
    log(L"[diag] shell launched (TTY)");

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
    WI_ClearAllFlags(inMode, ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT | ENABLE_INSERT_MODE | ENABLE_PROCESSED_INPUT);
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

    auto exitCode = process.GetExitCode();

    std::wstring shellWide(shell.begin(), shell.end());
    std::wstring message = Localization::MessageWslaShellExited(shellWide.c_str(), exitCode);

    wslutil::PrintMessage(message, stdout);

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

DEFINE_ENUM_FLAG_OPERATORS(WSLASessionFlags);

static wil::com_ptr<IWSLASession> OpenCLISession()
{
    wil::com_ptr<IWSLAUserSession> userSession;
    THROW_IF_FAILED(CoCreateInstance(__uuidof(WSLAUserSession), nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&userSession)));
    wsl::windows::common::security::ConfigureForCOMImpersonation(userSession.get());

    auto dataFolder = std::filesystem::path(wsl::windows::common::filesystem::GetLocalAppDataPath(nullptr)) / "wsla";

    // TODO: Have a configuration file for those.
    WSLA_SESSION_SETTINGS settings{};
    settings.DisplayName = L"wsla-cli";
    settings.CpuCount = 4;
    settings.MemoryMb = 2024;
    settings.BootTimeoutMs = 30 * 1000;
    settings.StoragePath = dataFolder.c_str();
    settings.MaximumStorageSizeMb = 10000; // 10GB.
    settings.NetworkingMode = WSLANetworkingModeNAT;

    wil::com_ptr<IWSLASession> session;
    THROW_IF_FAILED(userSession->CreateSession(&settings, WSLASessionFlagsPersistent | WSLASessionFlagsOpenExisting, &session));
    wsl::windows::common::security::ConfigureForCOMImpersonation(session.get());

    return session;
}

static int Pull(std::wstring_view commandLine)
{
    ArgumentParser parser(std::wstring{commandLine}, L"wsladiag", 2);

    std::string image;
    parser.AddPositionalArgument(Utf8String{image}, 0);

    parser.Parse();

    system("pause");

    THROW_HR_IF(E_INVALIDARG, image.empty());

    class DECLSPEC_UUID("7A1D3376-835A-471A-8DC9-23653D9962D0") Callback
        : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IProgressCallback, IFastRundown>
    {
    public:
        auto MoveToLine(CONSOLE_SCREEN_BUFFER_INFO& Current, SHORT Line)
        {
            THROW_IF_WIN32_BOOL_FALSE(SetConsoleCursorPosition(GetStdHandle(STD_OUTPUT_HANDLE), COORD{.X = 0, .Y = Line}));

            return wil::scope_exit([&]() {
                THROW_IF_WIN32_BOOL_FALSE(SetConsoleCursorPosition(GetStdHandle(STD_OUTPUT_HANDLE), Current.dwCursorPosition));
            });
        }

        HRESULT OnProgress(LPCSTR Status, LPCSTR Id, ULONGLONG Current, ULONGLONG Total) override
        try
        {
            if (Id == nullptr) // Print all 'global' statuses on their own line
            {
                wprintf(L"%hs\n", Status);
                return S_OK;
            }

            auto info = Info();

            auto it = m_statuses.find(Id);
            if (it == m_statuses.end())
            {
                // If this is the first time we see this ID, create a new line for it.
                m_statuses.emplace(Id, Info().dwCursorPosition.Y);
                wprintf(L"%ls\n", GenerateStatusLine(Status, Id, Current, Total, info).c_str());
            }
            else
            {
                auto revert = MoveToLine(info, it->second);
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
        SHORT m_currentLine = Info().dwCursorPosition.Y;
    };

    wil::com_ptr<IWSLASession> session = OpenCLISession();

    Callback callback;
    THROW_IF_FAILED(session->PullImage(image.c_str(), nullptr, &callback));

    return 0;
}

static void PrintUsage()
{
    wslutil::PrintMessage(Localization::MessageWsladiagUsage(), stderr);
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

    try
    {
        parser.Parse();
    }
    catch (...)
    {
        const auto hr = wil::ResultFromCaughtException();
        if (hr == E_INVALIDARG)
        {
            PrintUsage();
            return 1;
        }
        throw;
    }

    if (help || verb.empty())
    {
        PrintUsage();
        return 0;
    }
    else if (verb == L"list")
    {
        return RunListCommand(verbose);
    }
    else if (verb == L"shell")
    {
        if (shellSession.empty())
        {
            PrintUsage();
            return 1;
        }
        return RunShellCommand(shellSession, verbose);
    }
    else if (verb == L"pull")
    {
        return Pull(commandLine);
    }
    else
    {
        wslutil::PrintMessage(Localization::MessageWslaUnknownCommand(verb.c_str()), stderr);
        PrintUsage();

        return 1;
    }
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
