/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    wsladiag.cpp

Abstract:

    Entry point for the wsladiag tool. Provides diagnostic commands for WSLA sessions.

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
using wsl::windows::common::Context;
using wsl::windows::common::ExecutionContext;
using wsl::windows::common::ProcessFlags;
using wsl::windows::common::WSLAProcessLauncher;

// Defaults used when auto-creating the WSLA session.
static constexpr LPCWSTR kCanonicalSessionDisplayName = L"WSLAShell";
static constexpr LPCWSTR kDefaultSessionName = L"WSLAShell";
static constexpr uint32_t kDefaultCpuCount = 4;
static constexpr uint32_t kDefaultMemoryMb = 4096;
static constexpr uint32_t kDefaultBootTimeoutMs = 30 * 1000;

// Report an operation failure with localized context and HRESULT details.
static int ReportError(const std::wstring& context, HRESULT hr)
{
    auto errorString = wsl::windows::common::wslutil::ErrorCodeToString(hr);
    wslutil::PrintMessage(Localization::MessageErrorCode(context, errorString), stderr);
    return 1;
}

// Enumerate sessions from user session COM interface.
static wil::unique_cotaskmem_array_ptr<WSLA_SESSION_INFORMATION> EnumerateSessions(IWSLAUserSession* userSession)
{
    wil::unique_cotaskmem_array_ptr<WSLA_SESSION_INFORMATION> sessions;
    THROW_IF_FAILED(userSession->ListSessions(&sessions, sessions.size_address<ULONG>()));
    return sessions;
}

