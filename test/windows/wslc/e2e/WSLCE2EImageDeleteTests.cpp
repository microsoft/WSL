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
#include "TestImageRegistry.h"

namespace WSLCE2ETests {
using namespace wsl::shared;

class WSLCE2EImageDeleteTests
{
    WSLC_TEST_CLASS(WSLCE2EImageDeleteTests)

    TEST_METHOD_SETUP(MethodSetup)
    {
        EnsureContainerDoesNotExist(WslcContainerName);
        TestImageRegistry::Instance().Delete(DebianImage);
        TestImageRegistry::Instance().Delete(AlpineImage);
        TestImageRegistry::Instance().Delete(NoPruneTaggedImage);
        return true;
    }

    TEST_CLASS_CLEANUP(ClassCleanup)
    {
        EnsureContainerDoesNotExist(WslcContainerName);
        TestImageRegistry::Instance().Delete(DebianImage);
        TestImageRegistry::Instance().Delete(AlpineImage);
        TestImageRegistry::Instance().Delete(NoPruneTaggedImage);
        return true;
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Delete_HelpCommand)
    {
        auto result = RunWslc(L"image delete --help");
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_IS_FALSE(result.Stdout.value().empty());
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Delete_ImageNotFound)
    {
        auto result = RunWslc(std::format(L"image delete {}", InvalidImage.Name));
        auto errorMessage =
            FormatErrorMessage(std::format(L"No such image: {}", InvalidImage.NameAndTag()), L"WSLC_E_IMAGE_NOT_FOUND");
        result.Verify({.Stdout = L"", .Stderr = errorMessage, .ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Delete_MissingImageName)
    {
        auto result = RunWslc(L"image delete");
        result.Verify({.Stdout = L"", .ExitCode = 1});
        VERIFY_IS_TRUE(result.StderrContainsSubstring(L"Required argument not provided: 'image'"));
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Delete_UnusedImage_Success)
    {
        TestImageRegistry::Instance().EnsureLoaded(DebianImage);
        VerifyImageIsNotUsed(DebianImage);

        auto result = RunWslc(std::format(L"image delete {}", DebianImage.Name));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_IS_TRUE(result.StdoutContainsSubstring(std::format(L"Untagged: {}", DebianImage.NameAndTag())));
        VERIFY_IS_TRUE(result.StdoutContainsSubstring(L"Deleted: sha256:"));
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Delete_MultipleUnusedImages_Success)
    {
        TestImageRegistry::Instance().EnsureLoaded(DebianImage);
        TestImageRegistry::Instance().EnsureLoaded(AlpineImage);
        VerifyImageIsNotUsed(DebianImage);
        VerifyImageIsNotUsed(AlpineImage);

        auto result = RunWslc(std::format(L"image delete {} {}", DebianImage.Name, AlpineImage.Name));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_IS_TRUE(result.StdoutContainsSubstring(std::format(L"Untagged: {}", DebianImage.NameAndTag())));
        VERIFY_IS_TRUE(result.StdoutContainsSubstring(std::format(L"Untagged: {}", AlpineImage.NameAndTag())));
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Delete_UsedImage_Failure)
    {
        TestImageRegistry::Instance().EnsureLoaded(DebianImage);
        VerifyImageIsNotUsed(DebianImage);

        auto createResult = RunWslc(std::format(L"container create --name {} {}", WslcContainerName, DebianImage.NameAndTag()));
        createResult.Verify({.Stderr = L"", .ExitCode = 0});

        VerifyImageIsUsed(DebianImage);

        auto inspectContainer = InspectContainer(WslcContainerName);
        auto containerId = GetHashId(inspectContainer.Id);
        auto inspectImage = InspectImage(DebianImage.NameAndTag());
        auto imageId = GetHashId(inspectImage.Id);

        auto result = RunWslc(std::format(L"image delete {}", DebianImage.Name));
        auto errorMessage = FormatErrorMessage(
            std::format(
                L"conflict: unable to remove repository reference \"{}\" (must force) - container {} is using its referenced "
                L"image {}",
                DebianImage.Name,
                containerId,
                imageId),
            L"ERROR_SHARING_VIOLATION");
        result.Verify({.Stdout = L"", .Stderr = errorMessage, .ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_DeleteForce_UsedImage_Success)
    {
        TestImageRegistry::Instance().EnsureLoaded(DebianImage);
        VerifyImageIsNotUsed(DebianImage);

        auto createResult = RunWslc(std::format(L"container create --name {} {}", WslcContainerName, DebianImage.NameAndTag()));
        createResult.Verify({.Stderr = L"", .ExitCode = 0});

        VerifyImageIsUsed(DebianImage);

        auto result = RunWslc(std::format(L"image delete --force {}", DebianImage.Name));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_IS_TRUE(result.StdoutContainsSubstring(std::format(L"Untagged: {}", DebianImage.NameAndTag())));
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_DeleteNoPrune)
    {
        // Tag debian a second time, then remove via the alias with --no-prune.
        // The alias must disappear while the original tag stays resolvable.
        TestImageRegistry::Instance().EnsureLoaded(DebianImage);
        TestImageRegistry::Instance().Delete(NoPruneTaggedImage);

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
};
} // namespace WSLCE2ETests
