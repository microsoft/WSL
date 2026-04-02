/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCE2ESettingsTests.cpp

Abstract:

    This file contains end-to-end tests for WSLC settings commands.
--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCExecutor.h"
#include "WSLCE2EHelpers.h"

namespace WSLCE2ETests {

class WSLCE2ESettingsResetTests
{
    WSLC_TEST_CLASS(WSLCE2ESettingsResetTests)

    TEST_METHOD(WSLCE2E_Settings_Reset_HelpCommand)
    {
        WSL2_TEST_ONLY();

        auto result = RunWslc(L"settings reset --help");
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"", .ExitCode = 0});
    }

    TEST_METHOD(WSLCE2E_Settings_Reset_Success)
    {
        WSL2_TEST_ONLY();

        auto result = RunWslc(L"settings reset");
        result.Verify({.Stdout = L"Settings reset to defaults.\r\n", .Stderr = L"", .ExitCode = 0});
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
        return L"Overwrites the settings file with a commented-out defaults template.\r\n\r\n";
    }

    std::wstring GetUsage() const
    {
        return L"Usage: wslc settings reset [<options>]\r\n\r\n";
    }

    std::wstring GetAvailableOptions() const
    {
        std::wstringstream options;
        options << L"The following options are available:\r\n" //
                << L"  -h,--help  Shows help about the selected command\r\n\r\n";
        return options.str();
    }
};
} // namespace WSLCE2ETests