static int LaunchContainerCommand(IWSLASession& session, const std::vector<std::string>& containerArgs, const std::function<void(std::wstring_view)>& log)
{
    static const std::vector<std::string> kCandidates = {
        "/usr/bin/docker",
        "/usr/local/bin/docker",
        "/usr/bin/nerdctl",
        "/usr/local/bin/nerdctl",
    };

    const auto flags = ProcessFlags::Stdout | ProcessFlags::Stderr;

    const bool isRun = (!containerArgs.empty() && containerArgs[0] == "run");
    constexpr DWORD kRunTimeoutMs = 300000;  // 5 minutes for run/pull
    constexpr DWORD kOtherTimeoutMs = 60000; // 60 seconds for start/stop/rm/inspect
    const DWORD timeoutMs = isRun ? kRunTimeoutMs : kOtherTimeoutMs;

    auto to_wstring = [](const std::string& s) { return wsl::shared::string::MultiByteToWide(s); };

    // Find --name <name> if present (used for verification)
    auto getNameArg = [&]() -> std::optional<std::string> {
        auto it = std::find(containerArgs.begin(), containerArgs.end(), "--name");
        if (it != containerArgs.end() && std::next(it) != containerArgs.end())
        {
            return *std::next(it);
        }
        return std::nullopt;
    };

    const auto nameOpt = getNameArg();

    for (const auto& binPath : kCandidates)
    {
        const std::wstring binPathW = to_wstring(binPath);

        // Build argv: [binPath, ...containerArgs]
        std::vector<std::string> args;
        args.reserve(1 + containerArgs.size());
        args.push_back(binPath);
        args.insert(args.end(), containerArgs.begin(), containerArgs.end());

        WSLAProcessLauncher launcher{binPath, args, {}, flags};
        auto [hr, errorCode, processOpt] = launcher.LaunchNoThrow(session);

        if (FAILED(hr))
        {
            // Try next runtime only for "not found"
            if (errorCode == ERROR_FILE_NOT_FOUND || errorCode == ERROR_PATH_NOT_FOUND || errorCode == 2 /*ENOENT*/)
            {
                log(std::format(L"[diag] {} not found, trying next runtime", binPathW));
                continue;
            }

            wslutil::PrintMessage(
                std::format(L"Failed to launch {}: errno={}, hr=0x{:08x}", binPathW, errorCode, static_cast<unsigned int>(hr)), stderr);
            return 1;
        }

        log(std::format(L"[diag] Successfully launched container runtime from {}", binPathW));

        auto process = std::move(processOpt.value());
        auto result = process.WaitAndCaptureOutput(timeoutMs);
        const int exitCode = result.Code;

        // Log exact command
        {
            std::wstring cmdDebug = binPathW;
            for (const auto& a : containerArgs)
            {
                cmdDebug += L" " + to_wstring(a);
            }
            log(std::format(L"[diag] Executed: {}", cmdDebug));
            log(std::format(L"[diag] WaitAndCaptureOutput: Code={}, OutputCount={}", exitCode, result.Output.size()));
        }

        // Collect stdout/stderr (UTF‑8)
        std::string stdoutUtf8;
        std::string stderrUtf8;
        for (const auto& [fd, output] : result.Output)
        {
            if (output.empty())
            {
                continue;
            }

            if (fd == 1)
            {
                stdoutUtf8 += output;
            }
            else if (fd == 2)
            {
                stderrUtf8 += output;
            }
        }

        // Trim trailing whitespace/newlines from captured output.
        auto trimTrailingWhitespace = [](std::string& s) {
            while (!s.empty() && isspace(static_cast<unsigned char>(s.back())))
            {
                s.pop_back();
            }
        };

        trimTrailingWhitespace(stdoutUtf8);
        trimTrailingWhitespace(stderrUtf8);

        if (!stdoutUtf8.empty())
        {
            log(std::format(L"[diag] stdout: {}", wsl::shared::string::MultiByteToWide(stdoutUtf8)));
        }
        if (!stderrUtf8.empty())
        {
            log(std::format(L"[diag] stderr: {}", wsl::shared::string::MultiByteToWide(stderrUtf8)));
        }

        log(std::format(L"[diag] Process exit code: {}", exitCode));

        // Normal success
        if (exitCode == 0)
        {
            log(L"[diag] Container command completed successfully");
            return 0;
        }

        // Verification path: "run" + "--name" -> verify existence via "<runtime> ps -a --filter name=... --format {{.ID}}"
        if (isRun && nameOpt.has_value())
        {
            const std::string& containerName = *nameOpt;
            std::vector<std::string> verifyArgs = {
                binPath,
                "ps",
                "-a",
                "--filter",
                "name=" + containerName,
                "--format",
                "{{.ID}}",
            };

            WSLAProcessLauncher verifyLauncher{binPath, verifyArgs, {}, flags};
            auto [vHr, vErr, vProcOpt] = verifyLauncher.LaunchNoThrow(session);

            if (SUCCEEDED(vHr) && vProcOpt.has_value())
            {
                auto vProc = std::move(vProcOpt.value());
                auto vRes = vProc.WaitAndCaptureOutput(kOtherTimeoutMs);

                std::string vStdout;
                for (const auto& [fd, out] : vRes.Output)
                {
                    if (fd == 1 && !out.empty())
                    {
                        vStdout += out;
                    }
                }

                // Trim trailing whitespace/newlines
                while (!vStdout.empty() && isspace(static_cast<unsigned char>(vStdout.back())))
                {
                    vStdout.pop_back();
                }

                if (!vStdout.empty())
                {
                    log(std::format(L"[diag] Verified container exists (docker ps returned id: {})", wsl::shared::string::MultiByteToWide(vStdout)));
                    log(L"[diag] Treating as success despite non-zero exit code");
                    return 0;
                }

                log(L"[diag] Verification via docker ps returned no container id");
            }
            else
            {
                log(std::format(L"[diag] Verification launch failed: errno={}, hr=0x{:08x}", vErr, static_cast<unsigned int>(vHr)));
            }
        }

        // Real failure: print stderr if we have it, otherwise generic
        if (!stderrUtf8.empty())
        {
            wslutil::PrintMessage(wsl::shared::string::MultiByteToWide(stderrUtf8), stderr);
        }

        wslutil::PrintMessage(std::format(L"Container command failed with exit code: {}", exitCode), stderr);
        return exitCode;
    }

    wslutil::PrintMessage(L"Error: Docker or nerdctl not found in session", stderr);
    return 1;
}
// Get or create default WSLA session for container operations.
static wil::com_ptr<IWSLASession> GetOrCreateDefaultSession(IWSLAUserSession* userSession, const std::function<void(std::wstring_view)>& log)
{
    wil::com_ptr<IWSLASession> session;

    log(L"[diag] Enumerating sessions...");
    auto sessions = EnumerateSessions(userSession);
    log(std::format(L"[diag] ListSessions returned {} sessions", sessions.size()));

    const wchar_t* foundName = nullptr;

    for (const auto& s : sessions)
    {
        if (wcscmp(s.DisplayName, kCanonicalSessionDisplayName) == 0 || wcscmp(s.DisplayName, kDefaultSessionName) == 0)
        {
            foundName = s.DisplayName;
            log(std::format(L"[diag] Found existing session '{}'", foundName));
            break;
        }
    }

    if (foundName != nullptr)
    {
        log(L"[diag] Opening session by name...");
        HRESULT hr = userSession->OpenSessionByName(foundName, &session);
        if (SUCCEEDED(hr))
        {
            log(L"[diag] Opened session by name");
            return session;
        }

        log(std::format(L"[diag] OpenSessionByName('{}') failed (hr=0x{:08x})", foundName, static_cast<unsigned int>(hr)));
        THROW_IF_FAILED(hr);
    }

    log(L"[diag] Default session not found, creating...");

    WSLA_SESSION_SETTINGS settings{};
    settings.DisplayName = kDefaultSessionName;
    settings.CpuCount = kDefaultCpuCount;
    settings.MemoryMb = kDefaultMemoryMb;
    settings.NetworkingMode = WSLANetworkingModeNAT;
    settings.BootTimeoutMs = kDefaultBootTimeoutMs;

    THROW_IF_FAILED(userSession->CreateSession(&settings, &session));
    log(std::format(L"[diag] Created session with display name: {}", kDefaultSessionName));

    return session;
}

