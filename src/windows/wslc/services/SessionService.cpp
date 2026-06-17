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

namespace {

    wil::com_ptr<IWSLCSession> OpenOrCreateSession(const std::wstring& sessionName)
    {
        wil::com_ptr<IWSLCSessionManager> manager;
        THROW_IF_FAILED(CoCreateInstance(__uuidof(WSLCSessionManager), nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&manager)));
        wsl::windows::common::security::ConfigureForCOMImpersonation(manager.get());

        wil::com_ptr<IWSLCSession> session;
        if (sessionName.empty())
        {
            // Default session: open it if it exists, otherwise create it.
            auto warningCallback = Microsoft::WRL::Make<WarningCallback>();
            THROW_IF_FAILED(manager->CreateSession(nullptr, WSLCSessionFlagsNone, warningCallback.Get(), &session));
            wsl::windows::common::security::ConfigureForCOMImpersonation(session.get());
            return session;
        }

        HRESULT hr = manager->OpenSessionByName(sessionName.c_str(), &session);
        if (FAILED(hr))
        {
            THROW_HR_WITH_USER_ERROR_IF(hr, Localization::MessageWslcSessionNotFound(sessionName.c_str()), hr == WSLC_E_SESSION_NOT_FOUND);

            THROW_HR_WITH_USER_ERROR(hr, Localization::MessageWslcOpenSessionFailed(sessionName.c_str()));
        }

        wsl::windows::common::security::ConfigureForCOMImpersonation(session.get());

        return session;
    }

} // namespace

int SessionService::Attach(const std::wstring& sessionName)
{
    auto session = OpenOrCreateSession(sessionName);

    // Configure console for interactive usage.
    wsl::windows::common::ConsoleState console{};
    console.SetInteractiveMode();
    const auto windowSize = console.GetWindowSize();

    const std::string shell = "/bin/sh";

    // Launch with terminal fds (PTY).
    wsl::windows::common::WSLCProcessLauncher launcher{shell, {shell, "--login"}, {"TERM=xterm-256color"}, WSLCProcessFlagsTty | WSLCProcessFlagsStdin};
    launcher.SetTtySize(windowSize.Y, windowSize.X);
    auto process = launcher.Launch(*session);
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

    wslutil::PrintMessage(wsl::shared::Localization::MessageWslcShellExited(string::MultiByteToWide(shell), static_cast<int>(exitCode)), stdout);

    return static_cast<int>(exitCode);
}

Session SessionService::CreateDefaultSession()
{
    wil::com_ptr<IWSLCSessionManager> sessionManager;
    THROW_IF_FAILED(CoCreateInstance(__uuidof(WSLCSessionManager), nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&sessionManager)));
    wsl::windows::common::security::ConfigureForCOMImpersonation(sessionManager.get());

    // Null Settings = default session with server-determined name and settings.
    wil::com_ptr<IWSLCSession> session;
    auto warningCallback = Microsoft::WRL::Make<WarningCallback>();
    THROW_IF_FAILED(sessionManager->CreateSession(nullptr, WSLCSessionFlagsNone, warningCallback.Get(), &session));
    wsl::windows::common::security::ConfigureForCOMImpersonation(session.get());
    return Session(std::move(session));
}

int SessionService::Enter(const std::wstring& storagePath, const std::wstring& displayName)
{
    THROW_HR_IF(E_INVALIDARG, storagePath.empty());
    THROW_HR_IF(E_INVALIDARG, displayName.empty());

    wil::com_ptr<IWSLCSessionManager> sessionManager;
    THROW_IF_FAILED(CoCreateInstance(__uuidof(WSLCSessionManager), nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&sessionManager)));
    wsl::windows::common::security::ConfigureForCOMImpersonation(sessionManager.get());

    wil::com_ptr<IWSLCSession> session;
    auto warningCallback = Microsoft::WRL::Make<WarningCallback>();
    THROW_IF_FAILED(sessionManager->EnterSession(displayName.c_str(), storagePath.c_str(), warningCallback.Get(), &session));
    wsl::windows::common::security::ConfigureForCOMImpersonation(session.get());
    wsl::windows::common::wslutil::PrintMessage(Localization::MessageWslcCreatedSession(displayName), stderr);

    const std::string shell = "/bin/sh";
    wsl::windows::common::WSLCProcessLauncher launcher{shell, {shell, "--login"}, {"TERM=xterm-256color"}, WSLCProcessFlagsTty | WSLCProcessFlagsStdin};

    wsl::windows::common::ConsoleState console;
    const auto windowSize = console.GetWindowSize();
    launcher.SetTtySize(windowSize.Y, windowSize.X);

    return ConsoleService::AttachToCurrentConsole(console, launcher.Launch(*session.get()));
}

