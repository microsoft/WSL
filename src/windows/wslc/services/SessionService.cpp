/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    SessionService.cpp

Abstract:

    This file contains the SessionService implementation

--*/

#include "precomp.h"
#include "SessionService.h"
#include "ConsoleService.h"
#include "WarningCallback.h"
#include <wslc.h>
#include <WSLCProcessLauncher.h>

namespace wsl::windows::wslc::services {
using namespace wsl::shared;
using namespace wsl::windows::wslc::models;
namespace wslutil = wsl::windows::common::wslutil;

static wil::com_ptr<IWSLCSessionManager> CreateSessionManager()
{
    wil::com_ptr<IWSLCSessionManager> manager;
    THROW_IF_FAILED(CoCreateInstance(__uuidof(WSLCSessionManager), nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&manager)));
    wsl::windows::common::security::ConfigureForCOMImpersonation(manager.get());
    return manager;
}

Session SessionService::OpenSessionByName(const wil::com_ptr<IWSLCSessionManager>& manager, LPCWSTR displayName)
{
    wil::com_ptr<IWSLCSession> session;
    THROW_IF_FAILED(manager->OpenSessionByName(displayName, &session));

    wsl::windows::common::security::ConfigureForCOMImpersonation(session.get());
    return Session(std::move(session));
}

Session SessionService::OpenSession(const std::wstring& sessionName)
{
    return OpenSessionByName(CreateSessionManager(), sessionName.c_str());
}

Session SessionService::OpenDefaultSession()
{
    // Null DisplayName = default session, resolved from caller's token by the server.
    return OpenSessionByName(CreateSessionManager(), nullptr);
}

Session SessionService::OpenOrCreateDefaultSession(Reporter& reporter)
{
    WarningCallback warningCallback(reporter);
    auto manager = CreateSessionManager();

    // Null Settings = default session with server-determined name and settings.
    wil::com_ptr<IWSLCSession> session;
    THROW_IF_FAILED(manager->CreateSession(nullptr, WSLCSessionFlagsNone, &warningCallback, &session));
    wsl::windows::common::security::ConfigureForCOMImpersonation(session.get());
    return Session(std::move(session));
}

int SessionService::Attach(Reporter& reporter, const Session& session)
{
    // Configure console for interactive usage.
    wsl::windows::common::ConsoleState console{};
    console.SetInteractiveMode();
    const auto windowSize = console.GetWindowSize();

    const std::string shell = "/bin/sh";

    // Launch with terminal fds (PTY).
    wsl::windows::common::WSLCProcessLauncher launcher{shell, {shell, "--login"}, {"TERM=xterm-256color"}, WSLCProcessFlagsTty | WSLCProcessFlagsStdin};
    launcher.SetTtySize(windowSize.Y, windowSize.X);
    auto process = launcher.Launch(*session.Get());
    auto tty = process.GetStdHandle(WSLCFDTty);
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

    reporter.Output(L"{}\n", wsl::shared::Localization::MessageWslcShellExited(string::MultiByteToWide(shell), static_cast<int>(exitCode)));

    return static_cast<int>(exitCode);
}

int SessionService::Enter(Reporter& reporter, const std::wstring& storagePath, const std::wstring& displayName)
{
    THROW_HR_IF(E_INVALIDARG, storagePath.empty());
    THROW_HR_IF(E_INVALIDARG, displayName.empty());

    WarningCallback warningCallback(reporter);
    auto sessionManager = CreateSessionManager();

    wil::com_ptr<IWSLCSession> session;
    THROW_IF_FAILED(sessionManager->EnterSession(displayName.c_str(), storagePath.c_str(), &warningCallback, &session));
    wsl::windows::common::security::ConfigureForCOMImpersonation(session.get());
    reporter.Info(L"{}\n", Localization::MessageWslcCreatedSession(displayName));

    const std::string shell = "/bin/sh";
    wsl::windows::common::WSLCProcessLauncher launcher{shell, {shell, "--login"}, {"TERM=xterm-256color"}, WSLCProcessFlagsTty | WSLCProcessFlagsStdin};

    wsl::windows::common::ConsoleState console;
    const auto windowSize = console.GetWindowSize();
    launcher.SetTtySize(windowSize.Y, windowSize.X);

    return ConsoleService::AttachToCurrentConsole(reporter, console, launcher.Launch(*session.get()));
}

std::vector<SessionInformation> SessionService::List()
{
    std::vector<SessionInformation> result;
    auto sessionManager = CreateSessionManager();

    wil::unique_cotaskmem_array_ptr<WSLCSessionListEntry> sessions;
    THROW_IF_FAILED(sessionManager->ListSessions(&sessions, sessions.size_address<ULONG>()));
    for (size_t i = 0; i < sessions.size(); ++i)
    {
        const auto& current = sessions[i];
        SessionInformation info{};
        info.CreatorPid = current.CreatorPid;
        info.SessionId = current.SessionId;
        info.DisplayName = current.DisplayName;
        result.emplace_back(info);
    }

    return result;
}

int SessionService::Run(Reporter& reporter, const Session& session, const std::vector<std::string>& arguments)
{
    WI_ASSERT(!arguments.empty());

    // Pass a default $PATH environment for convenience.
    const std::vector<std::string> environment{"PATH=/bin:/usr/local/sbin:/usr/bin:/usr/sbin:/sbin"};
    wsl::windows::common::WSLCProcessLauncher launcher{arguments.front(), arguments, environment, WSLCProcessFlagsStdin};

    auto [result, process, error] = launcher.LaunchNoThrow(*session.Get());
    THROW_HR_WITH_USER_ERROR_IF(result, Localization::MessageWslcFailedToLaunchCommand(arguments.front(), error), FAILED(result) && error != 0);

    THROW_IF_FAILED(result);

    wsl::windows::common::ConsoleState console{};
    return ConsoleService::AttachToCurrentConsole(reporter, console, std::move(process.value()));
}

int SessionService::TerminateSession(Reporter& reporter, const Session& session)
{
    HRESULT hr = session.Get()->Terminate();
    if (FAILED(hr))
    {
        auto errorString = wsl::windows::common::wslutil::ErrorCodeToString(hr);

        wil::unique_cotaskmem_string displayName;
        if (SUCCEEDED(session.Get()->GetDisplayName(&displayName)) && displayName)
        {
            reporter.Error(L"{}\n", Localization::MessageErrorCode(Localization::MessageWslcTerminateSessionFailed(displayName.get()), errorString));
        }
        else
        {
            reporter.Error(L"{}\n", Localization::MessageErrorCode(Localization::MessageWslcTerminateDefaultSessionFailed(), errorString));
        }
        return 1;
    }

    return 0;
}
} // namespace wsl::windows::wslc::services
