/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCE2EImageTagTests.cpp

Abstract:

    This file contains end-to-end tests for WSLC.
--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCExecutor.h"
#include "WSLCE2EHelpers.h"
#include "TestImageRegistry.h"

namespace WSLCE2ETests {

using namespace wsl::shared::string;

class WSLCE2EImageTagTests
{
    WSLC_TEST_CLASS(WSLCE2EImageTagTests)

    TEST_METHOD_SETUP(MethodSetup)
    {
        TestImageRegistry::Instance().Delete(DebianTaggedImage);
        TestImageRegistry::Instance().EnsureLoaded(DebianImage);
        TestImageRegistry::Instance().EnsureLoaded(AlpineImage);
        return true;
    }

    TEST_CLASS_CLEANUP(ClassCleanup)
    {
        TestImageRegistry::Instance().Delete(DebianTaggedImage);
        return true;
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Tag_HelpCommand)
    {
        auto result = RunWslc(L"image tag --help");
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_IS_FALSE(result.Stdout.value().empty());
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Tag_MissingSourceAndTarget)
    {
        auto result = RunWslc(L"image tag");
        result.Verify({.Stdout = L"", .ExitCode = 1});
        VERIFY_IS_TRUE(result.StderrContainsSubstring(L"Required argument not provided: 'source'"));
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Tag_MissingTarget)
    {
        auto result = RunWslc(std::format(L"image tag {}", DebianImage.NameAndTag()));
        result.Verify({.Stdout = L"", .ExitCode = 1});
        VERIFY_IS_TRUE(result.StderrContainsSubstring(L"Required argument not provided: 'target'"));
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Tag_SourceImageNotFound)
    {
        auto result = RunWslc(std::format(L"image tag {} {}", InvalidImage.NameAndTag(), DebianTaggedImage.NameAndTag()));
        auto errorMessage =
            FormatErrorMessage(std::format(L"No such image: {}", InvalidImage.NameAndTag()), L"WSLC_E_IMAGE_NOT_FOUND");
        result.Verify({.Stdout = L"", .Stderr = errorMessage, .ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Tag_TargetImageWithDigest_Fail)
    {
        auto imageWithDigest = L"debian-mock:tag@sha256:11111111111111111111111111111111";
        auto result = RunWslc(std::format(L"image tag {} {}", DebianImage.NameAndTag(), imageWithDigest));
        auto errorMessage = FormatErrorMessage(
            std::format(L"Invalid image tag format: '{}'. Expected format is 'name:tag'", imageWithDigest), L"E_INVALIDARG");
        result.Verify({.Stdout = L"", .Stderr = errorMessage, .ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Tag_Success)
    {
        auto result = RunWslc(std::format(L"image tag {} {}", DebianImage.NameAndTag(), DebianTaggedImage.NameAndTag()));
        result.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 0});

        VerifyImageIsListed(DebianImage);
        VerifyImageIsListed(DebianTaggedImage);

        auto resultSourceInspect = RunWslc(std::format(L"image inspect {}", DebianImage.NameAndTag()));
        resultSourceInspect.Verify({.Stderr = L"", .ExitCode = 0});
        auto sourceInspect = resultSourceInspect.Stdout;

        auto resultTargetInspect = RunWslc(std::format(L"image inspect {}", DebianTaggedImage.NameAndTag()));
        resultTargetInspect.Verify({.Stderr = L"", .ExitCode = 0});
        auto targetInspect = resultTargetInspect.Stdout;

        VERIFY_IS_TRUE(sourceInspect.has_value());
        VERIFY_IS_TRUE(targetInspect.has_value());
        VERIFY_ARE_EQUAL(sourceInspect, targetInspect);
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Tag_SourceAndTargetAreTheSame_Noop)
    {
        auto result = RunWslc(std::format(L"image tag {} {}", DebianImage.NameAndTag(), DebianImage.NameAndTag()));
        result.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 0});

        VerifyImageIsListed(DebianImage);

        auto imageInspect = InspectImage(DebianImage.NameAndTag());
        VERIFY_ARE_EQUAL(1u, imageInspect.RepoTags->size());
        VERIFY_ARE_EQUAL(imageInspect.RepoTags->at(0), WideToMultiByte(DebianImage.NameAndTag()));
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Tag_TargetAlreadyExists_OverwritesTarget)
    {
        {
            auto result = RunWslc(std::format(L"image tag {} {}", DebianImage.NameAndTag(), DebianTaggedImage.NameAndTag()));
            result.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 0});

            auto resultSourceInspect = RunWslc(std::format(L"image inspect {}", DebianImage.NameAndTag()));
            resultSourceInspect.Verify({.Stderr = L"", .ExitCode = 0});
            auto sourceInspect = resultSourceInspect.Stdout;

            auto resultTargetInspect = RunWslc(std::format(L"image inspect {}", DebianTaggedImage.NameAndTag()));
            resultTargetInspect.Verify({.Stderr = L"", .ExitCode = 0});
            auto targetInspect = resultTargetInspect.Stdout;

            VERIFY_IS_TRUE(sourceInspect.has_value());
            VERIFY_IS_TRUE(targetInspect.has_value());
            VERIFY_ARE_EQUAL(sourceInspect, targetInspect);
        }

        {
            auto result = RunWslc(std::format(L"image tag {} {}", AlpineImage.NameAndTag(), DebianTaggedImage.NameAndTag()));
            result.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 0});

            auto resultSourceInspect = RunWslc(std::format(L"image inspect {}", AlpineImage.NameAndTag()));
            resultSourceInspect.Verify({.Stderr = L"", .ExitCode = 0});
            auto sourceInspect = resultSourceInspect.Stdout;

            auto resultTargetInspect = RunWslc(std::format(L"image inspect {}", DebianTaggedImage.NameAndTag()));
            resultTargetInspect.Verify({.Stderr = L"", .ExitCode = 0});
            auto targetInspect = resultTargetInspect.Stdout;

            VERIFY_IS_TRUE(sourceInspect.has_value());
            VERIFY_IS_TRUE(targetInspect.has_value());
            VERIFY_ARE_EQUAL(sourceInspect, targetInspect);
        }
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Tag_DeleteSourceImage_TargetRemains)
    {
        auto result = RunWslc(std::format(L"image tag {} {}", DebianImage.NameAndTag(), DebianTaggedImage.NameAndTag()));
        result.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 0});

        TestImageRegistry::Instance().Delete(DebianImage);
        VerifyImageIsListed(DebianTaggedImage);
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Tag_DeleteTargetImage_SourceRemains)
    {
        auto result = RunWslc(std::format(L"image tag {} {}", DebianImage.NameAndTag(), DebianTaggedImage.NameAndTag()));
        result.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 0});

        TestImageRegistry::Instance().Delete(DebianTaggedImage);
        VerifyImageIsListed(DebianImage);
    }

private:
    const TestImage& DebianImage = DebianTestImage();
    const TestImage& AlpineImage = AlpineTestImage();
    const TestImage& InvalidImage = InvalidTestImage();
    const TestImage DebianTaggedImage{L"debian", L"e2e-new-tag"};
};
} // namespace WSLCE2ETests
