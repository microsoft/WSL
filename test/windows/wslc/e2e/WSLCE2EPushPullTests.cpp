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
        result.Verify({.Stdout = GetPushHelpMessage(), .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Push_RootAlias)
    {
        auto result = RunWslc(L"push --help");
        result.Verify({.Stdout = GetPushRootAliasHelpMessage(), .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Pull_HelpCommand)
    {
        auto result = RunWslc(L"image pull --help");
        result.Verify({.Stdout = GetPullHelpMessage(), .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Pull_RootAlias)
    {
        auto result = RunWslc(L"pull --help");
        result.Verify({.Stdout = GetPullRootAliasHelpMessage(), .Stderr = L"", .ExitCode = 0});
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

            // Push should succeed.
            auto result = RunWslc(std::format(L"push {}", registryImage));
            result.Verify({.ExitCode = 0});

            // Delete the local copy and pull it back.
            RunWslcAndVerify(std::format(L"image delete --force {}", registryImage), {.ExitCode = 0});

            result = RunWslc(std::format(L"pull {}", registryImage));
            result.Verify({.Stderr = L"", .ExitCode = 0});

            // Verify the image is now present.
            result = RunWslc(L"image list -q");
            result.Verify({.Stderr = L"", .ExitCode = 0});
            VERIFY_IS_TRUE(result.Stdout.has_value());
            VERIFY_IS_TRUE(result.Stdout->find(registryImage) != std::wstring::npos);
        }
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Push_NonExistentImage)
    {
        auto result = RunWslc(L"push does-not-exist:latest");
        auto errorMessage = L"An image does not exist locally with the tag: does-not-exist\r\nError code: E_FAIL\r\n";
        result.Verify({.Stdout = L"", .Stderr = errorMessage, .ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Pull_NonExistentImage)
    {
        auto result = RunWslc(L"pull does-not-exist:latest");
        auto errorMessage =
            L"pull access denied for does-not-exist, repository does not exist or may require 'docker login': denied: requested "
            L"access to the resource is denied\r\nError code: WSLC_E_IMAGE_NOT_FOUND\r\n";
        result.Verify({.Stdout = L"", .Stderr = errorMessage, .ExitCode = 1});
    }

    // Verify that --scheme http enables push/pull to a non-loopback insecure HTTP registry,
    // and that the insecure configuration is correctly scoped to each individual operation
    // (i.e., does not leak into subsequent commands that omit --scheme).
    //
    // Why bridge networking is required:
    // Docker treats 127.0.0.1 (loopback) as implicitly insecure — pulls and pushes to
    // localhost registries succeed without any insecure-registry configuration. To properly
    // exercise the --scheme flag, the registry must be on a non-loopback IP, which bridge
    // networking provides (e.g. 172.17.0.x).
    WSLC_TEST_METHOD(WSLCE2E_Image_PushPull_SchemeHttp)
    {
        const auto& debianImage = DebianTestImage();
        EnsureImageIsLoaded(debianImage);

        auto session = OpenDefaultElevatedSession();

        {
            // Start an HTTP registry on bridge networking (non-loopback IP, no TLS).
            auto [registryContainer, registryAddress] =
                StartLocalRegistry(*session, "", "", 15010, L"", true /* useBridge */);
            auto registryAddressW = string::MultiByteToWide(registryAddress);
            auto registryImage = TagImageForRegistry(debianImage.NameAndTag(), registryAddressW);

            auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
                RunWslc(std::format(L"image delete --force {}", registryImage));
            });

            // Step 1: Push WITHOUT --scheme http should fail.
            // The registry is on a non-loopback IP serving plain HTTP, so dockerd refuses
            // the connection because it is not in the insecure-registries list.
            auto result = RunWslc(std::format(L"push {}", registryImage));
            VERIFY_ARE_EQUAL(1u, result.ExitCode.value_or(0));

            // Step 2: Push WITH --scheme http should succeed.
            // This writes the registry into daemon.json as an insecure-registry and
            // SIGHUPs dockerd to reload the config before the push.
            result = RunWslc(std::format(L"push --scheme http {}", registryImage));
            result.Verify({.ExitCode = 0});

            // Delete the local copy so we can test pull.
            RunWslcAndVerify(std::format(L"image delete --force {}", registryImage), {.ExitCode = 0});

            // Step 3: Pull WITHOUT --scheme http should fail.
            // This verifies per-operation scope: the insecure-registry entry from Step 2
            // was removed from daemon.json after that push completed. The scope guard
            // cleaned up and SIGHUPed dockerd, so the registry is no longer trusted.
            result = RunWslc(std::format(L"pull {}", registryImage));
            VERIFY_ARE_EQUAL(1u, result.ExitCode.value_or(0));

            // Step 4: Pull WITH --scheme http should succeed.
            result = RunWslc(std::format(L"pull --scheme http {}", registryImage));
            result.Verify({.Stderr = L"", .ExitCode = 0});
        }
    }

    // Verify that concurrent --scheme http operations to different registries do not
    // interfere with each other. The implementation uses reference-counted insecure-registry
    // entries in daemon.json — when two operations overlap, both registries are listed;
    // when one completes and cleans up, only its entry is removed.
    //
    // This test verifies sequential operations against two registries (true concurrency
    // is not easily testable via the CLI, but this confirms the ref-counted cleanup
    // correctly handles multiple distinct registries within one session).
    WSLC_TEST_METHOD(WSLCE2E_Image_PushPull_SchemeHttp_MultipleRegistries)
    {
        const auto& debianImage = DebianTestImage();
        EnsureImageIsLoaded(debianImage);

        auto session = OpenDefaultElevatedSession();

        {
            // Start two independent HTTP registries on bridge networking.
            auto [reg1Container, reg1Address] = StartLocalRegistry(*session, "", "", 15011, L"", true /* useBridge */);
            auto [reg2Container, reg2Address] = StartLocalRegistry(*session, "", "", 15012, L"", true /* useBridge */);

            auto reg1AddressW = string::MultiByteToWide(reg1Address);
            auto reg2AddressW = string::MultiByteToWide(reg2Address);
            auto image1 = TagImageForRegistry(debianImage.NameAndTag(), reg1AddressW);
            auto image2 = TagImageForRegistry(debianImage.NameAndTag(), reg2AddressW);

            auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
                RunWslc(std::format(L"image delete --force {}", image1));
                RunWslc(std::format(L"image delete --force {}", image2));
            });

            // Push to both registries with --scheme http.
            auto result = RunWslc(std::format(L"push --scheme http {}", image1));
            result.Verify({.ExitCode = 0});
            result = RunWslc(std::format(L"push --scheme http {}", image2));
            result.Verify({.ExitCode = 0});

            // Delete local copies.
            RunWslcAndVerify(std::format(L"image delete --force {}", image1), {.ExitCode = 0});
            RunWslcAndVerify(std::format(L"image delete --force {}", image2), {.ExitCode = 0});

            // Pull both with --scheme http — both should succeed independently.
            result = RunWslc(std::format(L"pull --scheme http {}", image1));
            result.Verify({.Stderr = L"", .ExitCode = 0});
            result = RunWslc(std::format(L"pull --scheme http {}", image2));
            result.Verify({.Stderr = L"", .ExitCode = 0});

            // Delete again and verify neither registry persists as insecure.
            RunWslcAndVerify(std::format(L"image delete --force {}", image1), {.ExitCode = 0});
            RunWslcAndVerify(std::format(L"image delete --force {}", image2), {.ExitCode = 0});

            // Without --scheme http, both should fail — confirms each operation's
            // scope guard independently cleaned up its insecure-registry entry.
            result = RunWslc(std::format(L"pull {}", image1));
            VERIFY_ARE_EQUAL(1u, result.ExitCode.value_or(0));
            result = RunWslc(std::format(L"pull {}", image2));
            VERIFY_ARE_EQUAL(1u, result.ExitCode.value_or(0));
        }
    }

private:
    std::wstring GetPushHelpMessage() const
    {
        std::wstringstream output;
        output << GetWslcHeader() << GetPushDescription() << GetPushUsage() << GetAvailableArguments() << GetAvailableOptions();
        return output.str();
    }

    std::wstring GetPushRootAliasHelpMessage() const
    {
        std::wstringstream output;
        output << GetWslcHeader() << GetPushDescription() << GetPushRootUsage() << GetAvailableArguments() << GetAvailableOptions();
        return output.str();
    }

    std::wstring GetPullHelpMessage() const
    {
        std::wstringstream output;
        output << GetWslcHeader() << GetPullDescription() << GetPullUsage() << GetAvailableArguments() << GetAvailableOptions();
        return output.str();
    }

    std::wstring GetPullRootAliasHelpMessage() const
    {
        std::wstringstream output;
        output << GetWslcHeader() << GetPullDescription() << GetPullRootUsage() << GetAvailableArguments() << GetAvailableOptions();
        return output.str();
    }

    std::wstring GetPushDescription() const
    {
        return Localization::WSLCCLI_ImagePushLongDesc() + L"\r\n\r\n";
    }

    std::wstring GetPullDescription() const
    {
        return Localization::WSLCCLI_ImagePullLongDesc() + L"\r\n\r\n";
    }

    std::wstring GetPushUsage() const
    {
        return L"Usage: wslc image push [<options>] <image>\r\n\r\n";
    }

    std::wstring GetPushRootUsage() const
    {
        return L"Usage: wslc push [<options>] <image>\r\n\r\n";
    }

    std::wstring GetPullUsage() const
    {
        return L"Usage: wslc image pull [<options>] <image>\r\n\r\n";
    }

    std::wstring GetPullRootUsage() const
    {
        return L"Usage: wslc pull [<options>] <image>\r\n\r\n";
    }

    std::wstring GetAvailableArguments() const
    {
        std::wstringstream args;
        args << Localization::WSLCCLI_AvailableArguments() << L"\r\n"
             << L"  image      " << Localization::WSLCCLI_ImageIdArgDescription() << L"\r\n"
             << L"\r\n";
        return args.str();
    }

    std::wstring GetAvailableOptions() const
    {
        std::wstringstream options;
        options << Localization::WSLCCLI_AvailableOptions() << L"\r\n"
                << L"  --scheme   " << Localization::WSLCCLI_SchemeArgDescription() << L"\r\n"
                << L"  -?,--help  " << Localization::WSLCCLI_HelpArgDescription() << L"\r\n"
                << L"\r\n";
        return options.str();
    }
};
} // namespace WSLCE2ETests
