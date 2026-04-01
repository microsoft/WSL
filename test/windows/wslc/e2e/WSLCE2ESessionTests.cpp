/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCE2ESessionTests.cpp

Abstract:

    This file contains end-to-end tests for WSLC session list and shell commands.
--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCExecutor.h"
#include "WSLCE2EHelpers.h"

namespace WSLCE2ETests {

class WSLCE2ESessionTests
{
    WSLC_TEST_CLASS(WSLCE2ESessionTests)

    TEST_METHOD(WSLCE2E_Session_List_HelpCommand)
    {
        WSL2_TEST_ONLY();

        auto result = RunWslc(L"session list --help");
        result.Verify({.Stdout = GetSessionListHelpMessage(), .Stderr = L"", .ExitCode = 0});
    }

    TEST_METHOD(WSLCE2E_Session_List_Verbose)
    {
        WSL2_TEST_ONLY();

        // First create a session by running a container
        RunWslc(L"container list", ElevationType::Elevated).Verify({.Stderr = L"", .ExitCode = 0});

        // Now list sessions with verbose output
        auto result = RunWslc(L"session list --verbose", ElevationType::Elevated);
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // Verbose output should have more details
        VERIFY_IS_TRUE(result.Stdout.has_value());
        auto output = result.Stdout.value();
        // With verbose flag, should include more information (actual format depends on implementation)
        VERIFY_IS_FALSE(output.empty());
    }

    TEST_METHOD(WSLCE2E_Session_Shell_HelpCommand)
    {
        WSL2_TEST_ONLY();

        auto result = RunWslc(L"session shell --help");
        result.Verify({.Stdout = GetSessionShellHelpMessage(), .Stderr = L"", .ExitCode = 0});
    }

private:
    std::wstring GetSessionListHelpMessage() const
    {
        std::wstringstream output;
        output << GetWslcHeader()
               << L"Lists active session(s).\r\n\r\n"
               << L"Usage: wslc session list [<options>]\r\n\r\n"
               << L"The following options are available:\r\n"
               << L"  -v,--verbose  Show detailed information about the listed sessions.\r\n"
               << L"  -h,--help     Shows help about the selected command\r\n\r\n";
        return output.str();
    }

    std::wstring GetSessionShellHelpMessage() const
    {
        std::wstringstream output;
        output << GetWslcHeader()
               << L"Attaches to a session.\r\n\r\n"
               << L"Usage: wslc session shell [<options>] [<session-id>]\r\n\r\n"
               << L"The following arguments are available:\r\n"
               << L"  session-id  Session ID\r\n\r\n"
               << L"The following options are available:\r\n"
               << L"  -h,--help   Shows help about the selected command\r\n\r\n";
        return output.str();
    }
};
} // namespace WSLCE2ETests
