/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCE2EGlobalTests.cpp

Abstract:

    This file contains end-to-end tests for WSLC.
--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCCLITestHelpers.h"
#include "WSLCExecutor.h"
#include "WSLCE2EHelpers.h"
#include "WSLCSessionDefaults.h"
#include "Argument.h"

using namespace WEX::Logging;

namespace WSLCE2ETests {
using namespace wsl::shared;

namespace {

    // Returns the expected default session name for the current user (e.g. "wslc-cli-admin-benhill").
    std::wstring GetExpectedDefaultSessionName(bool elevated)
    {
        auto baseName = elevated ? wsl::windows::wslc::DefaultAdminSessionName : wsl::windows::wslc::DefaultSessionName;

        wchar_t username[256 + 1] = {};
        DWORD usernameLen = ARRAYSIZE(username);
        THROW_IF_WIN32_BOOL_FALSE(GetUserNameW(username, &usernameLen));

        return std::format(L"{}-{}", baseName, username);
    }

} // namespace

class WSLCE2EGlobalTests
{
    WSLC_TEST_CLASS(WSLCE2EGlobalTests)

    wil::unique_couninitialize_call m_coinit = wil::CoInitializeEx();

    TEST_CLASS_SETUP(TestClassSetup)
    {
        return true;
    }

    TEST_CLASS_CLEANUP(TestClassCleanup)
    {
        return true;
    }

