/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCE2ERegistryTests.cpp

Abstract:

    End-to-end tests for wslc registry login/logout auth flows against a local registry.

--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCExecutor.h"
#include "WSLCE2EHelpers.h"
#include "Argument.h"
#include <wslutil.h>

namespace WSLCE2ETests {
using namespace wsl::shared;
using namespace WEX::Logging;

namespace {

    constexpr auto c_username = "wslctest";
    constexpr auto c_password = "password";

    void VerifyAuthFailure(const WSLCExecutionResult& result)
    {
        VERIFY_ARE_EQUAL(1u, result.ExitCode.value_or(0));
        VERIFY_IS_TRUE(result.Stderr.has_value());
        VERIFY_IS_TRUE(result.Stderr->find(L"no basic auth credentials") != std::wstring::npos);
    }

} // namespace

class WSLCE2ERegistryTests
{
    WSLC_TEST_CLASS(WSLCE2ERegistryTests)

    wil::unique_mta_usage_cookie m_mtaCookie;

    TEST_CLASS_SETUP(TestClassSetup)
    {
        THROW_IF_FAILED(CoIncrementMTAUsage(&m_mtaCookie));
        return true;
    }

    WSLC_TEST_METHOD(WSLCE2E_Registry_LoginLogout_PushPull_AuthFlow)
    {
        const auto& debianImage = DebianTestImage();
        EnsureImageIsLoaded(debianImage);

        // Ensure the default elevated session exists before opening it via COM.
        RunWslcAndVerify(L"container list", {.Stderr = L"", .ExitCode = 0});

        auto session = OpenDefaultElevatedSession();

        {
            Log::Comment(L"Starting local registry with auth");
            auto [registryContainer, registryAddress] = StartLocalRegistry(*session, c_username, c_password, 15001);
            auto registryAddressW = string::MultiByteToWide(registryAddress);
            Log::Comment(std::format(L"Registry started at {}", registryAddressW).c_str());

            Log::Comment(L"Tagging image for registry");
            auto registryImageName = TagImageForRegistry(*session, "debian:latest", registryAddress);
            auto registryImageNameW = string::MultiByteToWide(registryImageName);
            Log::Comment(std::format(L"Tagged image: {}", registryImageNameW).c_str());

            auto cleanup = wil::scope_exit([&]() {
                RunWslc(std::format(L"image delete --force {}", registryImageNameW));
                RunWslc(std::format(L"logout {}", registryAddressW));
            });

            // Negative path before login: push and pull should fail.
            Log::Comment(L"Testing push without login");
            auto result = RunWslc(std::format(L"push {}", registryImageNameW));
            VerifyAuthFailure(result);

            Log::Comment(L"Deleting tagged image and testing pull without login");
            RunWslcAndVerify(std::format(L"image delete --force {}", registryImageNameW), {.ExitCode = 0});
            result = RunWslc(std::format(L"pull {}", registryImageNameW));
            VerifyAuthFailure(result);

            // Login and verify that saved credentials are used for push/pull.
            Log::Comment(L"Logging in");
            result = RunWslc(std::format(
                L"login -u {} -p {} {}", string::MultiByteToWide(c_username), string::MultiByteToWide(c_password), registryAddressW));
            result.Verify({.Stdout = Localization::WSLCCLI_LoginSucceeded() + L"\r\n", .Stderr = L"", .ExitCode = 0});

            Log::Comment(L"Re-tagging and pushing with auth");
            registryImageName = TagImageForRegistry(*session, "debian:latest", registryAddress);
            result = RunWslc(std::format(L"push {}", registryImageNameW));
            result.Verify({.ExitCode = 0});

            Log::Comment(L"Deleting and pulling with auth");
            RunWslcAndVerify(std::format(L"image delete --force {}", registryImageNameW), {.ExitCode = 0});
            result = RunWslc(std::format(L"pull {}", registryImageNameW));
            result.Verify({.Stderr = L"", .ExitCode = 0});

            // Logout and verify both pull and push fail again.
            Log::Comment(L"Logging out");
            result = RunWslc(std::format(L"logout {}", registryAddressW));
            result.Verify({.Stdout = Localization::WSLCCLI_LogoutSucceeded(registryAddressW) + L"\r\n", .Stderr = L"", .ExitCode = 0});

            Log::Comment(L"Verifying pull fails after logout");
            RunWslcAndVerify(std::format(L"image delete --force {}", registryImageNameW), {.ExitCode = 0});
            result = RunWslc(std::format(L"pull {}", registryImageNameW));
            VerifyAuthFailure(result);

            Log::Comment(L"Verifying push fails after logout");
            registryImageName = TagImageForRegistry(*session, "debian:latest", registryAddress);
            result = RunWslc(std::format(L"push {}", registryImageNameW));
            VerifyAuthFailure(result);

            // Negative path for logout command: second logout should fail.
            Log::Comment(L"Verifying second logout fails");
            result = RunWslc(std::format(L"logout {}", registryAddressW));
            VERIFY_ARE_EQUAL(1u, result.ExitCode.value_or(0));
            VERIFY_IS_TRUE(result.Stderr.has_value());
            VERIFY_IS_TRUE(result.Stderr->find(L"Not logged in to") != std::wstring::npos);
        }
    }