std::vector<SessionInformation> SessionService::List()
{
    std::vector<SessionInformation> result;
    wil::com_ptr<IWSLCSessionManager> sessionManager;
    THROW_IF_FAILED(CoCreateInstance(__uuidof(WSLCSessionManager), nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&sessionManager)));
    wsl::windows::common::security::ConfigureForCOMImpersonation(sessionManager.get());

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

Session SessionService::OpenSession(const std::wstring& displayName)
{
    wil::com_ptr<IWSLCSessionManager> sessionManager;
    THROW_IF_FAILED(CoCreateInstance(__uuidof(WSLCSessionManager), nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&sessionManager)));
    wsl::windows::common::security::ConfigureForCOMImpersonation(sessionManager.get());

    wil::com_ptr<IWSLCSession> session;
    HRESULT hr = sessionManager->OpenSessionByName(displayName.c_str(), &session);
    if (FAILED(hr))
    {
        THROW_HR_WITH_USER_ERROR_IF(hr, Localization::MessageWslcSessionNotFound(displayName.c_str()), hr == WSLC_E_SESSION_NOT_FOUND);

        THROW_HR_WITH_USER_ERROR(hr, Localization::MessageWslcOpenSessionFailed(displayName.c_str()));
    }

    wsl::windows::common::security::ConfigureForCOMImpersonation(session.get());
    return Session(std::move(session));
}

int SessionService::Run(const std::wstring& sessionName, const std::vector<std::string>& arguments)
{
    WI_ASSERT(!arguments.empty());

    auto session = OpenOrCreateSession(sessionName);

    // Pass a default $PATH environment for convenience.
    const std::vector<std::string> environment{"PATH=/bin:/usr/local/sbin:/usr/bin:/usr/sbin:/sbin"};
    wsl::windows::common::WSLCProcessLauncher launcher{arguments.front(), arguments, environment, WSLCProcessFlagsStdin};

    auto [result, process, error] = launcher.LaunchNoThrow(*session);
    THROW_HR_WITH_USER_ERROR_IF(result, Localization::MessageWslcFailedToLaunchCommand(arguments.front(), error), FAILED(result) && error != 0);

    THROW_IF_FAILED(result);

    wsl::windows::common::ConsoleState console{};
    return ConsoleService::AttachToCurrentConsole(console, std::move(process.value()));
}

int SessionService::TerminateSession(const std::wstring& displayName)
{
    wil::com_ptr<IWSLCSessionManager> sessionManager;
    THROW_IF_FAILED(CoCreateInstance(__uuidof(WSLCSessionManager), nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&sessionManager)));
    wsl::windows::common::security::ConfigureForCOMImpersonation(sessionManager.get());

    wil::com_ptr<IWSLCSession> session;
    HRESULT hr = sessionManager->OpenSessionByName(displayName.empty() ? nullptr : displayName.c_str(), &session);
    if (FAILED(hr))
    {
        if (hr == WSLC_E_SESSION_NOT_FOUND)
        {
            wslutil::PrintMessage(
                displayName.empty() ? Localization::MessageWslcDefaultSessionNotFound()
                                    : Localization::MessageWslcSessionNotFound(displayName.c_str()),
                stderr);
            return 1;
        }

        THROW_HR(hr);
    }

    wsl::windows::common::security::ConfigureForCOMImpersonation(session.get());

    hr = session->Terminate();
    if (FAILED(hr))
    {
        auto errorString = wsl::windows::common::wslutil::ErrorCodeToString(hr);
        wslutil::PrintMessage(
            Localization::MessageErrorCode(
                displayName.empty() ? Localization::MessageWslcTerminateDefaultSessionFailed()
                                    : Localization::MessageWslcTerminateSessionFailed(displayName.c_str()),
                errorString),
            stderr);
        return 1;
    }

    return 0;
}
} // namespace wsl::windows::wslc::services
