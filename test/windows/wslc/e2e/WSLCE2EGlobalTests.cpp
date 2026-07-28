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
        auto result = RunWslc(L"--help");
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_IS_FALSE(result.Stdout.value().empty());
    }

    WSLC_TEST_METHOD(WSLCE2E_InvalidCommand_DisplaysErrorMessage)
    {
        auto result = RunWslc(L"INVALID_CMD");
        result.Verify({.Stdout = L"", .ExitCode = 1});
        VERIFY_IS_TRUE(result.StderrContainsSubstring(L"Unrecognized command: 'INVALID_CMD'"));
    }

    WSLC_TEST_METHOD(WSLCE2E_Help_RoutesToStdout)
    {
        auto result = RunWslc(L"--help");
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_IS_TRUE(result.StdoutContainsSubstring(L"Usage: wslc"));
    }

    WSLC_TEST_METHOD(WSLCE2E_Help_ErrorRoutesToStderr)
    {
        // Help on error must land on stderr; stdout must remain empty.
        auto result = RunWslc(L"INVALID_CMD");
        VERIFY_ARE_NOT_EQUAL(0u, result.ExitCode.value_or(0));
        VERIFY_IS_TRUE(result.Stdout.has_value() && result.Stdout->empty());
        VERIFY_IS_TRUE(result.StderrContainsSubstring(L"Unrecognized command: 'INVALID_CMD'"));
        VERIFY_IS_TRUE(result.StderrContainsSubstring(L"Usage: wslc"));
    }

    WSLC_TEST_METHOD(WSLCE2E_Help_NoColorWhenRedirected)
    {
        // Captured via anonymous pipe; Reporter must suppress VT escape sequences.
        auto result = RunWslc(L"--help");
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_ARE_EQUAL(std::wstring::npos, result.Stdout.value().find(L'\x1b'));
    }

    WSLC_TEST_METHOD(WSLCE2E_Help_ColorOnTerminal)
    {
        // Pseudo console reports VT support; Reporter should emit SGR sequences.
        auto session = RunWslcInteractive(L"--help", ElevationType::Elevated, PseudoConsole{120, 30});
        session.WaitForExit();
        VERIFY_IS_TRUE(session.GetStdoutData().find('\x1b') != std::string::npos);
    }

    WSLC_TEST_METHOD(WSLCE2E_VersionCommand)
    {
        RunWslcAndVerify(L"version", {.Stdout = GetVersionMessage(), .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_VersionFlag)
    {
        RunWslcAndVerify(L"--version", {.Stdout = GetVersionMessage(), .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_VersionCommand_FormatJson)
    {
        auto result = RunWslc(L"version --format json");
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_IS_TRUE(result.Stdout.has_value());
        const auto root = nlohmann::json::parse(wsl::shared::string::WideToMultiByte(result.Stdout.value()));
        VERIFY_ARE_EQUAL(std::string{WSL_PACKAGE_VERSION}, root["Client"]["Version"].get<std::string>());
    }

    WSLC_TEST_METHOD(WSLCE2E_VersionCommand_FormatTable)
    {
        // Explicit table format matches the default plain-text output.
        RunWslcAndVerify(L"version --format table", {.Stdout = GetVersionMessage(), .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Session_DefaultElevated)
    {
        // Run container list to create the default elevated session
        auto result = RunWslc(L"container list", ElevationType::Elevated);
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // Verify session list shows the admin session name
        result = RunWslc(L"system session list", ElevationType::Elevated);
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
        result = RunWslc(L"system session list", ElevationType::NonElevated);
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
        result = RunWslc(std::format(L"--session \"{}\" container list", adminName), ElevationType::NonElevated);

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
        result = RunWslc(std::format(L"--session \"{}\" container list", nonAdminName), ElevationType::Elevated);

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
        auto result = RunWslc(std::format(L"--session \"{}\" container list", nonAdminName), ElevationType::Elevated);
        result.Verify({.Stderr = std::format(L"Session not found: '{}'\r\nError code: WSLC_E_SESSION_NOT_FOUND\r\n", nonAdminName), .ExitCode = 1});

        // Ensure non-elevated cannot create the elevated session.
        result = RunWslc(std::format(L"--session \"{}\" container list", adminName), ElevationType::NonElevated);
        result.Verify({.Stderr = std::format(L"Session not found: '{}'\r\nError code: WSLC_E_SESSION_NOT_FOUND\r\n", adminName), .ExitCode = 1});
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
            HRESULT hr = sessionManager->CreateSession(&settings, WSLCSessionFlagsNone, nullptr, &session);
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
            HRESULT hr = sessionManager->CreateSession(&settings, WSLCSessionFlagsNone, nullptr, &session);
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
            VERIFY_ARE_EQUAL(sessionManager->CreateSession(&settings, WSLCSessionFlagsNone, nullptr, &session), WSLC_E_SESSION_RESERVED);

            settings.DisplayName = L"Wslc-Cli-Admin";
            VERIFY_ARE_EQUAL(sessionManager->CreateSession(&settings, WSLCSessionFlagsNone, nullptr, &session), WSLC_E_SESSION_RESERVED);
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
        result = RunWslc(L"system session list");
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_IS_TRUE(result.Stdout.has_value());
        VERIFY_IS_TRUE(result.Stdout->find(adminName) != std::wstring::npos);

        // Terminate the session
        result = RunWslc(L"system session terminate");
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // Verify session no longer shows up
        result = RunWslc(L"system session list");
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_IS_TRUE(result.Stdout.has_value());
        VERIFY_IS_FALSE(result.Stdout->find(adminName) != std::wstring::npos);

        // Repeat test for non-elevated session.

        // Run container list to create the default session if it does not already exist
        result = RunWslc(L"container list", ElevationType::NonElevated);
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // Verify session list shows the non-elevated session name
        result = RunWslc(L"system session list");
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_IS_TRUE(result.Stdout.has_value());
        VERIFY_IS_TRUE(result.Stdout->find(nonAdminName + L"\r\n") != std::wstring::npos);

        // Terminate the session
        result = RunWslc(L"system session terminate", ElevationType::NonElevated);
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // Verify session no longer shows up
        result = RunWslc(L"system session list");
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
        result = RunWslc(L"system session list");
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_IS_TRUE(result.Stdout.has_value());
        VERIFY_IS_TRUE(result.Stdout->find(adminName) != std::wstring::npos);

        // Terminate the session
        result = RunWslc(std::format(L"--session \"{}\" system session terminate", adminName));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // Verify session no longer shows up
        result = RunWslc(L"system session list");
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_IS_TRUE(result.Stdout.has_value());
        VERIFY_IS_FALSE(result.Stdout->find(adminName) != std::wstring::npos);

        // Repeat test for non-elevated session.

        // Run container list to create the default session if it does not already exist
        result = RunWslc(L"container list", ElevationType::NonElevated);
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // Verify session list shows the non-elevated session name
        result = RunWslc(L"system session list");
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_IS_TRUE(result.Stdout.has_value());
        VERIFY_IS_TRUE(result.Stdout->find(nonAdminName + L"\r\n") != std::wstring::npos);

        // Terminate the session
        result = RunWslc(std::format(L"--session \"{}\" system session terminate", nonAdminName), ElevationType::NonElevated);
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // Verify session no longer shows up
        result = RunWslc(L"system session list");
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
        result = RunWslc(L"system session list");
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_IS_TRUE(result.Stdout.has_value());
        VERIFY_IS_TRUE(result.Stdout->find(adminName) != std::wstring::npos);
        VERIFY_IS_TRUE(result.Stdout->find(nonAdminName + L"\r\n") != std::wstring::npos);

        // Attempt to terminate the admin session from the non-elevated process and fail.
        result = RunWslc(std::format(L"--session \"{}\" system session terminate", adminName), ElevationType::NonElevated);
        result.Verify({.Stderr = L"The requested operation requires elevation. \r\nError code: ERROR_ELEVATION_REQUIRED\r\n", .ExitCode = 1});

        // Terminate the non-elevated session from the elevated process.
        result = RunWslc(std::format(L"--session \"{}\" system session terminate", nonAdminName), ElevationType::Elevated);
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // Verify non-elevated session no longer shows up
        result = RunWslc(L"system session list");
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
        auto result = RunWslc(L"--session INVALID_SESSION_NAME container list");
        result.Verify(
            {.Stdout = L"", .Stderr = L"Session not found: 'INVALID_SESSION_NAME'\r\nError code: WSLC_E_SESSION_NOT_FOUND\r\n", .ExitCode = 1});

        // Verify session list
        result = RunWslc(L"system session list");
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // Verify there is a session with the name of the test session in the session list output.
        VERIFY_IS_TRUE(result.Stdout.has_value());
        auto findResult = result.Stdout->find(session.Name());
        VERIFY_ARE_NOT_EQUAL(findResult, std::wstring::npos);

        // Run container list in the test session, which should succeed if the session is valid.
        result = RunWslc(std::format(L"--session \"{}\" container list", session.Name()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // Add a container to the new session.
        result = RunWslc(std::format(
            L"--session \"{}\" container create --name {} {}", session.Name(), L"test-cont", DebianTestImage().NameAndTag()));
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
            auto session = RunWslcInteractive(L"system session shell");
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
            auto session = RunWslcInteractive(std::format(L"--session \"{}\" system session shell", nonAdminName), ElevationType::NonElevated);
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
            auto session = RunWslcInteractive(std::format(L"--session \"{}\" system session shell", adminName), ElevationType::Elevated);
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

    WSLC_TEST_METHOD(WSLCE2E_Session_Run)
    {
        {
            auto result = RunWslc(L"system session run echo OK");
            result.Verify({.Stdout = L"OK\n", .Stderr = L"", .ExitCode = 0});
        }

        {
            auto result = RunWslc(std::format(L"--session \"{}\" system session run echo OK", GetExpectedDefaultSessionName(true)));
            result.Verify({.Stdout = L"OK\n", .Stderr = L"", .ExitCode = 0});
        }

        {
            auto result = RunWslc(L"--session not-found system session run echo OK");
            result.Verify({.Stderr = L"Session not found: 'not-found'\r\nError code: WSLC_E_SESSION_NOT_FOUND\r\n", .ExitCode = 1});
        }

        {
            auto result = RunWslc(L"system session run not-found");
            result.Verify({.Stdout = L"", .Stderr = L"Failed to launch command not-found. Errno = 2\r\nError code: E_FAIL\r\n", .ExitCode = 1});
        }
    }

    WSLC_TEST_METHOD(WSLCE2E_Session_List_Verbose)
    {
        auto result = RunWslc(L"container list");
        result.Verify({.Stderr = L"", .ExitCode = 0});

        auto verboseResult = RunWslc(L"system session list --verbose");
        verboseResult.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_IS_TRUE(verboseResult.Stdout.has_value());
        VERIFY_IS_TRUE(
            verboseResult.Stdout->find(L"[wslc] Found ") != std::wstring::npos, L"--verbose should print a session summary line");

        auto plainResult = RunWslc(L"system session list");
        plainResult.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_IS_TRUE(plainResult.Stdout.has_value());
        VERIFY_IS_TRUE(
            plainResult.Stdout->find(L"[wslc] Found ") == std::wstring::npos,
            L"plain list should not print a session summary line");
    }

private:
    std::wstring GetVersionMessage() const
    {
        return std::format(L"wslc {}\r\n", WSL_PACKAGE_VERSION);
    }
};
} // namespace WSLCE2ETests
