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

using namespace WEX::Logging;

namespace WSLCE2ETests {

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

    TEST_METHOD(WSLCE2E_HelpCommand)
    {
        WSL2_TEST_ONLY();
        RunWslcAndVerify(L"--help", {.Stdout = GetHelpMessage(), .Stderr = L"", .ExitCode = 0});
    }

    TEST_METHOD(WSLCE2E_InvalidCommand_DisplaysErrorMessage)
    {
        WSL2_TEST_ONLY();
        RunWslcAndVerify(L"INVALID_CMD", {.Stdout = GetHelpMessage(), .Stderr = L"Unrecognized command: 'INVALID_CMD'\r\n", .ExitCode = 1});
    }

    TEST_METHOD(WSLCE2E_Session_DefaultElevated)
    {
        WSL2_TEST_ONLY();

        // Run container list to create the default elevated session
        auto result = RunWslc(L"container list", ElevationType::Elevated);
        result.Verify({.Stderr = L"", .ExitCode = S_OK});

        // Verify session list shows the admin session name
        result = RunWslc(L"session list", ElevationType::Elevated);
        result.Verify({.Stderr = L"", .ExitCode = S_OK});

        VERIFY_IS_TRUE(result.Stdout.has_value());
        VERIFY_IS_TRUE(result.Stdout->find(L"wslc-cli-admin") != std::wstring::npos);
    }

    TEST_METHOD(WSLCE2E_Session_DefaultNonElevated)
    {
        WSL2_TEST_ONLY();

        // Run container list non-elevated to create the default non-elevated session
        auto result = RunWslc(L"container list", ElevationType::NonElevated);
        result.Verify({.Stderr = L"", .ExitCode = S_OK});

        // Verify session list shows the non-admin session name
        result = RunWslc(L"session list", ElevationType::NonElevated);
        result.Verify({.Stderr = L"", .ExitCode = S_OK});

        VERIFY_IS_TRUE(result.Stdout.has_value());

        // The "\r\n" after session name is important to differentiate it from the admin session.
        VERIFY_IS_TRUE(result.Stdout->find(L"wslc-cli\r\n") != std::wstring::npos);
    }

    TEST_METHOD(WSLCE2E_Session_NonElevatedCannotAccessAdminSession)
    {
        WSL2_TEST_ONLY();

        // First ensure admin session is created by running container list.
        auto result = RunWslc(L"container list", ElevationType::Elevated);
        result.Verify({.Stderr = L"", .ExitCode = S_OK});

        // Try to explicitly target the admin session from non-elevated process
        result = RunWslc(L"container list --session wslc-cli-admin", ElevationType::NonElevated);

        // Should fail with access denied.
        result.Verify({.Stderr = L"The requested operation requires elevation. \r\nError code: ERROR_ELEVATION_REQUIRED\r\n", .ExitCode = 1});
    }

    TEST_METHOD(WSLCE2E_Session_ElevatedCanAccessNonElevatedSession)
    {
        WSL2_TEST_ONLY();

        // First ensure non-elevated session is created by running container list.
        auto result = RunWslc(L"container list", ElevationType::NonElevated);
        result.Verify({.Stderr = L"", .ExitCode = S_OK});

        // Elevated user should be able to explicitly target the non-admin session
        result = RunWslc(L"container list --session wslc-cli", ElevationType::Elevated);

        // This should work - elevated users can access non-elevated sessions
        result.Verify({.Stderr = L"", .ExitCode = S_OK});
    }

    TEST_METHOD(WSLCE2E_Session_CreateMixedElevation_Fails)
    {
        WSL2_TEST_ONLY();

        EnsureSessionIsTerminated(L"wslc-cli");
        EnsureSessionIsTerminated(L"wslc-cli-admin");

        // Ensure elevated cannot create the non-elevated session.
        auto result = RunWslc(L"container list --session wslc-cli", ElevationType::Elevated);
        result.Verify({.Stderr = L"Element not found. \r\nError code: ERROR_NOT_FOUND\r\n", .ExitCode = 1});

        // Ensure non-elevated cannot create the elevated session.
        result = RunWslc(L"container list --session wslc-cli-admin", ElevationType::NonElevated);
        result.Verify({.Stderr = L"Element not found. \r\nError code: ERROR_NOT_FOUND\r\n", .ExitCode = 1});
    }

