/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCE2ESessionTests.cpp

Abstract:

    This file contains end-to-end tests for WSLC session command.
--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCExecutor.h"
#include "WSLCE2EHelpers.h"

namespace WSLCE2ETests {

class WSLCE2ESessionTests
{
    WSLC_TEST_CLASS(WSLCE2ESessionTests)

    wil::unique_couninitialize_call m_coinit = wil::CoInitializeEx();

    TEST_METHOD(WSLCE2E_Session_HelpCommand)
    {
        WSL2_TEST_ONLY();
        auto result = RunWslc(L"session --help");
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"", .ExitCode = 0});
    }

    TEST_METHOD(WSLCE2E_Session_NoSubcommand_ShowsHelp)
    {
        WSL2_TEST_ONLY();
        auto result = RunWslc(L"session");
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"", .ExitCode = 0});
    }

    TEST_METHOD(WSLCE2E_Session_InvalidCommand_DisplaysErrorMessage)
    {
        WSL2_TEST_ONLY();
        auto result = RunWslc(L"session INVALID_CMD");
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"Unrecognized command: 'INVALID_CMD'\r\n", .ExitCode = 1});
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

    TEST_METHOD(WSLCE2E_Session_NonElevatedCannotAccessAdminSession)
    {
        WSL2_TEST_ONLY();

        // First ensure admin session is created by running container list.
        auto result = RunWslc(L"container list", ElevationType::Elevated);
        result.Verify({.Stderr = L"", .ExitCode = 0});

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
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // Elevated user should be able to explicitly target the non-admin session
        result = RunWslc(L"container list --session wslc-cli", ElevationType::Elevated);

        // This should work - elevated users can access non-elevated sessions
        result.Verify({.Stderr = L"", .ExitCode = 0});
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
        return L"Session command.\r\n\r\n";
    }

    std::wstring GetUsage() const
    {
        return L"Usage: wslc session [<command>] [<options>]\r\n\r\n";
    }

    std::wstring GetAvailableCommands() const
    {
        std::wstringstream commands;
        commands << L"The following sub-commands are available:\r\n"
                 << L"  list       List sessions.\r\n"
                 << L"  shell      Attach to a session.\r\n"
                 << L"  terminate  Terminate a session.\r\n"
                 << L"\r\n"
                 << L"For more details on a specific command, pass it the help argument. [-h]\r\n"
                 << L"\r\n";
        return commands.str();
    }

    std::wstring GetAvailableOptions() const
    {
        std::wstringstream options;
        options << L"The following options are available:\r\n"
                << L"  -h,--help  Shows help about the selected command\r\n"
                << L"\r\n";
        return options.str();
    }
};
} // namespace WSLCE2ETests
