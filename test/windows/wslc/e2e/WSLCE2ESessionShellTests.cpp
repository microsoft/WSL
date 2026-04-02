/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCE2ESessionShellTests.cpp

Abstract:

    This file contains end-to-end tests for WSLC session shell command.
--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCExecutor.h"
#include "WSLCE2EHelpers.h"

namespace WSLCE2ETests {

using namespace WEX::Logging;

class WSLCE2ESessionShellTests
{
    WSLC_TEST_CLASS(WSLCE2ESessionShellTests)

    TEST_METHOD(WSLCE2E_Session_Shell_HelpCommand)
    {
        WSL2_TEST_ONLY();
        auto result = RunWslc(L"session shell --help");
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"", .ExitCode = 0});
    }

    TEST_METHOD(WSLCE2E_Session_Shell_Interactive)
    {
        WSL2_TEST_ONLY();

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
        return L"Attaches to an active session. If no session ID is provided, the wslc default session will be used.\r\n\r\n";
    }

    std::wstring GetUsage() const
    {
        return L"Usage: wslc session shell [<options>] [<session-id>]\r\n\r\n";
    }

    std::wstring GetAvailableCommands() const
    {
        std::wstringstream commands;
        commands << L"The following arguments are available:\r\n"
                 << L"  session-id    Session ID\r\n"
                 << L"\r\n";
        return commands.str();
    }

    std::wstring GetAvailableOptions() const
    {
        std::wstringstream options;
        options << L"The following options are available:\r\n"
                << L"  -h,--help     Shows help about the selected command\r\n"
                << L"\r\n";
        return options.str();
    }
};
} // namespace WSLCE2ETests