    TEST_METHOD(WSLCE2E_Session_Terminate_Implicit)
    {
        WSL2_TEST_ONLY();

        // Run container list to create the default session if it does not already exist
        auto result = RunWslc(L"container list");
        result.Verify({.Stderr = L"", .ExitCode = S_OK});

        // Verify session list shows the admin session name
        result = RunWslc(L"session list");
        result.Verify({.Stderr = L"", .ExitCode = S_OK});
        VERIFY_IS_TRUE(result.Stdout.has_value());
        VERIFY_IS_TRUE(result.Stdout->find(L"wslc-cli-admin") != std::wstring::npos);

        // Terminate the session
        result = RunWslc(L"session terminate");
        result.Verify({.Stderr = L"", .ExitCode = S_OK});

        // Verify session no longer shows up
        result = RunWslc(L"session list");
        result.Verify({.Stderr = L"", .ExitCode = S_OK});
        VERIFY_IS_TRUE(result.Stdout.has_value());
        VERIFY_IS_FALSE(result.Stdout->find(L"wslc-cli-admin") != std::wstring::npos);

        // Repeat test for non-elevated session.

        // Run container list to create the default session if it does not already exist
        result = RunWslc(L"container list", ElevationType::NonElevated);
        result.Verify({.Stderr = L"", .ExitCode = S_OK});

        // Verify session list shows the non-elevated session name
        result = RunWslc(L"session list");
        result.Verify({.Stderr = L"", .ExitCode = S_OK});
        VERIFY_IS_TRUE(result.Stdout.has_value());
        VERIFY_IS_TRUE(result.Stdout->find(L"wslc-cli\r\n") != std::wstring::npos);

        // Terminate the session
        result = RunWslc(L"session terminate", ElevationType::NonElevated);
        result.Verify({.Stderr = L"", .ExitCode = S_OK});

        // Verify session no longer shows up
        result = RunWslc(L"session list");
        result.Verify({.Stderr = L"", .ExitCode = S_OK});
        VERIFY_IS_TRUE(result.Stdout.has_value());
        VERIFY_IS_FALSE(result.Stdout->find(L"wslc-cli\r\n") != std::wstring::npos);
    }

    TEST_METHOD(WSLCE2E_Session_Terminate_Explicit)
    {
        WSL2_TEST_ONLY();

        // Run container list to create the default session if it does not already exist
        auto result = RunWslc(L"container list");
        result.Verify({.Stderr = L"", .ExitCode = S_OK});

        // Verify session list shows the admin session name
        result = RunWslc(L"session list");
        result.Verify({.Stderr = L"", .ExitCode = S_OK});
        VERIFY_IS_TRUE(result.Stdout.has_value());
        VERIFY_IS_TRUE(result.Stdout->find(L"wslc-cli-admin") != std::wstring::npos);

        // Terminate the session
        result = RunWslc(L"session terminate wslc-cli-admin");
        result.Verify({.Stderr = L"", .ExitCode = S_OK});

        // Verify session no longer shows up
        result = RunWslc(L"session list");
        result.Verify({.Stderr = L"", .ExitCode = S_OK});
        VERIFY_IS_TRUE(result.Stdout.has_value());
        VERIFY_IS_FALSE(result.Stdout->find(L"wslc-cli-admin") != std::wstring::npos);

        // Repeat test for non-elevated session.

        // Run container list to create the default session if it does not already exist
        result = RunWslc(L"container list", ElevationType::NonElevated);
        result.Verify({.Stderr = L"", .ExitCode = S_OK});

        // Verify session list shows the non-elevated session name
        result = RunWslc(L"session list");
        result.Verify({.Stderr = L"", .ExitCode = S_OK});
        VERIFY_IS_TRUE(result.Stdout.has_value());
        VERIFY_IS_TRUE(result.Stdout->find(L"wslc-cli\r\n") != std::wstring::npos);

        // Terminate the session
        result = RunWslc(L"session terminate wslc-cli", ElevationType::NonElevated);
        result.Verify({.Stderr = L"", .ExitCode = S_OK});

        // Verify session no longer shows up
        result = RunWslc(L"session list");
        result.Verify({.Stderr = L"", .ExitCode = S_OK});
        VERIFY_IS_TRUE(result.Stdout.has_value());
        VERIFY_IS_FALSE(result.Stdout->find(L"wslc-cli\r\n") != std::wstring::npos);
    }