    WSLC_TEST_METHOD(WSLCE2E_Registry_Login_HelpCommand)
    {
        auto result = RunWslc(L"registry login --help");
        result.Verify({.Stdout = GetLoginHelpMessage(), .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Registry_Logout_HelpCommand)
    {
        auto result = RunWslc(L"registry logout --help");
        result.Verify({.Stdout = GetLogoutHelpMessage(), .Stderr = L"", .ExitCode = 0});
    }

private:
    std::wstring GetLoginHelpMessage() const
    {
        std::wstringstream output;
        output << GetWslcHeader() << GetLoginDescription() << GetLoginUsage() << GetLoginAvailableArguments() << GetLoginAvailableOptions();
        return output.str();
    }

    std::wstring GetLogoutHelpMessage() const
    {
        std::wstringstream output;
        output << GetWslcHeader() << GetLogoutDescription() << GetLogoutUsage() << GetLogoutAvailableArguments()
               << GetLogoutAvailableOptions();
        return output.str();
    }

    std::wstring GetLoginDescription() const
    {
        return Localization::WSLCCLI_LoginLongDesc() + L"\r\n\r\n";
    }

    std::wstring GetLogoutDescription() const
    {
        return Localization::WSLCCLI_LogoutLongDesc() + L"\r\n\r\n";
    }

    std::wstring GetLoginUsage() const
    {
        return L"Usage: wslc registry login [<options>] [<server>]\r\n\r\n";
    }

    std::wstring GetLogoutUsage() const
    {
        return L"Usage: wslc registry logout [<options>] [<server>]\r\n\r\n";
    }

    std::wstring GetLoginAvailableArguments() const
    {
        std::wstringstream args;
        args << Localization::WSLCCLI_AvailableArguments() << L"\r\n"
             << L"  server            " << Localization::WSLCCLI_LoginServerArgDescription() << L"\r\n"
             << L"\r\n";
        return args.str();
    }

    std::wstring GetLogoutAvailableArguments() const
    {
        std::wstringstream args;
        args << Localization::WSLCCLI_AvailableArguments() << L"\r\n"
             << L"  server     " << Localization::WSLCCLI_LoginServerArgDescription() << L"\r\n"
             << L"\r\n";
        return args.str();
    }

    std::wstring GetLoginAvailableOptions() const
    {
        std::wstringstream options;
        options << Localization::WSLCCLI_AvailableOptions() << L"\r\n"
                << L"  -p,--password     " << Localization::WSLCCLI_LoginPasswordArgDescription() << L"\r\n"
                << L"  --password-stdin  " << Localization::WSLCCLI_LoginPasswordStdinArgDescription() << L"\r\n"
                << L"  -u,--username     " << Localization::WSLCCLI_LoginUsernameArgDescription() << L"\r\n"
                << L"  --session         " << Localization::WSLCCLI_SessionIdArgDescription() << L"\r\n"
                << L"  -h,--help         " << Localization::WSLCCLI_HelpArgDescription() << L"\r\n"
                << L"\r\n";
        return options.str();
    }

    std::wstring GetLogoutAvailableOptions() const
    {
        std::wstringstream options;
        options << Localization::WSLCCLI_AvailableOptions() << L"\r\n"
                << L"  -h,--help  " << Localization::WSLCCLI_HelpArgDescription() << L"\r\n"
                << L"\r\n";
        return options.str();
    }
};
} // namespace WSLCE2ETests
