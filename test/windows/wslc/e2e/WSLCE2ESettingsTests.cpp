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

class WSLCE2ESettingsTests
{
    WSLC_TEST_CLASS(WSLCE2ESettingsTests)

    TEST_METHOD(WSLCE2E_Settings_HelpCommand)
    {
        WSL2_TEST_ONLY();

        auto result = RunWslc(L"settings --help");
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"", .ExitCode = 0});
    }

    TEST_METHOD(WSLCE2E_Settings_InvalidCommand)
    {
        WSL2_TEST_ONLY();

        auto result = RunWslc(L"settings INVALID_CMD");
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"Unrecognized command: 'INVALID_CMD'\r\n", .ExitCode = 1});
    }

    TEST_METHOD(WSLCE2E_Settings_Reset_HelpCommand)
    {
        WSL2_TEST_ONLY();

        auto result = RunWslc(L"settings reset --help");
        result.Verify({.Stdout = GetSettingsResetHelpMessage(), .Stderr = L"", .ExitCode = 0});
    }

    TEST_METHOD(WSLCE2E_Settings_Reset_Success)
    {
        WSL2_TEST_ONLY();

        // Reset settings to defaults
        auto result = RunWslc(L"settings reset");
        result.Verify({.Stderr = L"", .ExitCode = 0});
        
        // Should contain success message
        VERIFY_IS_TRUE(result.Stdout.has_value());
        VERIFY_ARE_NOT_EQUAL(std::wstring::npos, result.Stdout->find(L"Settings reset to defaults"));
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
        return L"Opens the wslc user settings file in the system default editor for .yaml files.\r\n"
               L"On first run, creates the file with all settings commented out at their defaults.\r\n\r\n";
    }

    std::wstring GetUsage() const
    {
        return L"Usage: wslc settings [<command>] [<options>]\r\n\r\n";
    }

    std::wstring GetAvailableCommands() const
    {
        std::wstringstream commands;
        commands << L"The following sub-commands are available:\r\n"
                 << L"  reset  Reset settings to built-in defaults.\r\n"
                 << L"\r\n"
                 << L"For more details on a specific command, pass it the help argument. [-h]\r\n"
                 << L"\r\n";
        return commands.str();
    }

    std::wstring GetAvailableOptions() const
    {
        std::wstringstream options;
        options << L"The following options are available:\r\n" //
                << L"  -h,--help  Shows help about the selected command\r\n\r\n";
        return options.str();
    }

    std::wstring GetSettingsResetHelpMessage() const
    {
        std::wstringstream output;
        output << GetWslcHeader()                         //
               << L"Reset settings to built-in defaults.\r\n\r\n"
               << L"Usage: wslc settings reset [<options>]\r\n\r\n"
               << L"The following options are available:\r\n"
               << L"  -h,--help  Shows help about the selected command\r\n\r\n";
        return output.str();
    }
};
} // namespace WSLCE2ETests
