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

class WSLCE2EImageDeleteTests
{
    WSL_TEST_CLASS(WSLCE2EImageDeleteTests)

    TEST_METHOD_SETUP(MethodSetup)
    {
        EnsureImageIsDeleted(DebianImage);
        EnsureContainerDoesNotExist(WslcContainerName);
        return true;
    }

    TEST_CLASS_CLEANUP(ClassCleanup)
    {
        EnsureImageIsDeleted(DebianImage);
        return true;
    }

    TEST_METHOD(WSLCE2E_Image_Delete_HelpCommand)
    {
        WSL2_TEST_ONLY();

        auto result = RunWslc(L"image delete --help");
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"", .ExitCode = S_OK});
    }

    TEST_METHOD(WSLCE2E_Image_Delete_NonExistentImage)
    {
        WSL2_TEST_ONLY();

        auto result = RunWslc(std::format(L"image delete {}", InvalidImage.Name));
        auto errorMessage = std::format(L"No such image: {}\r\nError code: WSLA_E_IMAGE_NOT_FOUND\r\n", InvalidImage.Name);
        result.Verify({.Stdout = L"", .Stderr = errorMessage, .ExitCode = 1});
    }

    TEST_METHOD(WSLCE2E_Image_Delete_MissingImageName)
    {
        WSL2_TEST_ONLY();

        auto result = RunWslc(L"image delete");
        result.Verify({.Stdout = L"", .Stderr = L"Required argument not provided: 'image'", .ExitCode = 1});
    }

    TEST_METHOD(WSLCE2E_Image_Delete_UnusedImage_Success)
    {
        WSL2_TEST_ONLY();

        EnsureImageIsLoaded(DebianImage);
        VerifyImageIsNotUsed(DebianImage);

        auto result = RunWslc(std::format(L"image delete {}", DebianImage.Name));
        result.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = S_OK});
    }

    TEST_METHOD(WSLCE2E_Image_Delete_UsedImage_Failure)
    {
        WSL2_TEST_ONLY();

        EnsureImageIsLoaded(DebianImage);
        VerifyImageIsNotUsed(DebianImage);

        auto createResult = RunWslc(std::format(L"container create --name {} {}", WslcContainerName, DebianImage.Name));
        createResult.Verify({.Stderr = L"", .ExitCode = S_OK});

        VerifyImageIsUsed(DebianImage);
        auto containerDetails = GetContainerDetails(WslcContainerName);
        VERIFY_ARE_EQUAL(DebianImage.NameAndTag(), containerDetails["Image"].get<std::string>());
        auto containerId = containerDetails["Id"].get<std::string>();

        auto result = RunWslc(std::format(L"image delete {}", DebianImage.Name));
        result.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 1});
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
        return L"Deletes images.\r\n\r\n";
    }

    std::wstring GetUsage() const
    {
        return L"Usage: wslc image delete [<options>] <image>\r\n\r\n";
    }

    std::wstring GetAvailableCommands() const
    {
        std::wstringstream commands;
        commands << L"The following arguments are available:\r\n" //
                 << L"  image       Image name\r\n" //
                 << L"\r\n";
        return commands.str();
    }

    std::wstring GetAvailableOptions() const
    {
        std::wstringstream options;
        options << L"The following options are available:\r\n" //
                << L"  -f,--force         Delete containers even if they are running\r\n" //
                << L"  --no-prune         Do not delete untagged parents\r\n" //
                << L"  -h,--help          Shows help about the selected command\r\n" //
                << L"\r\n";
        return options.str();
    }
};
} // namespace WSLCE2ETests