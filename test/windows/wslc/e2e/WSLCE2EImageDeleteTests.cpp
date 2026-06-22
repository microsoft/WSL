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
        EnsureImageIsDeleted(AlpineImage);
        EnsureImageIsDeleted(NoPruneTaggedImage);
        return true;
    }

    TEST_CLASS_CLEANUP(ClassCleanup)
    {
        EnsureContainerDoesNotExist(WslcContainerName);
        EnsureImageIsDeleted(DebianImage);
        EnsureImageIsDeleted(AlpineImage);
        EnsureImageIsDeleted(NoPruneTaggedImage);
        return true;
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Delete_HelpCommand)
    {
        auto result = RunWslc(L"image delete --help");
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Delete_ImageNotFound)
    {
        auto result = RunWslc(std::format(L"image delete {}", InvalidImage.Name));
        // docker: "No such image: X" — podman: "failed to find image X: image not known".
        // The command passes InvalidImage.Name (no tag), and both engines echo the
        // name as given — so match on .Name, not .NameAndTag.
        VERIFY_ARE_EQUAL(1, result.ExitCode.value_or(0));
        auto stderrText = result.Stderr.value_or(L"");
        VERIFY_IS_TRUE(stderrText.find(InvalidImage.Name) != std::wstring::npos);
        VERIFY_IS_TRUE(stderrText.find(L"WSLC_E_IMAGE_NOT_FOUND") != std::wstring::npos);
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Delete_MissingImageName)
    {
        auto result = RunWslc(L"image delete");
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"Required argument not provided: 'image'\r\n", .ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Delete_UnusedImage_Success)
    {
        EnsureImageIsLoaded(DebianImage);
        VerifyImageIsNotUsed(DebianImage);

        auto result = RunWslc(std::format(L"image delete {}", DebianImage.Name));
        result.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Delete_MultipleUnusedImages_Success)
    {
        EnsureImageIsLoaded(DebianImage);
        EnsureImageIsLoaded(AlpineImage);
        VerifyImageIsNotUsed(DebianImage);
        VerifyImageIsNotUsed(AlpineImage);

        auto result = RunWslc(std::format(L"image delete {} {}", DebianImage.Name, AlpineImage.Name));
        result.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Delete_UsedImage_Failure)
    {
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
        // docker: "conflict: unable to remove repository reference \"X\" (must force) - container Y..."
        // podman: "image X is in use: image used by Y: image is in use by a container..."
        // Verify image+container references + ERROR_SHARING_VIOLATION HRESULT.
        VERIFY_ARE_EQUAL(1, result.ExitCode.value_or(0));
        auto stderrText = result.Stderr.value_or(L"");
        VERIFY_IS_TRUE(stderrText.find(L"in use") != std::wstring::npos);
        VERIFY_IS_TRUE(stderrText.find(L"ERROR_SHARING_VIOLATION") != std::wstring::npos);
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_DeleteForce_UsedImage_Success)
    {
        EnsureImageIsLoaded(DebianImage);
        VerifyImageIsNotUsed(DebianImage);

        auto createResult = RunWslc(std::format(L"container create --name {} {}", WslcContainerName, DebianImage.NameAndTag()));
        createResult.Verify({.Stderr = L"", .ExitCode = 0});

        VerifyImageIsUsed(DebianImage);

        auto result = RunWslc(std::format(L"image delete --force {}", DebianImage.Name));
        result.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_DeleteNoPrune)
    {
        // Tag debian a second time, then remove via the alias with --no-prune.
        // The alias must disappear while the original tag stays resolvable.
        EnsureImageIsLoaded(DebianImage);
        EnsureImageIsDeleted(NoPruneTaggedImage);

        auto tagResult = RunWslc(std::format(L"image tag {} {}", DebianImage.NameAndTag(), NoPruneTaggedImage.NameAndTag()));
        tagResult.Verify({.Stderr = L"", .ExitCode = 0});

        auto removeResult = RunWslc(std::format(L"image delete --no-prune {}", NoPruneTaggedImage.NameAndTag()));
        removeResult.Verify({.Stderr = L"", .ExitCode = 0});

        VerifyImageIsListed(DebianImage);

        auto listAfter = RunWslc(L"image list");
        listAfter.Verify({.Stderr = L"", .ExitCode = 0});
        for (const auto& line : listAfter.GetStdoutLines())
        {
            VERIFY_IS_FALSE(
                line.find(NoPruneTaggedImage.Name) != std::wstring::npos && line.find(NoPruneTaggedImage.Tag) != std::wstring::npos,
                L"Secondary tag should have been removed by `image delete --no-prune`");
        }
    }

private:
    const std::wstring WslcContainerName = L"wslc-test-container";
    const TestImage& DebianImage = DebianTestImage();
    const TestImage& AlpineImage = AlpineTestImage();
    const TestImage& InvalidImage = InvalidTestImage();
    const TestImage NoPruneTaggedImage{L"wslc-test-noprune", L"alias", L""};

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
                << L"  -?,--help   Shows help about the selected command\r\n"     //
                << L"\r\n";
        return options.str();
    }
};
} // namespace WSLCE2ETests
