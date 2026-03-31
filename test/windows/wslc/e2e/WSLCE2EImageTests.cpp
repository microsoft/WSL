/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCE2EImageTests.cpp

Abstract:

    This file contains end-to-end tests for WSLC.
--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCExecutor.h"
#include "WSLCE2EHelpers.h"

namespace WSLCE2ETests {

class WSLCE2EImageTests
{
    WSLC_TEST_CLASS(WSLCE2EImageTests)

    TEST_METHOD(WSLCE2E_Image_HelpCommand)
    {
        WSL2_TEST_ONLY();
        auto result = RunWslc(L"image --help");
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"", .ExitCode = 0});
    }

    TEST_METHOD(WSLCE2E_Image_NoSubcommand_ShowsHelp)
    {
        WSL2_TEST_ONLY();
        auto result = RunWslc(L"image");
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"", .ExitCode = 0});
    }

    TEST_METHOD(WSLCE2E_Image_InvalidCommand_DisplaysErrorMessage)
    {
        WSL2_TEST_ONLY();
        auto result = RunWslc(L"image INVALID_CMD");
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"Unrecognized command: 'INVALID_CMD'\r\n", .ExitCode = 1});
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
        return L"Image command.\r\n\r\n";
    }

    std::wstring GetUsage() const
    {
        return L"Usage: wslc image [<command>] [<options>]\r\n\r\n";
    }

    std::wstring GetAvailableCommands() const
    {
        std::wstringstream commands;
        commands << L"The following sub-commands are available:\r\n"
                 << L"  build    Build an image from a Dockerfile.\r\n"
                 << L"  remove   Remove images.\r\n"
                 << L"  inspect  Inspect images.\r\n"
                 << L"  list     List images.\r\n"
                 << L"  load     Load images.\r\n"
                 << L"  pull     Pull images.\r\n"
                 << L"  save     Save images.\r\n"
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