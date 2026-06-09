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

    WSLC_TEST_METHOD(WSLCE2E_Image_Prune_Filter_MalformedValue)
    {
        // Filter values must be of the form key=value; bare keys are rejected by the CLI.
        const auto result = RunWslc(L"image prune --filter label");
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = Localization::WSLCCLI_InvalidFilterError(L"label") + L"\r\n", .ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Prune_Filter_InvalidKey)
    {
        // Filter keys are validated by the Docker daemon, which rejects unknown keys.
        const auto result = RunWslc(L"image prune --filter color=blue");
        VERIFY_ARE_EQUAL(1, result.ExitCode);
        VERIFY_IS_TRUE(result.Stderr.has_value());
        VERIFY_ARE_NOT_EQUAL(std::wstring::npos, result.Stderr->find(L"invalid filter"));
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Prune_Filter_LabelPreservesDangling)
    {
        // Create a dangling debian image (same trick as WSLCE2E_Image_Prune_DanglingImage).
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

        // Prune only dangling images carrying a label the dangling image does NOT have.
        // Multiple --filter label= values are AND'd by the daemon; the dangling image
        // matches neither, so it must survive this prune.
        auto filteredPrune = RunWslc(L"image prune --filter label=wslc.test.never=present --filter label=wslc.test.also=missing");
        filteredPrune.Verify({.Stderr = L"", .ExitCode = 0});
        for (const auto& line : filteredPrune.GetStdoutLines())
        {
            VERIFY_IS_FALSE(
                line.find(L"Deleted:") != std::wstring::npos, L"Filtered prune should not have deleted the dangling image");
            VERIFY_IS_FALSE(
                line.find(L"Untagged:") != std::wstring::npos, L"Filtered prune should not have untagged the dangling image");
        }

        // A subsequent unfiltered prune should still find and remove the dangling image,
        // proving the filter — not the absence of dangling images — was the reason nothing
        // was pruned above.
        auto unfilteredPrune = RunWslc(L"image prune");
        unfilteredPrune.Verify({.Stderr = L"", .ExitCode = 0});
        bool foundDeleted = false;
        for (const auto& line : unfilteredPrune.GetStdoutLines())
        {
            if (line.find(L"Deleted:") != std::wstring::npos || line.find(L"Untagged:") != std::wstring::npos)
            {
                foundDeleted = true;
                break;
            }
        }
        VERIFY_IS_TRUE(foundDeleted, L"Expected the dangling image to be pruned by the unfiltered call");
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
                << L"  -a,--all     " << Localization::WSLCCLI_ImagePruneAllArgDescription() << L"\r\n"
                << L"  -f,--filter  " << Localization::WSLCCLI_FilterArgDescription() << L"\r\n"
                << L"  --session    " << Localization::WSLCCLI_SessionIdArgDescription() << L"\r\n"
                << L"  -?,--help    " << Localization::WSLCCLI_HelpArgDescription() << L"\r\n"
                << L"\r\n";
        return options.str();
    }
};
} // namespace WSLCE2ETests
