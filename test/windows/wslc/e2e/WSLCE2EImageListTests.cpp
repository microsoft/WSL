/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCE2EImageListTests.cpp

Abstract:

    This file contains end-to-end tests for WSLC.
--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCExecutor.h"
#include "WSLCE2EHelpers.h"

namespace WSLCE2ETests {

class WSLCE2EImageListTests
{
    WSL_TEST_CLASS(WSLCE2EImageListTests)

    TEST_CLASS_SETUP(ClassSetup)
    {
        EnsureImageIsLoaded(DebianImage);
        return true;
    }

    TEST_METHOD(WSLCE2E_Image_List_HelpCommand)
    {
        WSL2_TEST_ONLY();
        auto result = RunWslc(L"image list --help");
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"", .ExitCode = 0});
    }


    TEST_METHOD(WSLCE2E_Image_List_DisplayLoadedImage)
    {
        WSL2_TEST_ONLY();

        auto result = RunWslc(L"image list");
        result.Verify({.Stderr = L"", .ExitCode = 0});
        auto outputLines = result.GetStdoutLines();
        for (const auto& line : outputLines)
        {
            if (line.find(DebianImage.NameAndTag()) != std::string::npos)
            {
                return;
            }
        }

        VERIFY_FAIL(L"Failed to find the loaded image in the output");
    }

private:
    const TestImage& DebianImage = DebianTestImage();

    std::wstring GetHelpMessage() const
    {
        std::wstringstream output;
        output << GetWslcHeader()              //
               << GetDescription()             //
               << GetUsage()                   //
               << GetAvailableCommandAliases() //
               << GetAvailableOptions();
        return output.str();
    }

    std::wstring GetDescription() const
    {
        return L"Lists images.\r\n\r\n";
    }

    std::wstring GetUsage() const
    {
        return L"Usage: wslc image list [<options>]\r\n\r\n";
    }

    std::wstring GetAvailableCommandAliases() const
    {
        return L"The following command aliases are available: ls\r\n\r\n";
    }

    std::wstring GetAvailableOptions() const
    {
        std::wstringstream options;
        options << L"The following options are available:\r\n"
                << L"  --format      Output formatting (json or table) (Default:table)\r\n"
                << L"  -q,--quiet    Outputs the container IDs only\r\n"
                << L"  --session     Specify the session to use\r\n"
                << L"  -v,--verbose  Output verbose details\r\n"
                << L"  -h,--help     Shows help about the selected command\r\n"
                << L"\r\n";
        return options.str();
    }
};
} // namespace WSLCE2ETests