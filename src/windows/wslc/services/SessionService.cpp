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
#include <wslc.h>
#include <WSLCProcessLauncher.h>

namespace wsl::windows::wslc::services {
using namespace wsl::shared;
using namespace wsl::windows::wslc::models;
namespace wslutil = wsl::windows::common::wslutil;

int SessionService::Attach(const std::wstring& sessionName)
{
    THROW_HR_IF(E_INVALIDARG, sessionName.empty());

    wil::com_ptr<IWSLCSessionManager> manager;
    THROW_IF_FAILED(CoCreateInstance(__uuidof(WSLCSessionManager), nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&manager)));
    wsl::windows::common::security::ConfigureForCOMImpersonation(manager.get());

    wil::com_ptr<IWSLCSession> session;
    HRESULT hr = manager->OpenSessionByName(sessionName.c_str(), &session);
    if (FAILED(hr))
    {
        if (hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND))
        {
            wslutil::PrintMessage(Localization::MessageWslcSessionNotFound(sessionName.c_str()), stderr);
            return 1;
        }

        auto errorString = wsl::windows::common::wslutil::ErrorCodeToString(hr);
        wslutil::PrintMessage(
            Localization::MessageErrorCode(Localization::MessageWslcOpenSessionFailed(sessionName.c_str()), errorString), stderr);
        return 1;
    }

    wsl::windows::common::security::ConfigureForCOMImpersonation(session.get());

    // Configure console for interactive usage.
    wsl::windows::common::ConsoleState console{};
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

Session SessionService::CreateSession(const SessionOptions& options)
{
    const WSLCSessionSettings* settings = options.Get();
    wil::com_ptr<IWSLCSessionManager> sessionManager;
    THROW_IF_FAILED(CoCreateInstance(__uuidof(WSLCSessionManager), nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&sessionManager)));
    wsl::windows::common::security::ConfigureForCOMImpersonation(sessionManager.get());

    wil::com_ptr<IWSLCSession> session;
    THROW_IF_FAILED(sessionManager->CreateSession(settings, WSLCSessionFlagsPersistent | WSLCSessionFlagsOpenExisting, &session));
    wsl::windows::common::security::ConfigureForCOMImpersonation(session.get());
    return Session(std::move(session));
}

int SessionService::Enter(const std::wstring& storagePath, const std::wstring& displayName)
{
    THROW_HR_IF(E_INVALIDARG, storagePath.empty());
    THROW_HR_IF(E_INVALIDARG, displayName.empty());

    // Build session settings from the user configuration, overriding storage path and display name.
    SessionOptions options;
    options.Get()->DisplayName = displayName.c_str();
    options.Get()->StoragePath = storagePath.c_str();
    options.Get()->StorageFlags = WSLCSessionStorageFlagsNoCreate; // Don't create storage if it doesn't exist.

    // Create a non-persistent session: lifetime is tied to our COM reference.
    // TODO: Consider adding a 'create' verb to do that.
    auto session = SessionService::CreateSession(options);
    wsl::windows::common::wslutil::PrintMessage(Localization::MessageWslcCreatedSession(displayName), stderr);

    const std::string shell = "/bin/sh";
    wsl::windows::common::WSLCProcessLauncher launcher{shell, {shell, "--login"}, {"TERM=xterm-256color"}, WSLCProcessFlagsTty | WSLCProcessFlagsStdin};

    wsl::windows::common::ConsoleState console;
    const auto windowSize = console.GetWindowSize();
    launcher.SetTtySize(windowSize.Y, windowSize.X);

    return ConsoleService::AttachToCurrentConsole(launcher.Launch(*session.Get()));
}

std::vector<SessionInformation> SessionService::List()
{
    std::vector<SessionInformation> result;
    wil::com_ptr<IWSLCSessionManager> sessionManager;
    THROW_IF_FAILED(CoCreateInstance(__uuidof(WSLCSessionManager), nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&sessionManager)));
    wsl::windows::common::security::ConfigureForCOMImpersonation(sessionManager.get());

    wil::unique_cotaskmem_array_ptr<WSLCSessionInformation> sessions;
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
    THROW_IF_FAILED(sessionManager->OpenSessionByName(displayName.c_str(), &session));
    wsl::windows::common::security::ConfigureForCOMImpersonation(session.get());
    return Session(std::move(session));
}

int SessionService::TerminateSession(const std::wstring& displayName)
{
    THROW_HR_IF(E_INVALIDARG, displayName.empty());

    wil::com_ptr<IWSLCSessionManager> sessionManager;
    THROW_IF_FAILED(CoCreateInstance(__uuidof(WSLCSessionManager), nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&sessionManager)));
    wsl::windows::common::security::ConfigureForCOMImpersonation(sessionManager.get());

    wil::com_ptr<IWSLCSession> session;
    HRESULT hr = sessionManager->OpenSessionByName(displayName.c_str(), &session);
    if (FAILED(hr))
    {
        if (hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND))
        {
            wslutil::PrintMessage(Localization::MessageWslcSessionNotFound(displayName.c_str()), stderr);
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
            Localization::MessageErrorCode(Localization::MessageWslcTerminateSessionFailed(displayName.c_str()), errorString), stderr);
        return 1;
    }

    return 0;
}
} // namespace wsl::windows::wslc::services
