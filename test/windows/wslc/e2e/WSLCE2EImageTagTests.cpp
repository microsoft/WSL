/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCE2EImageDeleteTests.cpp

Abstract:

    This file contains end-to-end tests for WSLC.
--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCExecutor.h"
#include "WSLCE2EHelpers.h"

namespace WSLCE2ETests {

class WSLCE2EImageTagTests
{
    WSL_TEST_CLASS(WSLCE2EImageTagTests)

    TEST_CLASS_SETUP(ClassSetup)
    {
        EnsureImageIsLoaded(DebianImage);
        return true;
    }

    TEST_CLASS_CLEANUP(ClassCleanup)
    {
        EnsureImageIsDeleted(DebianImage);
        return true;
    }

    TEST_METHOD(WSLCE2E_Image_Tag_HelpCommand)
    {
        WSL2_TEST_ONLY();

        auto result = RunWslc(L"image tag --help");
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"", .ExitCode = 0});
    }

private:
    const std::wstring WslcContainerName = L"wslc-test-container";
    const TestImage& DebianImage = DebianTestImage();
    const TestImage& InvalidImage = InvalidTestImage();

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
        return L"Tags an existing image.\r\n\r\n";
    }

    std::wstring GetUsage() const
    {
        return L"Usage: wslc image tag [<options>] <source> <target>\r\n\r\n";
    }

    std::wstring GetAvailableCommands() const
    {
        std::wstringstream commands;
        commands << L"The following arguments are available:\r\n" //
                 << L"  source     Current or existing image reference in the image-name[:tag] format\r\n" //
                 << L"  target     New image reference in the image-name[:tag] format\r\n" //
                 << L"\r\n";
        return commands.str();
    }

    std::wstring GetAvailableOptions() const
    {
        std::wstringstream options;
        options << L"The following options are available:\r\n"                    //
                << L"  -h,--help  Shows help about the selected command\r\n"     //
                << L"\r\n";
        return options.str();
    }
};
} // namespace WSLCE2ETests