#include <precomp.h>
#include "ShellService.h"
#include "Utils.h"

namespace wslc::services
{
using namespace wsl::shared;
namespace wslutil = wsl::windows::common::wslutil;

int ShellService::Attach(std::wstring sessionName)
{
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
    const UINT originalOutCP = GetConsoleOutputCP();
    const UINT originalInCP = GetConsoleCP();

    THROW_LAST_ERROR_IF(!GetConsoleMode(consoleIn, &originalInMode));
    THROW_LAST_ERROR_IF(!GetConsoleMode(consoleOut, &originalOutMode));

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

    auto updateTerminalSize = [&]() {
        CONSOLE_SCREEN_BUFFER_INFOEX infoEx{};
        infoEx.cbSize = sizeof(infoEx);
        THROW_IF_WIN32_BOOL_FALSE(GetConsoleScreenBufferInfoEx(consoleOut, &infoEx));

        LOG_IF_FAILED(process.Get().ResizeTty(
            infoEx.srWindow.Bottom - infoEx.srWindow.Top + 1, infoEx.srWindow.Right - infoEx.srWindow.Left + 1));
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

    auto joinInput = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&] {
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
    wslutil::PrintMessage(wsl::shared::Localization::MessageWslaShellExited(shellWide.c_str(), static_cast<int>(exitCode)), stdout);

    return static_cast<int>(exitCode);
}

std::vector<SessionInformation> ShellService::List()
{
    std::vector<SessionInformation> result;
    wil::com_ptr<IWSLASessionManager> sessionManager;
    THROW_IF_FAILED(CoCreateInstance(__uuidof(WSLASessionManager), nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&sessionManager)));
    wsl::windows::common::security::ConfigureForCOMImpersonation(sessionManager.get());

    wil::unique_cotaskmem_array_ptr<WSLA_SESSION_INFORMATION> sessions;
    (sessionManager->ListSessions(&sessions, sessions.size_address<ULONG>()));
    for (size_t i = 0; i < sessions.size(); ++i)
    {
        const auto& current = sessions[i];
        SessionInformation info;
        info.CreatorPid = current.CreatorPid;
        info.SessionId = current.SessionId;
        info.DisplayName = current.DisplayName;
        result.emplace_back(info);
    }

    return result;
}
}