// Handler for `wsladiag shell <SessionName>` command.
static int RunShellCommand(std::wstring_view commandLine)
{
    std::wstring sessionName;
    bool verbose = false;

    ArgumentParser parser(std::wstring{commandLine}, L"wsladiag", 2, false);
    parser.AddPositionalArgument(sessionName, 0);
    parser.AddArgument(verbose, L"--verbose", L'v');

    parser.Parse();

    if (sessionName.empty())
    {
        THROW_HR_WITH_USER_ERROR(
            E_INVALIDARG, wsl::shared::Localization::MessageMissingArgument(L"<SessionName>", L"wsladiag shell"));
    }

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

    if (verbose)
    {
        wslutil::PrintMessage(std::format(L"[diag] Session opened: '{}'", sessionName), stdout);
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
  
    auto process = launcher.Launch(*session);

    if (verbose)
    {
        wslutil::PrintMessage(L"[diag] Shell process launched", stdout);
    }

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

// Handler for `wsladiag list` command.
static int RunListCommand(std::wstring_view commandLine)
{
    bool verbose = false;

    ArgumentParser parser(std::wstring{commandLine}, L"wsladiag", 2, false);
    parser.AddArgument(verbose, L"--verbose", L'v');

    try
    {
        parser.Parse();
    }
    catch (...)
    {
        THROW_HR_WITH_USER_ERROR(E_INVALIDARG, wsl::shared::Localization::MessageWsladiagUsage());
    }

    wil::com_ptr<IWSLAUserSession> userSession;
    THROW_IF_FAILED(CoCreateInstance(__uuidof(WSLAUserSession), nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&userSession)));
    wsl::windows::common::security::ConfigureForCOMImpersonation(userSession.get());

    wil::unique_cotaskmem_array_ptr<WSLA_SESSION_INFORMATION> sessions;
    THROW_IF_FAILED(userSession->ListSessions(&sessions, sessions.size_address<ULONG>()));

    if (verbose)
    {
        const wchar_t* plural = sessions.size() == 1 ? L"" : L"s";
        wslutil::PrintMessage(std::format(L"[diag] Found {} session{}", sessions.size(), plural), stdout);
    }

    if (sessions.size() == 0)
    {
        wslutil::PrintMessage(Localization::MessageWslaNoSessionsFound(), stdout);
        return 0;
    }

    wslutil::PrintMessage(Localization::MessageWslaSessionsFound(sessions.size(), sessions.size() == 1 ? L"" : L"s"), stdout);

    // Use localized headers
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

// Handler: wsladiag create --name <container> <image>
// Uses COM API to create a container and starts it immediately (run -d semantics).
static int RunCreateCommand(std::wstring_view commandLine)
{
    std::wstring imageW;
    std::wstring nameW;
    bool verbose = false;

    ArgumentParser parser(std::wstring{commandLine}, L"wsladiag", 2, false);
    parser.AddPositionalArgument(imageW, 0);
    parser.AddArgument(nameW, L"--name", L'n');
    parser.AddArgument(verbose, L"--verbose", L'v');
    parser.Parse();

    if (imageW.empty())
    {
        THROW_HR_WITH_USER_ERROR(E_INVALIDARG, wsl::shared::Localization::MessageMissingArgument(L"<image>", L"wsladiag create"));
    }
    if (nameW.empty())
    {
        THROW_HR_WITH_USER_ERROR(
            E_INVALIDARG, wsl::shared::Localization::MessageMissingArgument(L"--name <container>", L"wsladiag create"));
    }

    const auto log = [&](std::wstring_view m) {
        if (verbose)
        {
            wslutil::PrintMessage(std::wstring(m), stdout);
        }
    };

    log(std::format(L"[diag] Creating container '{}' from image '{}'", nameW, imageW));

    // Open user session and get/create the default WSLA session.
    wil::com_ptr<IWSLAUserSession> userSession;
    THROW_IF_FAILED(CoCreateInstance(__uuidof(WSLAUserSession), nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&userSession)));
    wsl::windows::common::security::ConfigureForCOMImpersonation(userSession.get());
    auto session = GetOrCreateDefaultSession(userSession.get(), log);

    // Build container options (strings stay alive in this scope).
    const std::string imageUtf8 = wsl::shared::string::WideToMultiByte(imageW);
    const std::string nameUtf8 = wsl::shared::string::WideToMultiByte(nameW);

    WSLA_PROCESS_OPTIONS init{}; // Empty -> use image entrypoint/command
    WSLA_CONTAINER_NETWORK net{};
    net.ContainerNetworkType = WSLA_CONTAINER_NETWORK_NONE;

    WSLA_CONTAINER_OPTIONS opts{};
    opts.Image = imageUtf8.c_str();
    opts.Name = nameUtf8.c_str();
    opts.InitProcessOptions = init; // Value assignment (per IDL)
    opts.Volumes = nullptr;
    opts.VolumesCount = 0;
    opts.Ports = nullptr; // No port mappings yet
    opts.PortsCount = 0;
    opts.Flags = 0;
    opts.ShmSize = 0;
    opts.ContainerNetwork = net;

    // Create and start the container.
    wil::com_ptr<IWSLAContainer> container;
    THROW_IF_FAILED(session->CreateContainer(&opts, &container));
    THROW_IF_FAILED(container->Start());

    WSLA_CONTAINER_STATE state{};
    THROW_IF_FAILED(container->GetState(&state));
    log(std::format(L"[diag] Created and started container '{}', state={}", nameW, static_cast<int>(state)));

    wslutil::PrintMessage(std::format(L"Created and started container '{}'", nameW), stdout);
    return 0;
}

// Handler: wsladiag start <container>
// Opens and starts an existing container.
static int RunStartCommand(std::wstring_view commandLine)
{
    std::wstring nameW;
    bool verbose = false;
    ArgumentParser p(std::wstring{commandLine}, L"wsladiag", 2, false);
    p.AddPositionalArgument(nameW, 0);
    p.AddArgument(verbose, L"--verbose", L'v');
    p.Parse();
    if (nameW.empty())
    {
        THROW_HR_WITH_USER_ERROR(
            E_INVALIDARG, wsl::shared::Localization::MessageMissingArgument(L"<container>", L"wsladiag start"));
    }

    const auto log = [&](std::wstring_view m)
    {
        if (verbose)
        {
            wslutil::PrintMessage(std::wstring(m), stdout);
        }
    };

    wil::com_ptr<IWSLAUserSession> us;
    THROW_IF_FAILED(CoCreateInstance(__uuidof(WSLAUserSession), nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&us)));
    wsl::windows::common::security::ConfigureForCOMImpersonation(us.get());
    auto session = GetOrCreateDefaultSession(us.get(), log);

    std::string name = wsl::shared::string::WideToMultiByte(nameW);
    wil::com_ptr<IWSLAContainer> c;
    THROW_IF_FAILED(session->OpenContainer(name.c_str(), &c));
    THROW_IF_FAILED(c->Start());

    wslutil::PrintMessage(std::format(L"Started container '{}'", nameW), stdout);
    return 0;
}

