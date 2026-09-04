/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCE2EImagePullTests.cpp

Abstract:

    This file contains end-to-end tests for the WSLC image pull command.
--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCExecutor.h"
#include "WSLCE2EHelpers.h"
#include "TestImageRegistry.h"

namespace WSLCE2ETests {
using namespace wsl::shared;

class WSLCE2EImagePullTests
{
    WSLC_TEST_CLASS(WSLCE2EImagePullTests)

    WSLC_TEST_METHOD(WSLCE2E_Image_Pull_AllTagsListedInHelp)
    {
        const auto result = RunWslc(L"image pull --help");
        result.Verify({.Stderr = L"", .ExitCode = 0});

        VERIFY_IS_TRUE(result.StdoutContainsSubstring(L"--all-tags"));
        VERIFY_IS_TRUE(result.StdoutContainsSubstring(Localization::WSLCCLI_AllTagsArgDescription()));
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Pull_AllTagsRejectsTaggedReference)
    {
        const auto result = RunWslc(L"image pull debian:latest --all-tags");
        result.Verify({.Stdout = L"", .ExitCode = 1});
        VERIFY_IS_TRUE(result.StderrContainsSubstring(Localization::WSLCCLI_PullAllTagsWithTagError()));
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Pull_AllTagsRejectsDigestReference)
    {
        const auto result =
            RunWslc(L"image pull debian@sha256:0000000000000000000000000000000000000000000000000000000000000000 -a");
        result.Verify({.Stdout = L"", .ExitCode = 1});
        VERIFY_IS_TRUE(result.StderrContainsSubstring(Localization::WSLCCLI_PullAllTagsWithTagError()));
    }

    // Publishes one image under two tags in a single repository, drops the local copies, and verifies
    // that a single --all-tags pull brings every tag back.
    WSLC_TEST_METHOD(WSLCE2E_Image_Pull_AllTagsDownloadsEveryTag)
    {
        const auto& testImage = AlpineTestImage();
        TestImageRegistry::Instance().EnsureLoaded(testImage);

        auto session = OpenDefaultElevatedSession();

        {
            auto [registryContainer, registryAddress] = StartLocalRegistry(*session, "", "", 15006);

            const auto repository = std::format(L"{}/{}", string::MultiByteToWide(registryAddress), testImage.Name);
            const auto firstTag = std::format(L"{}:v1", repository);
            const auto secondTag = std::format(L"{}:v2", repository);

            auto cleanup = wil::scope_exit([&]() {
                RunWslc(std::format(L"image delete --force {}", firstTag));
                RunWslc(std::format(L"image delete --force {}", secondTag));
            });

            RunWslcAndVerify(std::format(L"image tag {} {}", testImage.NameAndTag(), firstTag), {.ExitCode = 0});
            RunWslcAndVerify(std::format(L"image tag {} {}", testImage.NameAndTag(), secondTag), {.ExitCode = 0});
            RunWslcAndVerify(std::format(L"push {}", firstTag), {.Stderr = L"", .ExitCode = 0});
            RunWslcAndVerify(std::format(L"push {}", secondTag), {.Stderr = L"", .ExitCode = 0});

            // Remove the local copies so the pull has to fetch both tags from the registry.
            RunWslcAndVerify(std::format(L"image delete --force {}", firstTag), {.ExitCode = 0});
            RunWslcAndVerify(std::format(L"image delete --force {}", secondTag), {.ExitCode = 0});

            // --all-tags resolves no single tag, so the repository is printed as the final line.
            auto result = RunWslc(std::format(L"pull --all-tags {}", repository));
            result.Verify({.Stderr = L"", .ExitCode = 0});
            VERIFY_IS_TRUE(result.StdoutContainsLine(repository));

            result = RunWslc(L"image list --format json");
            result.Verify({.Stderr = L"", .ExitCode = 0});

            const auto repositoryUtf8 = string::WideToMultiByte(repository);
            const auto listed = result.GetStdoutLines();
            const auto isListed = [&](const std::string& tag) {
                for (const auto& line : listed)
                {
                    const auto entry = nlohmann::json::parse(string::WideToMultiByte(line));
                    if (entry.value("Repository", std::string{}) == repositoryUtf8 && entry.value("Tag", std::string{}) == tag)
                    {
                        return true;
                    }
                }

                return false;
            };

            VERIFY_IS_TRUE(isListed("v1"));
            VERIFY_IS_TRUE(isListed("v2"));
        }
    }
};
} // namespace WSLCE2ETests
