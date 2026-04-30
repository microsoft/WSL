/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCE2EImageInspectTests.cpp

Abstract:

    This file contains end-to-end tests for WSLC.
--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCExecutor.h"
#include "WSLCE2EHelpers.h"
#include <wslc_schema.h>

namespace WSLCE2ETests {
using namespace wsl::shared;

class WSLCE2EImageInspectTests
{
    WSLC_TEST_CLASS(WSLCE2EImageInspectTests)

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

    WSLC_TEST_METHOD(WSLCE2E_Image_Inspect_HelpCommand)
    {
        auto result = RunWslc(L"image inspect --help");
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Inspect_MissingImageName)
    {
        auto result = RunWslc(L"image inspect");
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"Required argument not provided: 'image'\r\n", .ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Inspect_ImageNotFound)
    {
        auto result = RunWslc(std::format(L"image inspect {}", InvalidImage.NameAndTag()));
        result.Verify({.Stdout = L"[]\r\n", .Stderr = std::format(L"Image '{}' not found.\r\n", InvalidImage.NameAndTag()), .ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Inspect_Success)
    {
        auto result = RunWslc(std::format(L"image inspect {}", DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        auto inspectData =
            wsl::shared::FromJson<std::vector<wsl::windows::common::wslc_schema::InspectImage>>(result.Stdout.value().c_str());
        VERIFY_ARE_EQUAL(1u, inspectData.size());
        VERIFY_IS_TRUE(inspectData[0].RepoTags.has_value());
        VERIFY_ARE_EQUAL(1u, inspectData[0].RepoTags.value().size());
        VERIFY_ARE_EQUAL(DebianImage.NameAndTag(), wsl::shared::string::MultiByteToWide(inspectData[0].RepoTags.value()[0]));
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
        return Localization::WSLCCLI_ImageInspectLongDesc() + L"\r\n\r\n";
    }

    std::wstring GetUsage() const
    {
        return L"Usage: wslc image inspect [<options>] <image>\r\n\r\n";
    }

    std::wstring GetAvailableCommands() const
    {
        std::wstringstream commands;
        commands << L"The following arguments are available:\r\n" //
                 << L"  image      Image name\r\n"                //
                 << L"\r\n";
        return commands.str();
    }

    std::wstring GetAvailableOptions() const
    {
        std::wstringstream options;
        options << L"The following options are available:\r\n"               //
                << L"  --session  Specify the session to use\r\n"            //
                << L"  -?,--help  Shows help about the selected command\r\n" //
                << L"\r\n";
        return options.str();
    }
};
} // namespace WSLCE2ETests