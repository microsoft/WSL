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

class WSLCE2EImageTagTests
{
    WSL_TEST_CLASS(WSLCE2EImageTagTests)

    TEST_CLASS_SETUP(ClassSetup)
    {
        EnsureImageIsDeleted(DebianTaggedImage);
        EnsureImageIsLoaded(DebianImage);
        return true;
    }

    TEST_METHOD_SETUP(MethodSetup)
    {
        EnsureImageIsDeleted(DebianTaggedImage);
        return true;
    }

    TEST_CLASS_CLEANUP(ClassCleanup)
    {
        EnsureImageIsDeleted(DebianTaggedImage);
        EnsureImageIsDeleted(DebianImage);
        return true;
    }

    TEST_METHOD(WSLCE2E_Image_Tag_HelpCommand)
    {
        WSL2_TEST_ONLY();

        auto result = RunWslc(L"image tag --help");
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"", .ExitCode = 0});
    }

    TEST_METHOD(WSLCE2E_Image_Tag_MissingSourceAndTarget)
    {
        WSL2_TEST_ONLY();

        auto result = RunWslc(L"image tag");
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"Required argument not provided: 'source'\r\n", .ExitCode = 1});
    }

    TEST_METHOD(WSLCE2E_Image_Tag_MissingTarget)
    {
        WSL2_TEST_ONLY();

        auto result = RunWslc(std::format(L"image tag {}", DebianImage.NameAndTag()));
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"Required argument not provided: 'target'\r\n", .ExitCode = 1});
    }

    TEST_METHOD(WSLCE2E_Image_Tag_SourceImageNotFound)
    {
        WSL2_TEST_ONLY();

        auto result = RunWslc(std::format(L"image tag {} {}", InvalidImage.NameAndTag(), DebianTaggedImage.NameAndTag()));
        auto errorMessage = std::format(L"No such image: {}\r\nError code: WSLA_E_IMAGE_NOT_FOUND\r\n", InvalidImage.NameAndTag());
        result.Verify({.Stdout = L"", .Stderr = errorMessage, .ExitCode = 1});
    }

    TEST_METHOD(WSLCE2E_Image_Tag_Success)
    {
        WSL2_TEST_ONLY();

        auto result = RunWslc(std::format(L"image tag {} {}", DebianImage.NameAndTag(), DebianTaggedImage.NameAndTag()));
        result.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 0});

        VerifyImageIsListed(DebianImage);
        VerifyImageIsListed(DebianTaggedImage);

        auto resultSourceInspect = RunWslc(std::format(L"image inspect {}", DebianImage.NameAndTag()));
        resultSourceInspect.Verify({.Stderr = L"", .ExitCode = 0});
        auto sourceInspect = resultSourceInspect.GetStdoutOneLine();

        auto resultTargetInspect = RunWslc(std::format(L"image inspect {}", DebianTaggedImage.NameAndTag()));
        resultTargetInspect.Verify({.Stderr = L"", .ExitCode = 0});
        auto targetInspect = resultTargetInspect.GetStdoutOneLine();

        VERIFY_ARE_EQUAL(sourceInspect, targetInspect);
    }

private:
    const std::wstring WslcContainerName = L"wslc-test-container";
    const TestImage& DebianImage = DebianTestImage();
    const TestImage& InvalidImage = InvalidTestImage();
    const TestImage& DebianTaggedImage = TestImage{L"debian", L"e2e-new-tag"};

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
        options << L"The following options are available:\r\n"                //
                << L"  -h,--help  Shows help about the selected command\r\n" //
                << L"\r\n";
        return options.str();
    }
};
} // namespace WSLCE2ETests