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
    WSLA_TEST_CLASS(WSLCE2EGlobalTests)

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

    TEST_METHOD(WSLCE2E_Session_Targeting)
    {
        WSL2_TEST_ONLY();

        // Create a test session with VirtioProxy mode so it can pull images and create containers.
        auto session = TestSession::Create(L"wslc-test-session", WSLANetworkingModeVirtioProxy);

        // Verify targeting a non-existent session fails.
        auto result = RunWslc(L"container list --session INVALID_SESSION_NAME");
        result.Verify({.Stdout = L"", .Stderr = L"Element not found. \r\nError code: ERROR_NOT_FOUND\r\n", .ExitCode = 1});

        // Verify session list
        result = RunWslc(L"session list");
        result.Verify({.Stderr = L"", .ExitCode = S_OK});

        // Verify there is a session with the name of the test session in the session list output.
        VERIFY_IS_TRUE(result.Stdout.has_value());
        VERIFY_ARE_NOT_EQUAL(
            result.Stdout->find(L"wslc-test-session"),
            std::wstring::npos,
            L"Session name 'wslc-test-session' not found in session list output");

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
                 << L"  container  Container command.\r\n"
                 << L"  image      Image command.\r\n"
                 << L"  session    Session command.\r\n"
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
                 << L"  run        Run a container.\r\n"
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