/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCE2EPushPullTests.cpp

Abstract:

    End-to-end tests for wslc image push and pull against a local registry.

--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCExecutor.h"
#include "WSLCE2EHelpers.h"
#include "Argument.h"

namespace WSLCE2ETests {
using namespace wsl::shared;

class WSLCE2EPushPullTests
{
    WSLC_TEST_CLASS(WSLCE2EPushPullTests)

    WSLC_TEST_METHOD(WSLCE2E_Image_Push_HelpCommand)
    {
        auto result = RunWslc(L"image push --help");
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_IS_FALSE(result.Stdout.value().empty());
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Push_RootAlias)
    {
        auto result = RunWslc(L"push --help");
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_IS_FALSE(result.Stdout.value().empty());
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Pull_HelpCommand)
    {
        auto result = RunWslc(L"image pull --help");
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_IS_FALSE(result.Stdout.value().empty());
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Pull_RootAlias)
    {
        auto result = RunWslc(L"pull --help");
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_IS_FALSE(result.Stdout.value().empty());
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_PushPull)
    {
        const auto& debianImage = DebianTestImage();
        EnsureImageIsLoaded(debianImage);

        // Start a local registry without auth.
        auto session = OpenDefaultElevatedSession();

        {
            auto [registryContainer, registryAddress] = StartLocalRegistry(*session, "", "", 15003);
            auto registryAddressW = string::MultiByteToWide(registryAddress);

            // Tag the image for the local registry.
            auto registryImage = TagImageForRegistry(debianImage.NameAndTag(), registryAddressW);

            auto tagCleanup = wil::scope_exit([&]() { RunWslc(std::format(L"image delete --force {}", registryImage)); });

            // Standalone push/pull send progress to stdout (Docker parity), leaving stderr empty.
            auto result = RunWslc(std::format(L"push {}", registryImage));
            result.Verify({.Stderr = L"", .ExitCode = 0});
            VERIFY_IS_TRUE(result.Stdout.has_value());
            VERIFY_IS_FALSE(result.Stdout->empty());

            // Delete the local copy and pull it back.
            RunWslcAndVerify(std::format(L"image delete --force {}", registryImage), {.ExitCode = 0});

            result = RunWslc(std::format(L"pull {}", registryImage));
            result.Verify({.Stderr = L"", .ExitCode = 0});
            VERIFY_IS_TRUE(result.Stdout.has_value());
            VERIFY_IS_FALSE(result.Stdout->empty());

            // Verify the image is now present.
            auto registryRepo = registryImage.substr(0, registryImage.rfind(L':'));
            result = RunWslc(L"image list --format json");
            result.Verify({.Stderr = L"", .ExitCode = 0});
            VERIFY_IS_TRUE(result.Stdout.has_value());
            VERIFY_IS_TRUE(result.Stdout->find(registryRepo) != std::wstring::npos);
        }
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Pull_QuietOption)
    {
        const auto& debianImage = DebianTestImage();
        EnsureImageIsLoaded(debianImage);

        auto session = OpenDefaultElevatedSession();

        {
            auto [registryContainer, registryAddress] = StartLocalRegistry(*session, "", "", 15004);
            auto registryAddressW = string::MultiByteToWide(registryAddress);

            // Tag and push the image so it can be pulled back from the registry.
            auto registryImage = TagImageForRegistry(debianImage.NameAndTag(), registryAddressW);
            auto tagCleanup = wil::scope_exit([&]() { RunWslc(std::format(L"image delete --force {}", registryImage)); });

            RunWslcAndVerify(std::format(L"push {}", registryImage), {.Stderr = L"", .ExitCode = 0});

            // Delete the local copy so the pull actually fetches from the registry.
            RunWslcAndVerify(std::format(L"image delete --force {}", registryImage), {.ExitCode = 0});

            // Quiet pull (Docker parity): progress is suppressed and stdout is exactly the resolved canonical
            // reference. The registry image is already fully-qualified, so it equals the printed reference.
            // GetStdoutOneLine() also asserts there is exactly one output line, proving progress was suppressed.
            auto result = RunWslc(std::format(L"pull --quiet {}", registryImage));
            result.Verify({.Stderr = L"", .ExitCode = 0});
            VERIFY_ARE_EQUAL(registryImage, result.GetStdoutOneLine());
        }
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Push_NonExistentImage)
    {
        auto result = RunWslc(L"push does-not-exist:latest");
        auto errorMessage = L"An image does not exist locally with the tag: does-not-exist\r\nError code: E_FAIL\r\n";
        result.Verify({.Stderr = errorMessage, .ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Pull_NonExistentImage)
    {
        auto result = RunWslc(L"pull does-not-exist:latest");
        auto errorMessage =
            L"pull access denied for does-not-exist, repository does not exist or may require 'docker login': denied: requested "
            L"access to the resource is denied\r\nError code: WSLC_E_IMAGE_NOT_FOUND\r\n";
        result.Verify({.Stdout = L"", .Stderr = errorMessage, .ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Pull_NameOnlyDefaultsTag)
    {
        auto result = RunWslc(L"pull does-not-exist");
        result.Verify({.ExitCode = 1});
        VERIFY_IS_TRUE(result.StdoutContainsLine(L"Using default tag: latest"));

        // Quiet mode suppresses the "Using default tag" line, leaving stdout empty on failure.
        RunWslcAndVerify(L"pull -q does-not-exist", {.Stdout = L"", .ExitCode = 1});
    }
};
} // namespace WSLCE2ETests