// Handler: wsladiag stop <container>
// Stops a running container with a graceful signal and short timeout.
static int RunStopCommand(std::wstring_view commandLine)
{
    std::wstring nameW;
    bool verbose = false;
    ArgumentParser p(std::wstring{commandLine}, L"wsladiag", 2, false);
    p.AddPositionalArgument(nameW, 0);
    p.AddArgument(verbose, L"--verbose", L'v');
    p.Parse();
    if (nameW.empty())
    {
        THROW_HR_WITH_USER_ERROR(
            E_INVALIDARG, wsl::shared::Localization::MessageMissingArgument(L"<container>", L"wsladiag stop"));
    }

    const auto log = [&](std::wstring_view m) {
        if (verbose)
        {
            wslutil::PrintMessage(std::wstring(m), stdout);
        }
    };

    wil::com_ptr<IWSLAUserSession> us;
    THROW_IF_FAILED(CoCreateInstance(__uuidof(WSLAUserSession), nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&us)));
    wsl::windows::common::security::ConfigureForCOMImpersonation(us.get());
    auto session = GetOrCreateDefaultSession(us.get(), log);

    std::string name = wsl::shared::string::WideToMultiByte(nameW);
    wil::com_ptr<IWSLAContainer> c;
    THROW_IF_FAILED(session->OpenContainer(name.c_str(), &c));

    // Align with service-side client (DockerHTTPClient::StopContainer): requires signal + timeout.
    constexpr int kStopSignal = 15;           // SIGTERM
    constexpr ULONG kStopTimeoutSeconds = 10; // seconds
    THROW_IF_FAILED(c->Stop(kStopSignal, kStopTimeoutSeconds));

    wslutil::PrintMessage(std::format(L"Stopped container '{}'", nameW), stdout);
    return 0;
}

