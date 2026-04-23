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

namespace WSLCE2ETests {

using namespace wsl::shared::string;

class WSLCE2EImageTagTests
{
    WSL_TEST_CLASS(WSLCE2EImageTagTests)

    TEST_METHOD_SETUP(MethodSetup)
    {
        EnsureImageIsDeleted(DebianTaggedImage);
        EnsureImageIsLoaded(DebianImage);
        EnsureImageIsLoaded(AlpineImage);
        return true;
    }

    TEST_CLASS_CLEANUP(ClassCleanup)
    {
        EnsureImageIsDeleted(DebianTaggedImage);
        EnsureImageIsDeleted(DebianImage);
        EnsureImageIsDeleted(AlpineImage);
        return true;
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Tag_HelpCommand)
    {
        auto result = RunWslc(L"image tag --help");
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Tag_MissingSourceAndTarget)
    {
        auto result = RunWslc(L"image tag");
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"Required argument not provided: 'source'\r\n", .ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Tag_MissingTarget)
    {
        auto result = RunWslc(std::format(L"image tag {}", DebianImage.NameAndTag()));
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"Required argument not provided: 'target'\r\n", .ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Tag_SourceImageNotFound)
    {
        auto result = RunWslc(std::format(L"image tag {} {}", InvalidImage.NameAndTag(), DebianTaggedImage.NameAndTag()));
        auto errorMessage = std::format(L"No such image: {}\r\nError code: WSLC_E_IMAGE_NOT_FOUND\r\n", InvalidImage.NameAndTag());
        result.Verify({.Stdout = L"", .Stderr = errorMessage, .ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Tag_TargetImageWithDigest_Fail)
    {
        auto imageWithDigest = L"debian-mock:tag@sha256:11111111111111111111111111111111";
        auto result = RunWslc(std::format(L"image tag {} {}", DebianImage.NameAndTag(), imageWithDigest));
        auto errorMessage =
            std::format(L"Invalid image tag format: '{}'. Expected format is 'name:tag'\r\nError code: E_INVALIDARG\r\n", imageWithDigest);
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

        EnsureImageIsDeleted(DebianImage);
        VerifyImageIsListed(DebianTaggedImage);
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Tag_DeleteTargetImage_SourceRemains)
    {
        auto result = RunWslc(std::format(L"image tag {} {}", DebianImage.NameAndTag(), DebianTaggedImage.NameAndTag()));
        result.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 0});

        EnsureImageIsDeleted(DebianTaggedImage);
        VerifyImageIsListed(DebianImage);
    }

private:
    const TestImage& DebianImage = DebianTestImage();
    const TestImage& AlpineImage = AlpineTestImage();
    const TestImage& InvalidImage = InvalidTestImage();
    const TestImage DebianTaggedImage{L"debian", L"e2e-new-tag"};

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
        return wsl::shared::Localization::WSLCCLI_ImageTagLongDesc() + L"\r\n\r\n";
    }

    std::wstring GetUsage() const
    {
        return L"Usage: wslc image tag [<options>] <source> <target>\r\n\r\n";
    }

    std::wstring GetAvailableCommands() const
    {
        std::wstringstream commands;
        commands << L"The following arguments are available:\r\n"                                          //
                 << L"  source     Current or existing image reference in the image-name[:tag] format\r\n" //
                 << L"  target     New image reference in the image-name[:tag] format\r\n"                 //
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