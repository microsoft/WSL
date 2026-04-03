/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCE2EImageListTests.cpp

Abstract:

    This file contains end-to-end tests for WSLC.
--*/

#include "precomp.h"
#include "windows/Common.h"
#include "ImageModel.h"
#include "WSLCExecutor.h"
#include "WSLCE2EHelpers.h"

namespace WSLCE2ETests {
using namespace wsl::shared;

using namespace wsl::windows::wslc::models;

class WSLCE2EImageListTests
{
    WSLC_TEST_CLASS(WSLCE2EImageListTests)

    TEST_CLASS_SETUP(ClassSetup)
    {
        EnsureImageIsLoaded(DebianImage);
        EnsureImageIsLoaded(AlpineImage);
        return true;
    }

    TEST_CLASS_CLEANUP(ClassCleanup)
    {
        EnsureImageIsDeleted(DebianImage);
        EnsureImageIsDeleted(AlpineImage);
        return true;
    }

    TEST_METHOD(WSLCE2E_Image_List_HelpCommand)
    {
        WSL2_TEST_ONLY();
        const auto result = RunWslc(L"image list --help");
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"", .ExitCode = 0});
    }

    TEST_METHOD(WSLCE2E_Image_List_DisplayLoadedImage)
    {
        WSL2_TEST_ONLY();

        const auto result = RunWslc(L"image list");
        result.Verify({.Stderr = L"", .ExitCode = 0});
        for (const auto& line : result.GetStdoutLines())
        {
            if (line.find(DebianImage.Name) != std::wstring::npos && line.find(DebianImage.Tag) != std::wstring::npos)
            {
                return;
            }
        }

        VERIFY_FAIL(L"Failed to find the loaded image in the output");
    }

    TEST_METHOD(WSLCE2E_Image_List_QuietOption_OutputsNamesOnly)
    {
        WSL2_TEST_ONLY();

        const auto result = RunWslc(L"image list --quiet");
        result.Verify({.Stderr = L"", .ExitCode = 0});

        bool imageFound = false;
        for (const auto& line : result.GetStdoutLines())
        {
            if (line == DebianImage.NameAndTag())
            {
                imageFound = true;
                break;
            }
        }

        VERIFY_IS_TRUE(imageFound);
    }

    TEST_METHOD(WSLCE2E_Image_List_InvalidFormatOption)
    {
        WSL2_TEST_ONLY();

        const auto result = RunWslc(L"image list --format invalid");
        result.Verify({.Stderr = L"Invalid format value: invalid is not a recognized format type. Supported format types are: json, table.\r\n", .ExitCode = 1});
    }

    TEST_METHOD(WSLCE2E_Image_List_JsonFormat)
    {
        WSL2_TEST_ONLY();

        const auto result = RunWslc(L"image list --format json");
        result.Verify({.Stderr = L"", .ExitCode = 0});

        const auto images = wsl::shared::FromJson<std::vector<ImageInformation>>(result.Stdout.value().c_str());

        VERIFY_ARE_EQUAL(2u, images.size());

        std::vector<std::wstring> imageNames;
        for (const auto& image : images)
        {
            auto nameAndTag = std::format(
                L"{}:{}",
                wsl::shared::string::MultiByteToWide(image.Repository.value_or("<untagged>")),
                wsl::shared::string::MultiByteToWide(image.Tag.value_or("<untagged>")));
            imageNames.push_back(nameAndTag);
        }

        VERIFY_ARE_NOT_EQUAL(imageNames.end(), std::find(imageNames.begin(), imageNames.end(), DebianImage.NameAndTag()));
        VERIFY_ARE_NOT_EQUAL(imageNames.end(), std::find(imageNames.begin(), imageNames.end(), AlpineImage.NameAndTag()));
    }

    TEST_METHOD(WSLCE2E_Image_List_TableFormat_HasExpectedColumns)
    {
        WSL2_TEST_ONLY();

        const auto result = RunWslc(L"image list");
        result.Verify({.Stderr = L"", .ExitCode = 0});

        bool foundHeader = false;
        for (const auto& line : result.GetStdoutLines())
        {
            if (line.find(L"REPOSITORY") != std::wstring::npos && line.find(L"TAG") != std::wstring::npos &&
                line.find(L"IMAGE ID") != std::wstring::npos && line.find(L"CREATED") != std::wstring::npos &&
                line.find(L"SIZE") != std::wstring::npos)
            {
                foundHeader = true;
                break;
            }
        }

        VERIFY_IS_TRUE(foundHeader, L"Expected table header with REPOSITORY, TAG, IMAGE ID, CREATED, SIZE columns");
    }

private:
    const TestImage& DebianImage = DebianTestImage();
    const TestImage& AlpineImage = AlpineTestImage();

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
        return Localization::WSLCCLI_ImageListLongDesc() + L"\r\n\r\n";
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
                << L"  --format      " << Localization::WSLCCLI_FormatArgDescription() << L"\r\n"
                << L"  --no-trunc    Do not truncate output\r\n"
                << L"  -q,--quiet    Outputs the container IDs only\r\n"
                << L"  --session     Specify the session to use\r\n"
                << L"  -v,--verbose  Output verbose details\r\n"
                << L"  -h,--help     Shows help about the selected command\r\n"
                << L"\r\n";
        return options.str();
    }
};
} // namespace WSLCE2ETests