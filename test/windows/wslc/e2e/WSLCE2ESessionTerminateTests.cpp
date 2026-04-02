/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCE2ESessionTerminateTests.cpp

Abstract:

    This file contains end-to-end tests for WSLC session terminate command.
--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCExecutor.h"
#include "WSLCE2EHelpers.h"

namespace WSLCE2ETests {

class WSLCE2ESessionTerminateTests
{
    WSLC_TEST_CLASS(WSLCE2ESessionTerminateTests)

    TEST_METHOD(WSLCE2E_Session_Terminate_HelpCommand)
    {
        WSL2_TEST_ONLY();
        auto result = RunWslc(L"session terminate --help");
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"", .ExitCode = 0});
    }

    TEST_METHOD(WSLCE2E_Session_Terminate_Implicit)
    {
        WSL2_TEST_ONLY();

        // Run container list to create the default session if it does not already exist
        auto result = RunWslc(L"container list");
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // Verify session list shows the admin session name
        result = RunWslc(L"session list");
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_IS_TRUE(result.Stdout.has_value());
        VERIFY_IS_TRUE(result.Stdout->find(L"wslc-cli-admin") != std::wstring::npos);

        // Terminate the session
        result = RunWslc(L"session terminate");
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // Verify session no longer shows up
        result = RunWslc(L"session list");
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_IS_TRUE(result.Stdout.has_value());
        VERIFY_IS_FALSE(result.Stdout->find(L"wslc-cli-admin") != std::wstring::npos);

        // Repeat test for non-elevated session.

        // Run container list to create the default session if it does not already exist
        result = RunWslc(L"container list", ElevationType::NonElevated);
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // Verify session list shows the non-elevated session name
        result = RunWslc(L"session list");
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_IS_TRUE(result.Stdout.has_value());
        VERIFY_IS_TRUE(result.Stdout->find(L"wslc-cli\r\n") != std::wstring::npos);

        // Terminate the session
        result = RunWslc(L"session terminate", ElevationType::NonElevated);
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // Verify session no longer shows up
        result = RunWslc(L"session list");
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_IS_TRUE(result.Stdout.has_value());
        VERIFY_IS_FALSE(result.Stdout->find(L"wslc-cli\r\n") != std::wstring::npos);
    }

    TEST_METHOD(WSLCE2E_Session_Terminate_Explicit)
    {
        WSL2_TEST_ONLY();

        // Run container list to create the default session if it does not already exist
        auto result = RunWslc(L"container list");
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // Verify session list shows the admin session name
        result = RunWslc(L"session list");
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_IS_TRUE(result.Stdout.has_value());
        VERIFY_IS_TRUE(result.Stdout->find(L"wslc-cli-admin") != std::wstring::npos);

        // Terminate the session
        result = RunWslc(L"session terminate wslc-cli-admin");
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // Verify session no longer shows up
        result = RunWslc(L"session list");
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_IS_TRUE(result.Stdout.has_value());
        VERIFY_IS_FALSE(result.Stdout->find(L"wslc-cli-admin") != std::wstring::npos);

        // Repeat test for non-elevated session.

        // Run container list to create the default session if it does not already exist
        result = RunWslc(L"container list", ElevationType::NonElevated);
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // Verify session list shows the non-elevated session name
        result = RunWslc(L"session list");
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_IS_TRUE(result.Stdout.has_value());
        VERIFY_IS_TRUE(result.Stdout->find(L"wslc-cli\r\n") != std::wstring::npos);

        // Terminate the session
        result = RunWslc(L"session terminate wslc-cli", ElevationType::NonElevated);
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // Verify session no longer shows up
        result = RunWslc(L"session list");
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_IS_TRUE(result.Stdout.has_value());
        VERIFY_IS_FALSE(result.Stdout->find(L"wslc-cli\r\n") != std::wstring::npos);
    }

    TEST_METHOD(WSLCE2E_Session_Terminate_MixedElevation)
    {
        WSL2_TEST_ONLY();

        // Run container list to create the default sessions if they do not already exist.
        auto result = RunWslc(L"container list", ElevationType::Elevated);
        result.Verify({.Stderr = L"", .ExitCode = 0});
        result = RunWslc(L"container list", ElevationType::NonElevated);
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // Verify session list shows both sessions.
        result = RunWslc(L"session list");
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_IS_TRUE(result.Stdout.has_value());
        VERIFY_IS_TRUE(result.Stdout->find(L"wslc-cli-admin") != std::wstring::npos);
        VERIFY_IS_TRUE(result.Stdout->find(L"wslc-cli\r\n") != std::wstring::npos);

        // Attempt to terminate the admin session from the non-elevated process and fail.
        result = RunWslc(L"session terminate wslc-cli-admin", ElevationType::NonElevated);
        result.Verify({.Stderr = L"The requested operation requires elevation. \r\nError code: ERROR_ELEVATION_REQUIRED\r\n", .ExitCode = 1});

        // Terminate the non-elevated session from the elevated process.
        result = RunWslc(L"session terminate wslc-cli", ElevationType::Elevated);
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // Verify non-elevated session no longer shows up
        result = RunWslc(L"session list");
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_IS_TRUE(result.Stdout.has_value());
        VERIFY_IS_FALSE(result.Stdout->find(L"wslc-cli\r\n") != std::wstring::npos);
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
        return L"Terminates an active session. If no session is specified, the default session will be terminated.\r\n\r\n";
    }

    std::wstring GetUsage() const
    {
        return L"Usage: wslc session terminate [<options>] [<session-id>]\r\n\r\n";
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
