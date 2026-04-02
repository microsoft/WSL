/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCE2ESessionListTests.cpp

Abstract:

    This file contains end-to-end tests for WSLC session list command.
--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCExecutor.h"
#include "WSLCE2EHelpers.h"

namespace WSLCE2ETests {

class WSLCE2ESessionListTests
{
    WSLC_TEST_CLASS(WSLCE2ESessionListTests)

    TEST_METHOD(WSLCE2E_Session_List_HelpCommand)
    {
        WSL2_TEST_ONLY();
        auto result = RunWslc(L"session list --help");
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"", .ExitCode = 0});
    }

    TEST_METHOD(WSLCE2E_Session_List_ShowDefaultElevated)
    {
        WSL2_TEST_ONLY();

        // Run container list to create the default elevated session
        auto result = RunWslc(L"container list", ElevationType::Elevated);
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // Verify session list shows the admin session name
        result = RunWslc(L"session list", ElevationType::Elevated);
        result.Verify({.Stderr = L"", .ExitCode = 0});

        VERIFY_IS_TRUE(result.Stdout.has_value());
        VERIFY_IS_TRUE(result.Stdout->find(L"wslc-cli-admin") != std::wstring::npos);
    }

    TEST_METHOD(WSLCE2E_Session_List_ShowDefaultNonElevated)
    {
        WSL2_TEST_ONLY();

        // Run container list non-elevated to create the default non-elevated session
        auto result = RunWslc(L"container list", ElevationType::NonElevated);
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // Verify session list shows the non-admin session name
        result = RunWslc(L"session list", ElevationType::NonElevated);
        result.Verify({.Stderr = L"", .ExitCode = 0});

        VERIFY_IS_TRUE(result.Stdout.has_value());

        // The "\r\n" after session name is important to differentiate it from the admin session.
        VERIFY_IS_TRUE(result.Stdout->find(L"wslc-cli\r\n") != std::wstring::npos);
    }

private:
    std::wstring GetHelpMessage() const
    {
        std::wstringstream output;
        output << GetWslcHeader()        //
               << GetDescription()       //
               << GetUsage()             //
               << GetAvailableOptions();
        return output.str();
    }

    std::wstring GetDescription() const
    {
        return L"Lists active session(s).\r\n\r\n";
    }

    std::wstring GetUsage() const
    {
        return L"Usage: wslc session list [<options>]\r\n\r\n";
    }

    std::wstring GetAvailableOptions() const
    {
        std::wstringstream options;
        options << L"The following options are available:\r\n"
                << L"  -v,--verbose  Show detailed information about the listed sessions.\r\n"
                << L"  -h,--help     Shows help about the selected command\r\n"
                << L"\r\n";
        return options.str();
    }
};
} // namespace WSLCE2ETests
