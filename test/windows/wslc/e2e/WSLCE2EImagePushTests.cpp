/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCE2EImagePushTests.cpp

Abstract:

    This file contains end-to-end tests for the WSLC image push command.
--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCExecutor.h"
#include "WSLCE2EHelpers.h"
#include "TestImageRegistry.h"

namespace WSLCE2ETests {
using namespace wsl::shared;

class WSLCE2EImagePushTests
{
    WSLC_TEST_CLASS(WSLCE2EImagePushTests)

    WSLC_TEST_METHOD(WSLCE2E_Image_Push_AllTagsListedInHelp)
    {
        const auto result = RunWslc(L"image push --help");
        result.Verify({.Stderr = L"", .ExitCode = 0});

        VERIFY_IS_TRUE(result.StdoutContainsSubstring(L"--all-tags"));
        VERIFY_IS_TRUE(result.StdoutContainsSubstring(Localization::WSLCCLI_PushAllTagsArgDescription()));
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Push_AllTagsRejectsTaggedReference)
    {
        const auto result = RunWslc(L"image push debian:latest --all-tags");
        result.Verify({.Stdout = L"", .ExitCode = 1});
        VERIFY_IS_TRUE(result.StderrContainsSubstring(Localization::WSLCCLI_AllTagsWithTagError()));
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Push_AllTagsRejectsDigestReference)
    {
        const auto result =
            RunWslc(L"image push debian@sha256:0000000000000000000000000000000000000000000000000000000000000000 -a");
        result.Verify({.Stdout = L"", .ExitCode = 1});
        VERIFY_IS_TRUE(result.StderrContainsSubstring(Localization::WSLCCLI_AllTagsWithTagError()));
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Push_NameOnlyDefaultsTag)
    {
        const auto errorMessage = FormatErrorMessage(L"An image does not exist locally with the tag: does-not-exist", L"E_FAIL");

        const auto result = RunWslc(L"image push does-not-exist");
        result.Verify({.Stderr = errorMessage, .ExitCode = 1});
        VERIFY_IS_TRUE(result.StdoutContainsLine(L"Using default tag: latest"));

        // The root alias resolves to the same task.
        const auto rootResult = RunWslc(L"push does-not-exist");
        rootResult.Verify({.Stderr = errorMessage, .ExitCode = 1});
        VERIFY_IS_TRUE(rootResult.StdoutContainsLine(L"Using default tag: latest"));

        // A name-only reference fails exactly like the tag it defaults to.
        const auto taggedResult = RunWslc(L"image push does-not-exist:latest");
        taggedResult.Verify({.Stderr = errorMessage, .ExitCode = 1});
        VERIFY_IS_FALSE(taggedResult.StdoutContainsSubstring(L"Using default tag"));
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Push_AllTagsSkipsDefaultTag)
    {
        // --all-tags pushes every tag in the repository, so no tag is defaulted or reported.
        const auto result = RunWslc(L"image push does-not-exist --all-tags");
        result.Verify({.ExitCode = 1});
        VERIFY_IS_FALSE(result.StdoutContainsSubstring(L"Using default tag"));
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Push_AllTagsPushesEveryTag)
    {
        const auto& testImage = AlpineTestImage();
        TestImageRegistry::Instance().EnsureLoaded(testImage);

        auto session = OpenDefaultElevatedSession();

        {
            auto [registryContainer, registryAddress] = StartLocalRegistry(*session, "", "", 15006);
            const auto repository = std::format(L"{}/{}", string::MultiByteToWide(registryAddress), testImage.Name);
            const std::vector<std::wstring> tags = {L"latest", L"e2e-all-tags-first", L"e2e-all-tags-second"};

            auto tagCleanup = wil::scope_exit([&]() {
                for (const auto& tag : tags)
                {
                    RunWslc(std::format(L"image delete --force {}:{}", repository, tag));
                }
            });

            for (const auto& tag : tags)
            {
                RunWslcAndVerify(std::format(L"image tag {} {}:{}", testImage.NameAndTag(), repository, tag), {.ExitCode = 0});
            }

            // A repository reference without a tag pushes every local tag in that repository.
            RunWslcAndVerify(std::format(L"image push {} --all-tags", repository), {.Stderr = L"", .ExitCode = 0});

            // Drop the local copies so each pull has to fetch the tag back from the registry.
            for (const auto& tag : tags)
            {
                RunWslcAndVerify(std::format(L"image delete --force {}:{}", repository, tag), {.ExitCode = 0});
            }

            for (const auto& tag : tags)
            {
                RunWslcAndVerify(std::format(L"image pull {}:{}", repository, tag), {.Stderr = L"", .ExitCode = 0});
            }
        }
    }
};
} // namespace WSLCE2ETests
