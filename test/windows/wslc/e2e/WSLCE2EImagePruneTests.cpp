/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCE2EImagePruneTests.cpp

Abstract:

    This file contains end-to-end tests for WSLC image prune command.
--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCExecutor.h"
#include "WSLCE2EHelpers.h"

namespace WSLCE2ETests {
using namespace wsl::shared;

class WSLCE2EImagePruneTests
{
    WSLC_TEST_CLASS(WSLCE2EImagePruneTests)

    TEST_CLASS_SETUP(ClassSetup)
    {
        EnsureImageIsLoaded(DebianImage);
        return true;
    }

    TEST_CLASS_CLEANUP(ClassCleanup)
    {
        EnsureImageIsLoaded(DebianImage);
        return true;
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Prune_HelpCommand)
    {
        const auto result = RunWslc(L"image prune --help");
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Prune_NoDanglingImages)
    {
        // Prune when no dangling images exist should succeed with zero reclaimed space
        const auto result = RunWslc(L"image prune");
        result.Verify({.Stderr = L"", .ExitCode = 0});

        VerifyStdoutContains(result, L"Total reclaimed space:");
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Prune_DanglingImage)
    {
        // Create a dangling image by overwriting its only tag with a different image.
        // 1. Tag debian as prune-target:v1
        // 2. Delete the original debian:latest tag so prune-target:v1 is the only reference
        // 3. Tag alpine as prune-target:v1, overwriting it — debian image is now dangling
        EnsureImageIsLoaded(AlpineImage);
        auto cleanup = wil::scope_exit([&]() {
            RunWslc(L"image prune");
            RunWslc(L"image delete prune-target:v1");
            EnsureImageIsDeleted(AlpineImage);
            EnsureImageIsLoaded(DebianImage);
        });

        RunWslc(std::format(L"image tag {} prune-target:v1", DebianImage.NameAndTag())).Verify({.Stderr = L"", .ExitCode = 0});
        RunWslc(std::format(L"image delete {}", DebianImage.NameAndTag())).Verify({.Stderr = L"", .ExitCode = 0});
        RunWslc(std::format(L"image tag {} prune-target:v1", AlpineImage.NameAndTag())).Verify({.Stderr = L"", .ExitCode = 0});

        // Now prune should remove the dangling (original debian) image
        const auto result = RunWslc(L"image prune");
        result.Verify({.Stderr = L"", .ExitCode = 0});

        bool foundDeleted = false;
        for (const auto& line : result.GetStdoutLines())
        {
            if (line.find(L"Deleted:") != std::wstring::npos || line.find(L"Untagged:") != std::wstring::npos)
            {
                foundDeleted = true;
                break;
            }
        }

        VERIFY_IS_TRUE(foundDeleted, L"Expected pruned image output");
        VerifyStdoutContains(result, L"Total reclaimed space:");

        // Verify alpine image is still present (prune should only remove dangling images)
        VerifyImageIsListed(AlpineImage);
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Prune_AllFlag)
    {
        auto cleanup = wil::scope_exit([&]() { EnsureImageIsLoaded(DebianImage); });

        // --all should prune unused images (not just dangling)
        const auto result = RunWslc(L"image prune --all");
        result.Verify({.Stderr = L"", .ExitCode = 0});

        VerifyStdoutContains(result, L"Total reclaimed space:");

        // Verify the image was actually pruned
        auto listResult = RunWslc(L"image list");
        listResult.Verify({.Stderr = L"", .ExitCode = 0});
        for (const auto& line : listResult.GetStdoutLines())
        {
            VERIFY_IS_FALSE(
                line.find(DebianImage.NameAndTag()) != std::wstring::npos,
                std::format(L"Image '{}' should have been pruned by --all", DebianImage.NameAndTag()).c_str());
        }
    }

private:
    const TestImage& DebianImage = DebianTestImage();
    const TestImage& AlpineImage = AlpineTestImage();

    static void VerifyStdoutContains(const WSLCExecutionResult& result, const std::wstring& substring)
    {
        for (const auto& line : result.GetStdoutLines())
        {
            if (line.find(substring) != std::wstring::npos)
            {
                return;
            }
        }

        VERIFY_FAIL(std::format(L"Expected stdout to contain '{}'", substring).c_str());
    }

    std::wstring GetHelpMessage() const
    {
        std::wstringstream output;
        output << GetWslcHeader()  //
               << GetDescription() //
               << GetUsage()       //
               << GetAvailableOptions();
        return output.str();
    }

    std::wstring GetDescription() const
    {
        return Localization::WSLCCLI_ImagePruneLongDesc() + L"\r\n\r\n";
    }

    std::wstring GetUsage() const
    {
        return L"Usage: wslc image prune [<options>]\r\n\r\n";
    }

    std::wstring GetAvailableOptions() const
    {
        std::wstringstream options;
        options << L"The following options are available:\r\n"
                << L"  -a,--all   " << Localization::WSLCCLI_ImagePruneAllArgDescription() << L"\r\n"
                << L"  --session  " << Localization::WSLCCLI_SessionIdArgDescription() << L"\r\n"
                << L"  -?,--help  " << Localization::WSLCCLI_HelpArgDescription() << L"\r\n"
                << L"\r\n";
        return options.str();
    }
};
} // namespace WSLCE2ETests
