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
#include "wslc_schema.h"

#include <wslc.h>
#include <WSLCProcessLauncher.h>

namespace wsl::windows::wslc::services {
using namespace wsl::shared;
using namespace wsl::shared::string;
using namespace wsl::windows::common;
using namespace wsl::windows::wslc::models;
namespace wslutil = wsl::windows::common::wslutil;

namespace {

    constexpr std::array<std::string_view, 4> c_eventFilterKeys{"type", "event", "container", "image"};

    std::string FormatEventTimestamp(std::int64_t timestamp)
    {
        using namespace std::chrono;

        const sys_seconds time{seconds{timestamp}};
        try
        {
            auto output = std::format("{:%FT%T.000000000%z}", zoned_time{current_zone(), time});
            output.insert(output.size() - 2, ":");
            return output;
        }
        catch (...)
        {
            // The time zone database is unavailable, so report UTC rather than failing the stream.
            LOG_CAUGHT_EXCEPTION();
            return std::format("{:%FT%T.000000000+00:00}", time);
        }
    }

    std::string FormatEvent(const wslc_schema::Event& event)
    {
        auto output = std::format("{} {} {} {}", FormatEventTimestamp(event.time), event.Type, event.Action, event.Actor.ID);
        if (!event.Actor.Attributes.empty())
        {
            output.append(" (");
            bool first = true;
            for (const auto& [key, value] : event.Actor.Attributes)
            {
                if (!first)
                {
                    output.append(", ");
                }

                output.append(std::format("{}={}", key, value));
                first = false;
            }

            output.push_back(')');
        }

        return output;
    }

    void WriteOutput(HANDLE outputHandle, std::string_view output)
    {
        while (!output.empty())
        {
            DWORD bytesWritten{};
            THROW_LAST_ERROR_IF(!WriteFile(outputHandle, output.data(), gsl::narrow_cast<DWORD>(output.size()), &bytesWritten, nullptr));
            THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_WRITE_FAULT), bytesWritten == 0);
            output.remove_prefix(bytesWritten);
        }
    }

    void CancelCallWhenSignaled(HANDLE cancelEvent, HANDLE completedEvent, DWORD threadId) noexcept
    {
        const std::array handles{cancelEvent, completedEvent};
        const auto waitResult = WaitForMultipleObjects(static_cast<DWORD>(handles.size()), handles.data(), FALSE, INFINITE);
        if (waitResult != WAIT_OBJECT_0)
        {
            LOG_LAST_ERROR_IF(waitResult == WAIT_FAILED);
            return;
        }

        while (WaitForSingleObject(completedEvent, 0) != WAIT_OBJECT_0)
        {
            const auto result = CoCancelCall(threadId, 0);
            if (SUCCEEDED(result))
            {
                return;
            }

            if (result != RPC_E_CALL_COMPLETE && result != E_NOINTERFACE)
            {
                LOG_IF_FAILED(result);
                return;
            }

            if (WaitForSingleObject(completedEvent, 10) == WAIT_OBJECT_0)
            {
                return;
            }
        }
    }

} // namespace

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

Session SessionService::OpenOrCreateDefaultSession(Terminal& terminal)
{
    WarningCallback warningCallback(terminal);
    auto manager = CreateSessionManager();

    // Null Settings = default session with server-determined name and settings. The warning callback
    // is consumed during CreateSession (session initialization); it is not retained afterwards.
    wil::com_ptr<IWSLCSession> session;
    THROW_IF_FAILED(manager->CreateSession(nullptr, WSLCSessionFlagsNone, &warningCallback, &session));
    wsl::windows::common::security::ConfigureForCOMImpersonation(session.get());

    return Session(std::move(session));
}

int SessionService::Attach(Terminal& terminal, const Session& session)
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
                GetStdHandle(STD_INPUT_HANDLE), tty.Get(), updateTerminalSize, exitEvent.get());
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
    wsl::windows::common::relay::InterruptableRelay(tty.Get(), GetStdHandle(STD_OUTPUT_HANDLE), exitEvent.get());

    process.GetExitEvent().wait();

    auto exitCode = process.GetExitCode();

    terminal.Output(L"{}\n", wsl::shared::Localization::MessageWslcShellExited(string::MultiByteToWide(shell), static_cast<int>(exitCode)));

    return static_cast<int>(exitCode);
}

