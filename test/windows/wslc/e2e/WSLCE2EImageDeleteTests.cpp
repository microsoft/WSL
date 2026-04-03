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
using namespace wsl::shared;

class WSLCE2EImageDeleteTests
{
    WSLC_TEST_CLASS(WSLCE2EImageDeleteTests)

    TEST_METHOD_SETUP(MethodSetup)
    {
        EnsureContainerDoesNotExist(WslcContainerName);
        EnsureImageIsDeleted(DebianImage);
        return true;
    }

    TEST_CLASS_CLEANUP(ClassCleanup)
    {
        EnsureContainerDoesNotExist(WslcContainerName);
        EnsureImageIsDeleted(DebianImage);
        return true;
    }

    TEST_METHOD(WSLCE2E_Image_Delete_HelpCommand)
    {
        WSL2_TEST_ONLY();

        auto result = RunWslc(L"image delete --help");
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"", .ExitCode = 0});
    }

    TEST_METHOD(WSLCE2E_Image_Delete_ImageNotFound)
    {
        WSL2_TEST_ONLY();

        auto result = RunWslc(std::format(L"image delete {}", InvalidImage.Name));
        auto errorMessage = std::format(L"No such image: {}\r\nError code: WSLC_E_IMAGE_NOT_FOUND\r\n", InvalidImage.NameAndTag());
        result.Verify({.Stdout = L"", .Stderr = errorMessage, .ExitCode = 1});
    }

    TEST_METHOD(WSLCE2E_Image_Delete_MissingImageName)
    {
        WSL2_TEST_ONLY();

        auto result = RunWslc(L"image delete");
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"Required argument not provided: 'image'\r\n", .ExitCode = 1});
    }

    TEST_METHOD(WSLCE2E_Image_Delete_UnusedImage_Success)
    {
        WSL2_TEST_ONLY();

        EnsureImageIsLoaded(DebianImage);
        VerifyImageIsNotUsed(DebianImage);

        auto result = RunWslc(std::format(L"image delete {}", DebianImage.Name));
        result.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 0});
    }

    TEST_METHOD(WSLCE2E_Image_Delete_UsedImage_Failure)
    {
        WSL2_TEST_ONLY();

        EnsureImageIsLoaded(DebianImage);
        VerifyImageIsNotUsed(DebianImage);

        auto createResult = RunWslc(std::format(L"container create --name {} {}", WslcContainerName, DebianImage.NameAndTag()));
        createResult.Verify({.Stderr = L"", .ExitCode = 0});

        VerifyImageIsUsed(DebianImage);

        auto inspectContainer = InspectContainer(WslcContainerName);
        auto containerId = GetHashId(inspectContainer.Id);
        auto inspectImage = InspectImage(DebianImage.NameAndTag());
        auto imageId = GetHashId(inspectImage.Id);

        auto result = RunWslc(std::format(L"image delete {}", DebianImage.Name));
        auto errorMessage = std::format(
            L"conflict: unable to remove repository reference \"{}\" (must force) - container {} is using its referenced image "
            L"{}\r\nError code: ERROR_SHARING_VIOLATION\r\n",
            DebianImage.Name,
            containerId,
            imageId);
        result.Verify({.Stdout = L"", .Stderr = errorMessage, .ExitCode = 1});
    }

    TEST_METHOD(WSLCE2E_Image_DeleteForce_UsedImage_Success)
    {
        WSL2_TEST_ONLY();

        EnsureImageIsLoaded(DebianImage);
        VerifyImageIsNotUsed(DebianImage);

        auto createResult = RunWslc(std::format(L"container create --name {} {}", WslcContainerName, DebianImage.NameAndTag()));
        createResult.Verify({.Stderr = L"", .ExitCode = 0});

        VerifyImageIsUsed(DebianImage);

        auto result = RunWslc(std::format(L"image delete --force {}", DebianImage.Name));
        result.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 0});
    }

    TEST_METHOD(WSLCE2E_Image_DeleteNoPrune)
    {
        WSL2_TEST_ONLY();

        // TODO: Implement once 'image tag' is implemented
        SKIP_TEST_NOT_IMPL();
    }

private:
    const std::wstring WslcContainerName = L"wslc-test-container";
    const TestImage& DebianImage = DebianTestImage();
    const TestImage& InvalidImage = InvalidTestImage();

    std::wstring GetHelpMessage() const
    {
        std::wstringstream output;
        output << GetWslcHeader()              //
               << GetDescription()             //
               << GetUsage()                   //
               << GetAvailableCommandAliases() //
               << GetAvailableCommands()       //
               << GetAvailableOptions();
        return output.str();
    }

    std::wstring GetDescription() const
    {
        return Localization::WSLCCLI_ImageRemoveLongDesc() + L"\r\n\r\n";
    }

    std::wstring GetUsage() const
    {
        return L"Usage: wslc image remove [<options>] <image>\r\n\r\n";
    }

    std::wstring GetAvailableCommandAliases() const
    {
        return L"The following command aliases are available: delete rm\r\n\r\n";
    }

    std::wstring GetAvailableCommands() const
    {
        std::wstringstream commands;
        commands << L"The following arguments are available:\r\n" //
                 << L"  image       Image name\r\n"               //
                 << L"\r\n";
        return commands.str();
    }

    std::wstring GetAvailableOptions() const
    {
        std::wstringstream options;
        options << L"The following options are available:\r\n"                    //
                << L"  -f,--force  Delete images even if they are being used\r\n" //
                << L"  --no-prune  Do not delete untagged parents\r\n"            //
                << L"  --session   Specify the session to use\r\n"                //
                << L"  -h,--help   Shows help about the selected command\r\n"     //
                << L"\r\n";
        return options.str();
    }
};
} // namespace WSLCE2ETests