// Handler: wsladiag delete <container>
// Deletes an existing container (must be stopped).
static int RunDeleteCommand(std::wstring_view commandLine)
{
    std::wstring nameW;
    bool verbose = false;
    ArgumentParser p(std::wstring{commandLine}, L"wsladiag", 2, false);
    p.AddPositionalArgument(nameW, 0);
    p.AddArgument(verbose, L"--verbose", L'v');
    p.Parse();
    if (nameW.empty())
    {
        THROW_HR_WITH_USER_ERROR(
            E_INVALIDARG, wsl::shared::Localization::MessageMissingArgument(L"<container>", L"wsladiag delete"));
    }

    const auto log = [&](std::wstring_view m) {
        if (verbose)
        {
            wslutil::PrintMessage(std::wstring(m), stdout);
        }
    };

    wil::com_ptr<IWSLAUserSession> us;
    THROW_IF_FAILED(CoCreateInstance(__uuidof(WSLAUserSession), nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&us)));
    wsl::windows::common::security::ConfigureForCOMImpersonation(us.get());
    auto session = GetOrCreateDefaultSession(us.get(), log);

    std::string name = wsl::shared::string::WideToMultiByte(nameW);
    wil::com_ptr<IWSLAContainer> c;
    THROW_IF_FAILED(session->OpenContainer(name.c_str(), &c));
    THROW_IF_FAILED(c->Delete());

    wslutil::PrintMessage(std::format(L"Deleted container '{}'", nameW), stdout);
    return 0;
}

// Print localized usage message to stderr.
static void PrintUsage()
{
    wslutil::PrintMessage(Localization::MessageWsladiagUsage(), stderr);
}

int wsladiag_main(std::wstring_view commandLine)
{
    // Initialize runtime and COM.
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

    // Parse the top-level verb (list, shell, --help).
    ArgumentParser parser(std::wstring{commandLine}, L"wsladiag", 1, true);

    bool help = false;
    std::wstring verb;

    parser.AddPositionalArgument(verb, 0);
    parser.AddArgument(help, L"--help", L'h');

    parser.Parse();
  
    if (help || verb.empty())
    {
        PrintUsage();
        return 0;
    }

    if (verb == L"list")
    {
        return RunListCommand(commandLine);
    }

    if (verb == L"shell")
    {
        return RunShellCommand(commandLine);
    }

    if (verb == L"create")
    {
        return RunCreateCommand(commandLine);
    }

    if (verb == L"start")
    {
        return RunStartCommand(commandLine);
    }

    if (verb == L"stop")
    {
        return RunStopCommand(commandLine);
    }

    if (verb == L"delete" || verb == L"rm")
    {
        return RunDeleteCommand(commandLine);
    }


    // Unknown verb - show usage and fail.
    wslutil::PrintMessage(Localization::MessageWslaUnknownCommand(verb.c_str()), stderr);
    PrintUsage();
    return 1;
}

int wmain(int, wchar_t**)
{
    wsl::windows::common::EnableContextualizedErrors(false);

    ExecutionContext context{Context::WslaDiag};
    int exitCode = 1;
    HRESULT result = S_OK;

    try
    {
        exitCode = wsladiag_main(GetCommandLineW());
    }
    catch (...)
    {
        result = wil::ResultFromCaughtException();
    }

    if (FAILED(result))
    {
        if (auto reported = context.ReportedError())
        {
            auto strings = wsl::windows::common::wslutil::ErrorToString(*reported);

            wslutil::PrintMessage(strings.Message.empty() ? strings.Code : strings.Message, stderr);
        }
        else
        {
            // Fallback for errors without context
            wslutil::PrintMessage(wslutil::GetErrorString(result), stderr);
        }
    }

    return exitCode;
}