    TEST_METHOD(WSLCE2E_Session_Terminate_MixedElevation)
    {
        WSL2_TEST_ONLY();

        // Run container list to create the default sessions if they do not already exist.
        auto result = RunWslc(L"container list", ElevationType::Elevated);
        result.Verify({.Stderr = L"", .ExitCode = S_OK});
        result = RunWslc(L"container list", ElevationType::NonElevated);
        result.Verify({.Stderr = L"", .ExitCode = S_OK});

        // Verify session list shows both sessions.
        result = RunWslc(L"session list");
        result.Verify({.Stderr = L"", .ExitCode = S_OK});
        VERIFY_IS_TRUE(result.Stdout.has_value());
        VERIFY_IS_TRUE(result.Stdout->find(L"wslc-cli-admin") != std::wstring::npos);
        VERIFY_IS_TRUE(result.Stdout->find(L"wslc-cli\r\n") != std::wstring::npos);

        // Attempt to terminate the admin session from the non-elevated process and fail.
        result = RunWslc(L"session terminate wslc-cli-admin", ElevationType::NonElevated);
        result.Verify({.Stderr = L"The requested operation requires elevation. \r\nError code: ERROR_ELEVATION_REQUIRED\r\n", .ExitCode = 1});

        // Terminate the non-elevated session from the elevated process.
        result = RunWslc(L"session terminate wslc-cli", ElevationType::Elevated);
        result.Verify({.Stderr = L"", .ExitCode = S_OK});

        // Verify non-elevated session no longer shows up
        result = RunWslc(L"session list");
        result.Verify({.Stderr = L"", .ExitCode = S_OK});
        VERIFY_IS_TRUE(result.Stdout.has_value());
        VERIFY_IS_FALSE(result.Stdout->find(L"wslc-cli\r\n") != std::wstring::npos);
    }

    TEST_METHOD(WSLCE2E_Session_Targeting)
    {
        WSL2_TEST_ONLY();

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
        result.Verify({.Stderr = L"", .ExitCode = S_OK});

        // Verify there is a session with the name of the test session in the session list output.
        VERIFY_IS_TRUE(result.Stdout.has_value());
        auto findResult = result.Stdout->find(session.Name());
        VERIFY_ARE_NOT_EQUAL(findResult, std::wstring::npos);

        // Run container list in the test session, which should succeed if the session is valid.
        result = RunWslc(std::format(L"container list --session {}", session.Name()));
        result.Verify({.Stderr = L"", .ExitCode = S_OK});

        // Add a container to the new session.
        result = RunWslc(
            std::format(L"container create --session {} --name {} {}", session.Name(), L"test-cont", DebianTestImage().NameAndTag()));
        result.Dump(); // Dump so it is easier to find any potential issues with the pull in the test output.
        result.Verify({.ExitCode = S_OK});

        // Verify container exists in the custom session
        VerifyContainerIsListed(L"test-cont", L"created", session.Name());

        // Verify container does not exist in the default CLI session.
        VerifyContainerIsNotListed(L"test-cont");
    }

    TEST_METHOD(WSLCE2E_Session_Shell)
    {
        WSL2_TEST_ONLY();

        // Ensure sessions are created by running container list elevated and non-elevated.
        auto result = RunWslc(L"container list", ElevationType::NonElevated);
        result.Verify({.Stderr = L"", .ExitCode = S_OK});
        result = RunWslc(L"container list", ElevationType::Elevated);
        result.Verify({.Stderr = L"", .ExitCode = S_OK});

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
            auto session = RunWslcInteractive(L"session shell wslc-cli", ElevationType::NonElevated);
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
            auto session = RunWslcInteractive(L"session shell wslc-cli-admin", ElevationType::Elevated);
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
        std::wstringstream commands;
        commands << L"The following commands are available:\r\n"
                 << L"  container  Manage containers.\r\n"
                 << L"  image      Manage images.\r\n"
                 << L"  session    Manage sessions.\r\n"
                 << L"  settings   Open the settings file in the default editor.\r\n"
                 << L"  attach     Attach to a container.\r\n"
                 << L"  build      Build an image from a Dockerfile.\r\n"
                 << L"  create     Create a container.\r\n"
                 << L"  exec       Execute a command in a running container.\r\n"
                 << L"  images     List images.\r\n"
                 << L"  inspect    Inspect a container.\r\n"
                 << L"  kill       Kill containers.\r\n"
                 << L"  list       List containers.\r\n"
                 << L"  load       Load images.\r\n"
                 << L"  logs       View container logs.\r\n"
                 << L"  pull       Pull images.\r\n"
                 << L"  remove     Remove containers.\r\n"
                 << L"  rmi        Remove images.\r\n"
                 << L"  run        Run a container.\r\n"
                 << L"  save       Save images.\r\n"
                 << L"  start      Start a container.\r\n"
                 << L"  stop       Stop containers.\r\n"
                 << L"\r\n"
                 << L"For more details on a specific command, pass it the help argument. [-h]\r\n\r\n";
        return commands.str();
    }

    std::wstring GetAvailableOptions() const
    {
        std::wstringstream options;
        options << L"The following options are available:\r\n"
                << L"  -v,--version  Show version information for this tool\r\n"
                << L"  -h,--help     Shows help about the selected command\r\n"
                << L"\r\n";
        return options.str();
    }
};
} // namespace WSLCE2ETests