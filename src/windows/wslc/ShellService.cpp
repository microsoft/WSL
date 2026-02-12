/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ShellService.cpp

Abstract:

    This file contains the ShellService implementation

--*/
#include "precomp.h"
#include "ShellService.h"
#include <wslaservice.h>
#include <WSLAProcessLauncher.h>

namespace wsl::windows::wslc::services {
using namespace wsl::shared;
namespace wslutil = wsl::windows::common::wslutil;

int ShellService::Attach(std::wstring sessionName)
{
    THROW_HR_IF(E_INVALIDARG, sessionName.empty());

    wil::com_ptr<IWSLASessionManager> manager;
    THROW_IF_FAILED(CoCreateInstance(__uuidof(WSLASessionManager), nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&manager)));
    wsl::windows::common::security::ConfigureForCOMImpersonation(manager.get());

    wil::com_ptr<IWSLASession> session;
    HRESULT hr = manager->OpenSessionByName(sessionName.c_str(), &session);
    if (FAILED(hr))
    {
        if (hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND))
        {
            wslutil::PrintMessage(Localization::MessageWslaSessionNotFound(sessionName.c_str()), stderr);
            return 1;
        }

        auto errorString = wsl::windows::common::wslutil::ErrorCodeToString(hr);
        wslutil::PrintMessage(
            Localization::MessageErrorCode(Localization::MessageWslaOpenSessionFailed(sessionName.c_str()), errorString), stderr);
        return 1;
    }

    wsl::windows::common::security::ConfigureForCOMImpersonation(session.get());

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

    return static_cast<int>(exitCode);
}

std::vector<SessionInformation> ShellService::List()
{
    std::vector<SessionInformation> result;
    wil::com_ptr<IWSLASessionManager> sessionManager;
    THROW_IF_FAILED(CoCreateInstance(__uuidof(WSLASessionManager), nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&sessionManager)));
    wsl::windows::common::security::ConfigureForCOMImpersonation(sessionManager.get());

    wil::unique_cotaskmem_array_ptr<WSLA_SESSION_INFORMATION> sessions;
    THROW_IF_FAILED(sessionManager->ListSessions(&sessions, sessions.size_address<ULONG>()));
    for (size_t i = 0; i < sessions.size(); ++i)
    {
        const auto& current = sessions[i];
        SessionInformation info;
        info.CreatorPid = current.CreatorPid;
        info.SessionId = current.SessionId;
        info.DisplayName = current.DisplayName ? current.DisplayName : L"";
        result.emplace_back(info);
    }

    return result;
}
} // namespace wsl::windows::wslc::services