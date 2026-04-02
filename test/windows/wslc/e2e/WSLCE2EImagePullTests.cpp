/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCE2EImagePullTests.cpp

Abstract:

    This file contains end-to-end tests for WSLC image pull command.
--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCExecutor.h"
#include "WSLCE2EHelpers.h"

namespace WSLCE2ETests {

class WSLCE2EImagePullTests
{
    WSLC_TEST_CLASS(WSLCE2EImagePullTests)

    TEST_METHOD(WSLCE2E_Image_Pull_HelpCommand)
    {
        WSL2_TEST_ONLY();

        auto result = RunWslc(L"image pull --help");
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"", .ExitCode = 0});
    }

    TEST_METHOD(WSLCE2E_Image_Pull_MissingImageId)
    {
        WSL2_TEST_ONLY();

        auto result = RunWslc(L"image pull");
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"Required argument not provided: 'image'\r\n", .ExitCode = 1});
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
        return L"Pulls images.\r\n\r\n";
    }

    std::wstring GetUsage() const
    {
        return L"Usage: wslc image pull [<options>] <image>\r\n\r\n";
    }

    std::wstring GetAvailableCommands() const
    {
        std::wstringstream commands;
        commands << L"The following arguments are available:\r\n" //
                 << L"  image      Image name\r\n"                   //
                 << L"\r\n";
        return commands.str();
    }

    std::wstring GetAvailableOptions() const
    {
        std::wstringstream options;
        options << L"The following options are available:\r\n" //
                << L"  --session  Specify the session to use\r\n" //
                << L"  -h,--help  Shows help about the selected command\r\n"
                << L"\r\n";
        return options.str();
    }
};
} // namespace WSLCE2ETests
