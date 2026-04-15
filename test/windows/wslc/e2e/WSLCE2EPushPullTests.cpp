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

    wil::unique_mta_usage_cookie m_mtaCookie;

    TEST_CLASS_SETUP(TestClassSetup)
    {
        THROW_IF_FAILED(CoIncrementMTAUsage(&m_mtaCookie));
        return true;
    }

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
        // Ensure the default elevated session exists.
        RunWslcAndVerify(L"container list", {.Stderr = L"", .ExitCode = 0});

        const auto& debianImage = DebianTestImage();
        EnsureImageIsLoaded(debianImage);

        // Start a local registry without auth.
        auto session = OpenDefaultElevatedSession();

        {
            auto [registryContainer, registryAddress] = StartLocalRegistry(*session);

            // Ensure the registry container is cleaned up after the test.
            auto registryAddressW = string::MultiByteToWide(registryAddress);

            // Tag the image for the local registry.
            auto registryImage = TagImageForRegistry(L"debian:latest", registryAddressW);

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
                << L"  --session  " << Localization::WSLCCLI_SessionIdArgDescription() << L"\r\n"
                << L"  -h,--help  " << Localization::WSLCCLI_HelpArgDescription() << L"\r\n"
                << L"\r\n";
        return options.str();
    }
};
} // namespace WSLCE2ETests