    WSLC_TEST_METHOD(WSLCE2E_HelpCommand)
    {
        RunWslcAndVerify(L"--help", {.Stdout = GetHelpMessage(), .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_InvalidCommand_DisplaysErrorMessage)
    {
        RunWslcAndVerify(L"INVALID_CMD", {.Stdout = GetHelpMessage(), .Stderr = L"Unrecognized command: 'INVALID_CMD'\r\n", .ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_VersionCommand)
    {
        RunWslcAndVerify(L"version", {.Stdout = GetVersionMessage(), .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_VersionFlag)
    {
        RunWslcAndVerify(L"--version", {.Stdout = GetVersionMessage(), .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Session_DefaultElevated)
    {
        // Run container list to create the default elevated session
        auto result = RunWslc(L"container list", ElevationType::Elevated);
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // Verify session list shows the admin session name
        result = RunWslc(L"session list", ElevationType::Elevated);
        result.Verify({.Stderr = L"", .ExitCode = 0});

        VERIFY_IS_TRUE(result.Stdout.has_value());
        auto adminName = GetExpectedDefaultSessionName(true);
        VERIFY_IS_TRUE(result.Stdout->find(adminName) != std::wstring::npos);
    }

    WSLC_TEST_METHOD(WSLCE2E_Session_DefaultNonElevated)
    {
        // Run container list non-elevated to create the default non-elevated session
        auto result = RunWslc(L"container list", ElevationType::NonElevated);
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // Verify session list shows the non-admin session name
        result = RunWslc(L"session list", ElevationType::NonElevated);
        result.Verify({.Stderr = L"", .ExitCode = 0});

        VERIFY_IS_TRUE(result.Stdout.has_value());

        // The "\r\n" after session name is important to differentiate it from the admin session.
        auto nonAdminName = GetExpectedDefaultSessionName(false);
        VERIFY_IS_TRUE(result.Stdout->find(nonAdminName + L"\r\n") != std::wstring::npos);
    }

    WSLC_TEST_METHOD(WSLCE2E_Session_NonElevatedCannotAccessAdminSession)
    {
        // First ensure admin session is created by running container list.
        auto result = RunWslc(L"container list", ElevationType::Elevated);
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // Try to explicitly target the admin session from non-elevated process
        auto adminName = GetExpectedDefaultSessionName(true);
        result = RunWslc(std::format(L"container list --session {}", adminName), ElevationType::NonElevated);

        // Should fail with access denied.
        result.Verify({.Stderr = L"The requested operation requires elevation. \r\nError code: ERROR_ELEVATION_REQUIRED\r\n", .ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_Session_ElevatedCanAccessNonElevatedSession)
    {
        // First ensure non-elevated session is created by running container list.
        auto result = RunWslc(L"container list", ElevationType::NonElevated);
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // Elevated user should be able to explicitly target the non-admin session
        auto nonAdminName = GetExpectedDefaultSessionName(false);
        result = RunWslc(std::format(L"container list --session {}", nonAdminName), ElevationType::Elevated);

        // This should work - elevated users can access non-elevated sessions
        result.Verify({.Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Session_CreateMixedElevation_Fails)
    {
        EnsureSessionIsTerminated(GetExpectedDefaultSessionName(false));
        EnsureSessionIsTerminated(GetExpectedDefaultSessionName(true));

        // Ensure elevated cannot create the non-elevated session.
        auto nonAdminName = GetExpectedDefaultSessionName(false);
        auto adminName = GetExpectedDefaultSessionName(true);
        auto result = RunWslc(std::format(L"container list --session {}", nonAdminName), ElevationType::Elevated);
        result.Verify({.Stderr = L"Element not found. \r\nError code: ERROR_NOT_FOUND\r\n", .ExitCode = 1});

        // Ensure non-elevated cannot create the elevated session.
        result = RunWslc(std::format(L"container list --session {}", adminName), ElevationType::NonElevated);
        result.Verify({.Stderr = L"Element not found. \r\nError code: ERROR_NOT_FOUND\r\n", .ExitCode = 1});
    }

    // Regression test for session name squatting vulnerability.
    //
    // Validates that a process cannot create a session with the reserved default
    // session names ("wslc-cli" or "wslc-cli-admin") via the COM API. These names
    // are assigned server-side when the client passes null Settings to CreateSession,
    // preventing a malicious process from squatting on the name and blocking
    // legitimate wslc.exe clients.
    WSLC_TEST_METHOD(WSLCE2E_Session_NameSquatting_ElevatedCannotBlockNonElevated)
    {
        // Ensure no existing sessions with default names.
        EnsureSessionIsTerminated(wsl::windows::wslc::DefaultSessionName);
        EnsureSessionIsTerminated(wsl::windows::wslc::DefaultAdminSessionName);

        // Attack: attempt to create a session with the reserved non-admin default
        // name directly through the COM API from this elevated process.
        // The service should reject this because reserved default session names
        // cannot be explicitly created.
        {
            wil::com_ptr<IWSLCSessionManager> sessionManager;
            VERIFY_SUCCEEDED(CoCreateInstance(__uuidof(WSLCSessionManager), nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&sessionManager)));
            wsl::windows::common::security::ConfigureForCOMImpersonation(sessionManager.get());

            WSLCSessionSettings settings{};
            settings.DisplayName = wsl::windows::wslc::DefaultSessionName;
            settings.StoragePath = L"C:\\dummy";
            settings.CpuCount = 4;
            settings.MemoryMb = 2048;
            settings.BootTimeoutMs = 30000;
            settings.MaximumStorageSizeMb = 4096;

            wil::com_ptr<IWSLCSession> session;
            HRESULT hr = sessionManager->CreateSession(&settings, WSLCSessionFlagsNone, &session);
            VERIFY_ARE_EQUAL(hr, WSLC_E_SESSION_RESERVED);
        }

        // Also verify that the admin reserved name is rejected.
        {
            wil::com_ptr<IWSLCSessionManager> sessionManager;
            VERIFY_SUCCEEDED(CoCreateInstance(__uuidof(WSLCSessionManager), nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&sessionManager)));
            wsl::windows::common::security::ConfigureForCOMImpersonation(sessionManager.get());

            WSLCSessionSettings settings{};
            settings.DisplayName = wsl::windows::wslc::DefaultAdminSessionName;
            settings.StoragePath = L"C:\\dummy";
            settings.CpuCount = 4;
            settings.MemoryMb = 2048;
            settings.BootTimeoutMs = 30000;
            settings.MaximumStorageSizeMb = 4096;

            wil::com_ptr<IWSLCSession> session;
            HRESULT hr = sessionManager->CreateSession(&settings, WSLCSessionFlagsNone, &session);
            VERIFY_ARE_EQUAL(hr, WSLC_E_SESSION_RESERVED);
        }

        // Non-elevated wslc.exe should still be able to create and use its default
        // session (which now passes null Settings, resolved entirely server-side).
        auto result = RunWslc(L"container list", ElevationType::NonElevated);
        result.Verify({.Stderr = L"", .ExitCode = S_OK});

        // Verify that case variations of reserved names are also rejected,
        // preventing bypass on case-insensitive filesystems (NTFS).
        {
            wil::com_ptr<IWSLCSessionManager> sessionManager;
            VERIFY_SUCCEEDED(CoCreateInstance(__uuidof(WSLCSessionManager), nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&sessionManager)));
            wsl::windows::common::security::ConfigureForCOMImpersonation(sessionManager.get());

            WSLCSessionSettings settings{};
            settings.DisplayName = L"WSLC-CLI";
            settings.StoragePath = L"C:\\dummy";
            settings.CpuCount = 4;
            settings.MemoryMb = 2048;
            settings.BootTimeoutMs = 30000;
            settings.MaximumStorageSizeMb = 4096;

            wil::com_ptr<IWSLCSession> session;
            VERIFY_ARE_EQUAL(sessionManager->CreateSession(&settings, WSLCSessionFlagsNone, &session), WSLC_E_SESSION_RESERVED);

            settings.DisplayName = L"Wslc-Cli-Admin";
            VERIFY_ARE_EQUAL(sessionManager->CreateSession(&settings, WSLCSessionFlagsNone, &session), WSLC_E_SESSION_RESERVED);
        }
    }

    WSLC_TEST_METHOD(WSLCE2E_Session_Terminate_Implicit)
    {
        auto adminName = GetExpectedDefaultSessionName(true);
        auto nonAdminName = GetExpectedDefaultSessionName(false);

        // Run container list to create the default session if it does not already exist
        auto result = RunWslc(L"container list");
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // Verify session list shows the admin session name
        result = RunWslc(L"session list");
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_IS_TRUE(result.Stdout.has_value());
        VERIFY_IS_TRUE(result.Stdout->find(adminName) != std::wstring::npos);

        // Terminate the session
        result = RunWslc(L"session terminate");
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // Verify session no longer shows up
        result = RunWslc(L"session list");
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_IS_TRUE(result.Stdout.has_value());
        VERIFY_IS_FALSE(result.Stdout->find(adminName) != std::wstring::npos);

        // Repeat test for non-elevated session.

        // Run container list to create the default session if it does not already exist
        result = RunWslc(L"container list", ElevationType::NonElevated);
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // Verify session list shows the non-elevated session name
        result = RunWslc(L"session list");
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_IS_TRUE(result.Stdout.has_value());
        VERIFY_IS_TRUE(result.Stdout->find(nonAdminName + L"\r\n") != std::wstring::npos);

        // Terminate the session
        result = RunWslc(L"session terminate", ElevationType::NonElevated);
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // Verify session no longer shows up
        result = RunWslc(L"session list");
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_IS_TRUE(result.Stdout.has_value());
        VERIFY_IS_FALSE(result.Stdout->find(nonAdminName + L"\r\n") != std::wstring::npos);
    }

    WSLC_TEST_METHOD(WSLCE2E_Session_Terminate_Explicit)
    {
        auto adminName = GetExpectedDefaultSessionName(true);
        auto nonAdminName = GetExpectedDefaultSessionName(false);

        // Run container list to create the default session if it does not already exist
        auto result = RunWslc(L"container list");
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // Verify session list shows the admin session name
        result = RunWslc(L"session list");
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_IS_TRUE(result.Stdout.has_value());
        VERIFY_IS_TRUE(result.Stdout->find(adminName) != std::wstring::npos);

        // Terminate the session
        result = RunWslc(std::format(L"session terminate {}", adminName));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // Verify session no longer shows up
        result = RunWslc(L"session list");
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_IS_TRUE(result.Stdout.has_value());
        VERIFY_IS_FALSE(result.Stdout->find(adminName) != std::wstring::npos);

        // Repeat test for non-elevated session.

        // Run container list to create the default session if it does not already exist
        result = RunWslc(L"container list", ElevationType::NonElevated);
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // Verify session list shows the non-elevated session name
        result = RunWslc(L"session list");
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_IS_TRUE(result.Stdout.has_value());
        VERIFY_IS_TRUE(result.Stdout->find(nonAdminName + L"\r\n") != std::wstring::npos);

        // Terminate the session
        result = RunWslc(std::format(L"session terminate {}", nonAdminName), ElevationType::NonElevated);
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // Verify session no longer shows up
        result = RunWslc(L"session list");
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_IS_TRUE(result.Stdout.has_value());
        VERIFY_IS_FALSE(result.Stdout->find(nonAdminName + L"\r\n") != std::wstring::npos);
    }

    WSLC_TEST_METHOD(WSLCE2E_Session_Terminate_MixedElevation)
    {
        auto adminName = GetExpectedDefaultSessionName(true);
        auto nonAdminName = GetExpectedDefaultSessionName(false);

        // Run container list to create the default sessions if they do not already exist.
        auto result = RunWslc(L"container list", ElevationType::Elevated);
        result.Verify({.Stderr = L"", .ExitCode = 0});
        result = RunWslc(L"container list", ElevationType::NonElevated);
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // Verify session list shows both sessions.
        result = RunWslc(L"session list");
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_IS_TRUE(result.Stdout.has_value());
        VERIFY_IS_TRUE(result.Stdout->find(adminName) != std::wstring::npos);
        VERIFY_IS_TRUE(result.Stdout->find(nonAdminName + L"\r\n") != std::wstring::npos);

        // Attempt to terminate the admin session from the non-elevated process and fail.
        result = RunWslc(std::format(L"session terminate {}", adminName), ElevationType::NonElevated);
        result.Verify({.Stderr = L"The requested operation requires elevation. \r\nError code: ERROR_ELEVATION_REQUIRED\r\n", .ExitCode = 1});

        // Terminate the non-elevated session from the elevated process.
        result = RunWslc(std::format(L"session terminate {}", nonAdminName), ElevationType::Elevated);
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // Verify non-elevated session no longer shows up
        result = RunWslc(L"session list");
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_IS_TRUE(result.Stdout.has_value());
        VERIFY_IS_FALSE(result.Stdout->find(nonAdminName + L"\r\n") != std::wstring::npos);
    }

    WSLC_TEST_METHOD(WSLCE2E_Session_Targeting)
    {
        // Generate a unique session name to avoid conflicts with previous runs or concurrent tests.
        // Use only a short portion of the GUID to avoid MAX_PATH issues.
        GUID sessionGuid;
        VERIFY_SUCCEEDED(CoCreateGuid(&sessionGuid));
        auto guidStr = wsl::shared::string::GuidToString<wchar_t>(sessionGuid, wsl::shared::string::GuidToStringFlags::None);
        const auto sessionName = std::format(L"wslc-test-{}", guidStr.substr(0, 8));

        auto session = TestSession::Create(sessionName);

        // Load the Debian image into the test session to avoid hitting Docker Hub rate limits.
        EnsureImageIsLoaded(DebianTestImage(), session.Name());

        // Verify targeting a non-existent session fails.
        auto result = RunWslc(L"container list --session INVALID_SESSION_NAME");
        result.Verify({.Stdout = L"", .Stderr = L"Element not found. \r\nError code: ERROR_NOT_FOUND\r\n", .ExitCode = 1});

        // Verify session list
        result = RunWslc(L"session list");
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // Verify there is a session with the name of the test session in the session list output.
        VERIFY_IS_TRUE(result.Stdout.has_value());
        auto findResult = result.Stdout->find(session.Name());
        VERIFY_ARE_NOT_EQUAL(findResult, std::wstring::npos);

        // Run container list in the test session, which should succeed if the session is valid.
        result = RunWslc(std::format(L"container list --session {}", session.Name()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // Add a container to the new session.
        result = RunWslc(
            std::format(L"container create --session {} --name {} {}", session.Name(), L"test-cont", DebianTestImage().NameAndTag()));
        result.Dump(); // Dump so it is easier to find any potential issues with the pull in the test output.
        result.Verify({.ExitCode = 0});

        // Verify container exists in the custom session
        VerifyContainerIsListed(L"test-cont", L"created", session.Name());

        // Verify container does not exist in the default CLI session.
        VerifyContainerIsNotListed(L"test-cont");
    }

    WSLC_TEST_METHOD(WSLCE2E_Session_Shell)
    {
        // Ensure sessions are created by running container list elevated and non-elevated.
        auto result = RunWslc(L"container list", ElevationType::NonElevated);
        result.Verify({.Stderr = L"", .ExitCode = 0});
        result = RunWslc(L"container list", ElevationType::Elevated);
        result.Verify({.Stderr = L"", .ExitCode = 0});

        {
            Log::Comment(L"Testing elevated interactive session");
            // Session shell should attach to the correct default session.
            // Test should be elevated, therefore this should be the admin session.
            auto session = RunWslcInteractive(L"session shell");
            VERIFY_IS_TRUE(session.IsRunning(), L"Session should be running");

            session.ExpectStdout(VT::SESSION_PROMPT);

            session.WriteLine("echo hello");
            session.ExpectStdout(VT::RESET);
            session.ExpectCommandEcho("echo hello");
            session.ExpectStdout("hello\r\n");
            session.ExpectStdout(VT::SESSION_PROMPT);

            session.WriteLine("whoami");
            session.ExpectStdout(VT::RESET);
            session.ExpectCommandEcho("whoami");
            session.ExpectStdout("root\r\n");
            session.ExpectStdout(VT::SESSION_PROMPT);

            session.ExitAndVerifyNoErrors();
            auto exitCode = session.Wait();
            VERIFY_ARE_EQUAL(0, exitCode);
        }
        {
            Log::Comment(L"Testing non-elevated interactive session with explicit session name");
            // Non-Elevated session shell should attach to the wslc by name also.
            auto nonAdminName = GetExpectedDefaultSessionName(false);
            auto session = RunWslcInteractive(std::format(L"session shell {}", nonAdminName), ElevationType::NonElevated);
            VERIFY_IS_TRUE(session.IsRunning(), L"Session should be running");

            session.ExpectStdout(VT::SESSION_PROMPT);

            session.WriteLine("echo hello");
            session.ExpectStdout(VT::RESET);
            session.ExpectCommandEcho("echo hello");
            session.ExpectStdout("hello\r\n");
            session.ExpectStdout(VT::SESSION_PROMPT);

            session.WriteLine("whoami");
            session.ExpectStdout(VT::RESET);
            session.ExpectCommandEcho("whoami");
            session.ExpectStdout("root\r\n");
            session.ExpectStdout(VT::SESSION_PROMPT);

            session.ExitAndVerifyNoErrors();
            auto exitCode = session.Wait();
            VERIFY_ARE_EQUAL(0, exitCode);
        }
        {
            Log::Comment(L"Testing elevated interactive session with explicit admin session name");
            // Elevated session shell should attach to the wslc by name also.
            auto adminName = GetExpectedDefaultSessionName(true);
            auto session = RunWslcInteractive(std::format(L"session shell {}", adminName), ElevationType::Elevated);
            VERIFY_IS_TRUE(session.IsRunning(), L"Session should be running");

            session.ExpectStdout(VT::SESSION_PROMPT);

            session.WriteLine("echo hello");
            session.ExpectStdout(VT::RESET);
            session.ExpectCommandEcho("echo hello");
            session.ExpectStdout("hello\r\n");
            session.ExpectStdout(VT::SESSION_PROMPT);

            session.WriteLine("whoami");
            session.ExpectStdout(VT::RESET);
            session.ExpectCommandEcho("whoami");
            session.ExpectStdout("root\r\n");
            session.ExpectStdout(VT::SESSION_PROMPT);

            session.ExitAndVerifyNoErrors();
            auto exitCode = session.Wait();
            VERIFY_ARE_EQUAL(0, exitCode);
        }
    }

private:
    std::wstring GetHelpMessage() const
    {
        std::wstringstream output;
        output << GetWslcHeader()        //
               << GetDescription()       //
               << GetUsage()             //
               << GetAvailableCommands() //
               << GetAvailableOptions();
        return output.str();
    }

    std::wstring GetVersionMessage() const
    {
        return std::format(L"wslc {}\r\n", WSL_PACKAGE_VERSION);
    }

    std::wstring GetDescription() const
    {
        return L"WSLC is the Windows Subsystem for Linux Container CLI tool. It enables management and interaction with WSL "
               L"containers from the command line.\r\n\r\n";
    }

    std::wstring GetUsage() const
    {
        return L"Usage: wslc  [<command>] [<options>]\r\n\r\n";
    }

    std::wstring GetAvailableCommands() const
    {
        std::vector<std::pair<std::wstring_view, std::wstring>> entries = {
            {L"container", Localization::WSLCCLI_ContainerCommandDesc()},
            {L"image", Localization::WSLCCLI_ImageCommandDesc()},
            {L"registry", Localization::WSLCCLI_RegistryCommandDesc()},
            {L"session", Localization::WSLCCLI_SessionCommandDesc()},
            {L"settings", Localization::WSLCCLI_SettingsCommandDesc()},
            {L"volume", Localization::WSLCCLI_VolumeCommandDesc()},
            {L"attach", Localization::WSLCCLI_ContainerAttachDesc()},
            {L"build", Localization::WSLCCLI_ImageBuildDesc()},
            {L"create", Localization::WSLCCLI_ContainerCreateDesc()},
            {L"exec", Localization::WSLCCLI_ContainerExecDesc()},
            {L"images", Localization::WSLCCLI_ImageListDesc()},
            {L"inspect", Localization::WSLCCLI_ContainerInspectDesc()},
            {L"kill", Localization::WSLCCLI_ContainerKillDesc()},
            {L"list", Localization::WSLCCLI_ContainerListDesc()},
            {L"load", Localization::WSLCCLI_ImageLoadDesc()},
            {L"login", Localization::WSLCCLI_LoginDesc()},
            {L"logout", Localization::WSLCCLI_LogoutDesc()},
            {L"logs", Localization::WSLCCLI_ContainerLogsDesc()},
            {L"pull", Localization::WSLCCLI_ImagePullDesc()},
            {L"push", Localization::WSLCCLI_ImagePushDesc()},
            {L"remove", Localization::WSLCCLI_ContainerRemoveDesc()},
            {L"rmi", Localization::WSLCCLI_ImageRemoveDesc()},
            {L"run", Localization::WSLCCLI_ContainerRunDesc()},
            {L"save", Localization::WSLCCLI_ImageSaveDesc()},
            {L"start", Localization::WSLCCLI_ContainerStartDesc()},
            {L"stop", Localization::WSLCCLI_ContainerStopDesc()},
            {L"tag", Localization::WSLCCLI_ImageTagDesc()},
            {L"version", Localization::WSLCCLI_VersionDesc()},
        };

        size_t maxLen = 0;
        for (const auto& [name, _] : entries)
        {
            maxLen = (std::max)(maxLen, name.size());
        }

        std::wstringstream commands;
        commands << Localization::WSLCCLI_AvailableCommands() << L"\r\n";
        for (const auto& [name, desc] : entries)
        {
            commands << L"  " << name << std::wstring(maxLen - name.size() + 2, L' ') << desc << L"\r\n";
        }
        commands << L"\r\n" << Localization::WSLCCLI_HelpForDetails() << L" [" << WSLC_CLI_HELP_ARG_STRING << L"]\r\n\r\n";
        return commands.str();
    }

    std::wstring GetAvailableOptions() const
    {
        std::wstringstream options;
        options << L"The following options are available:\r\n"
                << L"  -v,--version  Show version information for this tool\r\n"
                << L"  -?,--help     Shows help about the selected command\r\n"
                << L"\r\n";
        return options.str();
    }
};
} // namespace WSLCE2ETests