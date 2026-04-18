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

        VERIFY_IS_TRUE(result.StdoutContainsLine(L"Total reclaimed space:"));
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Prune_DanglingImage)
    {
        // Create a dangling image by tagging and then deleting the original tag
        auto tagResult = RunWslc(std::format(L"image tag {} debian:prune-test", DebianImage.NameAndTag()));
        tagResult.Verify({.Stderr = L"", .ExitCode = 0});

        auto deleteResult = RunWslc(std::format(L"image delete {}", DebianImage.NameAndTag()));
        deleteResult.Verify({.Stderr = L"", .ExitCode = 0});

        // Now prune should remove the dangling image
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
        VERIFY_IS_TRUE(result.StdoutContainsLine(L"Total reclaimed space:"));

        // Verify the dangling image is no longer listed
        auto listResult = RunWslc(L"image list");
        listResult.Verify({.Stderr = L"", .ExitCode = 0});
        for (const auto& line : listResult.GetStdoutLines())
        {
            VERIFY_IS_FALSE(line.find(L"<none>") != std::wstring::npos, L"Dangling image should have been pruned");
        }

        // Cleanup: delete the test tag and reload original
        RunWslc(L"image delete debian:prune-test");
        EnsureImageIsLoaded(DebianImage);
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Prune_AllFlag)
    {
        // --all should prune unused images (not just dangling)
        const auto result = RunWslc(L"image prune --all");
        result.Verify({.Stderr = L"", .ExitCode = 0});

        VERIFY_IS_TRUE(result.StdoutContainsLine(L"Total reclaimed space:"));

        // Verify the image was actually pruned
        auto listResult = RunWslc(L"image list");
        listResult.Verify({.Stderr = L"", .ExitCode = 0});
        for (const auto& line : listResult.GetStdoutLines())
        {
            VERIFY_IS_FALSE(
                line.find(DebianImage.NameAndTag()) != std::wstring::npos,
                std::format(L"Image '{}' should have been pruned by --all", DebianImage.NameAndTag()).c_str());
        }

        // Reload images for subsequent tests
        EnsureImageIsLoaded(DebianImage);
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Prune_SpaceReclaimedOutput)
    {
        // Verify the output contains the expected format
        const auto result = RunWslc(L"image prune");
        result.Verify({.Stderr = L"", .ExitCode = 0});

        bool foundSpaceReclaimed = false;
        for (const auto& line : result.GetStdoutLines())
        {
            if (line.find(L"Total reclaimed space:") != std::wstring::npos && line.find(L"MB") != std::wstring::npos)
            {
                foundSpaceReclaimed = true;
                break;
            }
        }

        VERIFY_IS_TRUE(foundSpaceReclaimed, L"Expected 'Total reclaimed space: X.XX MB' in output");
    }

private:
    const TestImage& DebianImage = DebianTestImage();

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
                << L"  -a,--all    " << Localization::WSLCCLI_ImagePruneAllArgDescription() << L"\r\n"
                << L"  --session   " << Localization::WSLCCLI_SessionIdArgDescription() << L"\r\n"
                << L"  -h,--help   Shows help about the selected command\r\n"
                << L"\r\n";
        return options.str();
    }
};
} // namespace WSLCE2ETests
