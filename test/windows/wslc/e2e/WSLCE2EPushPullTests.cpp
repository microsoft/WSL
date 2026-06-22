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
            auto registryRepo = registryImage.substr(0, registryImage.rfind(L':'));
            result = RunWslc(L"image list --format json");
            result.Verify({.Stderr = L"", .ExitCode = 0});
            VERIFY_IS_TRUE(result.Stdout.has_value());
            VERIFY_IS_TRUE(result.Stdout->find(registryRepo) != std::wstring::npos);
        }
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Push_NonExistentImage)
    {
        auto result = RunWslc(L"push does-not-exist:latest");
        // docker: "An image does not exist locally with the tag: X" —
        // podman: "failed to find image X: image not known".
        // Verify image name in stderr.
        VERIFY_ARE_EQUAL(1, result.ExitCode.value_or(0));
        auto stderrText = result.Stderr.value_or(L"");
        VERIFY_IS_TRUE(stderrText.find(L"does-not-exist") != std::wstring::npos);
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Pull_NonExistentImage)
    {
        auto result = RunWslc(L"pull does-not-exist:latest");
        // docker: "pull access denied for X, repository does not exist..."
        // podman: {"message":"denied: requested access to the resource is denied"}
        // (pull error path doesn't extract .message yet — separate sub-bug;
        // for now substring-match on the "denied" keyword which is present
        // in both engines' wording.)
        VERIFY_ARE_EQUAL(1, result.ExitCode.value_or(0));
        auto stderrText = result.Stderr.value_or(L"");
        VERIFY_IS_TRUE(stderrText.find(L"denied") != std::wstring::npos);
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
                << L"  -?,--help  " << Localization::WSLCCLI_HelpArgDescription() << L"\r\n"
                << L"\r\n";
        return options.str();
    }
};
} // namespace WSLCE2ETests