int SessionService::Enter(Terminal& terminal, const std::wstring& storagePath, const std::wstring& displayName)
{
    THROW_HR_IF(E_INVALIDARG, storagePath.empty());
    THROW_HR_IF(E_INVALIDARG, displayName.empty());

    WarningCallback warningCallback(terminal);
    auto sessionManager = CreateSessionManager();

    wil::com_ptr<IWSLCSession> session;
    THROW_IF_FAILED(sessionManager->EnterSession(displayName.c_str(), storagePath.c_str(), &warningCallback, &session));
    wsl::windows::common::security::ConfigureForCOMImpersonation(session.get());
    terminal.Info(L"{}\n", Localization::MessageWslcCreatedSession(displayName));

    const std::string shell = "/bin/sh";
    wsl::windows::common::WSLCProcessLauncher launcher{shell, {shell, "--login"}, {"TERM=xterm-256color"}, WSLCProcessFlagsTty | WSLCProcessFlagsStdin};

    wsl::windows::common::ConsoleState console;
    const auto windowSize = console.GetWindowSize();
    launcher.SetTtySize(windowSize.Y, windowSize.X);

    return ConsoleService::AttachToCurrentConsole(terminal, console, launcher.Launch(*session.get()));
}

WSLCVersion SessionService::ManagerVersion()
{
    WSLCVersion version{};
    THROW_IF_FAILED(CreateSessionManager()->GetVersion(&version));

    return version;
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

int SessionService::Run(Terminal& terminal, const Session& session, const std::vector<std::string>& arguments)
{
    WI_ASSERT(!arguments.empty());

    // Pass a default $PATH environment for convenience.
    const std::vector<std::string> environment{"PATH=/bin:/usr/local/sbin:/usr/bin:/usr/sbin:/sbin"};
    wsl::windows::common::WSLCProcessLauncher launcher{arguments.front(), arguments, environment, WSLCProcessFlagsStdin};

    auto [result, process, error] = launcher.LaunchNoThrow(*session.Get());
    THROW_HR_WITH_USER_ERROR_IF(result, Localization::MessageWslcFailedToLaunchCommand(arguments.front(), error), FAILED(result) && error != 0);

    THROW_IF_FAILED(result);

    wsl::windows::common::ConsoleState console{};
    return ConsoleService::AttachToCurrentConsole(terminal, console, std::move(process.value()));
}

void SessionService::StreamEvents(
    const Session& session, LONGLONG since, LONGLONG until, const std::vector<std::pair<std::string, std::string>>& filterValues, HANDLE cancelEvent)
{
    std::vector<WSLCFilter> filterEntries;
    filterEntries.reserve(filterValues.size());
    for (const auto& [key, value] : filterValues)
    {
        THROW_HR_WITH_USER_ERROR_IF(
            E_INVALIDARG,
            Localization::MessageWslcInvalidFilter(MultiByteToWide(key)),
            std::ranges::find(c_eventFilterKeys, key) == c_eventFilterKeys.end());
        filterEntries.push_back({.Key = key.c_str(), .Value = value.c_str()});
    }

    [[maybe_unused]] auto operation = session.BeginContainerOperation();

    wil::com_ptr<IWSLCEventStream> stream;
    THROW_IF_FAILED(session.Get()->GetEvents(
        since, until, filterEntries.empty() ? nullptr : filterEntries.data(), static_cast<ULONG>(filterEntries.size()), &stream));

    // Event output is UTF-8.
    wsl::windows::common::ConsoleState console;
    console.SetOutputCodePageUtf8();
    const auto outputHandle = GetStdHandle(STD_OUTPUT_HANDLE);

    THROW_IF_FAILED(CoEnableCallCancellation(nullptr));
    const auto disableCallCancellation = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, []() { CoDisableCallCancellation(nullptr); });

    wil::unique_event completedEvent(wil::EventOptions::ManualReset);
    std::thread cancellationThread{CancelCallWhenSignaled, cancelEvent, completedEvent.get(), GetCurrentThreadId()};
    const auto cancellationCleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
        completedEvent.SetEvent();
        cancellationThread.join();
    });

    while (true)
    {
        wil::unique_cotaskmem_ansistring eventJson;
        const auto result = stream->GetNext(&eventJson);
        if (result == WSLC_E_EVENT_STREAM_FINISHED)
        {
            return;
        }

        THROW_IF_FAILED(result);

        auto line = FormatEvent(nlohmann::json::parse(eventJson.get()).get<wslc_schema::Event>());
        line.push_back('\n');
        WriteOutput(outputHandle, line);
    }
}

int SessionService::TerminateSession(Terminal& terminal, const Session& session)
{
    HRESULT hr = session.Get()->Terminate();
    if (FAILED(hr))
    {
        auto errorString = wsl::windows::common::wslutil::ErrorCodeToString(hr);

        wil::unique_cotaskmem_string displayName;
        if (SUCCEEDED(session.Get()->GetDisplayName(&displayName)) && displayName)
        {
            terminal.Error(L"{}\n", Localization::MessageErrorCode(Localization::MessageWslcTerminateSessionFailed(displayName.get()), errorString));
        }
        else
        {
            terminal.Error(L"{}\n", Localization::MessageErrorCode(Localization::MessageWslcTerminateDefaultSessionFailed(), errorString));
        }
        return 1;
    }

    return 0;
}
} // namespace wsl::windows::wslc::services
