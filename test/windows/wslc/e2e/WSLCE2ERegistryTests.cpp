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

    void VerifyLogoutSucceeds(const std::wstring& registryAddress)
    {
        auto result = RunWslc(std::format(L"logout {}", registryAddress));
        result.Verify({.Stdout = Localization::WSLCCLI_LogoutSucceeded(registryAddress) + L"\r\n", .Stderr = L"", .ExitCode = 0});
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
            auto [registryContainer, registryAddress] = StartLocalRegistry(*session, c_username, c_password, 15001);
            auto registryAddressW = string::MultiByteToWide(registryAddress);

            auto registryImageName = TagImageForRegistry(debianImage.NameAndTag(), registryAddressW);

            auto cleanup = wil::scope_exit([&]() {
                RunWslc(std::format(L"image delete --force {}", registryImageName));
                RunWslc(std::format(L"logout {}", registryAddressW));
            });

            // Negative path before login: push and pull should fail.
            auto result = RunWslc(std::format(L"push {}", registryImageName));
            VerifyAuthFailure(result);

            RunWslcAndVerify(std::format(L"image delete --force {}", registryImageName), {.ExitCode = 0});

            result = RunWslc(std::format(L"pull {}", registryImageName));
            VerifyAuthFailure(result);

            // Login and verify that saved credentials are used for push/pull.
            result = RunWslc(std::format(
                L"login -u {} -p {} {}", string::MultiByteToWide(c_username), string::MultiByteToWide(c_password), registryAddressW));
            result.Verify({.Stdout = Localization::WSLCCLI_LoginSucceeded() + L"\r\n", .Stderr = L"", .ExitCode = 0});

            registryImageName = TagImageForRegistry(L"debian:latest", registryAddressW);
            result = RunWslc(std::format(L"push {}", registryImageName));
            result.Verify({.ExitCode = 0});

            RunWslcAndVerify(std::format(L"image delete --force {}", registryImageName), {.ExitCode = 0});
            result = RunWslc(std::format(L"pull {}", registryImageName));
            result.Verify({.Stderr = L"", .ExitCode = 0});

            // Logout and verify both pull and push fail again.
            VerifyLogoutSucceeds(registryAddressW);

            RunWslcAndVerify(std::format(L"image delete --force {}", registryImageName), {.ExitCode = 0});
            result = RunWslc(std::format(L"pull {}", registryImageName));
            VerifyAuthFailure(result);

            registryImageName = TagImageForRegistry(L"debian:latest", registryAddressW);
            result = RunWslc(std::format(L"push {}", registryImageName));
            VerifyAuthFailure(result);

            // Negative path for logout command: second logout should fail.
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

    WSLC_TEST_METHOD(WSLCE2E_Registry_Login_PasswordAndStdinMutuallyExclusive)
    {
        auto result = RunWslc(L"login -u testuser -p testpass --password-stdin localhost:15099");
        VERIFY_ARE_EQUAL(1u, result.ExitCode.value_or(0));
        VERIFY_IS_TRUE(result.Stderr.has_value());
        VERIFY_IS_TRUE(result.Stderr->find(L"--password and --password-stdin are mutually exclusive") != std::wstring::npos);
    }

    WSLC_TEST_METHOD(WSLCE2E_Registry_Login_PasswordStdinRequiresUsername)
    {
        auto result = RunWslc(L"login --password-stdin localhost:15099");
        VERIFY_ARE_EQUAL(1u, result.ExitCode.value_or(0));
        VERIFY_IS_TRUE(result.Stderr.has_value());
        VERIFY_IS_TRUE(result.Stderr->find(L"Must provide --username with --password-stdin") != std::wstring::npos);
    }

    WSLC_TEST_METHOD(WSLCE2E_Registry_Login_InvalidCredentials)
    {
        auto session = OpenDefaultElevatedSession();

        {
            auto [registryContainer, registryAddress] = StartLocalRegistry(*session, c_username, c_password, 15003);
            auto registryAddressW = string::MultiByteToWide(registryAddress);

            // Login with wrong password should fail.
            {
                auto result = RunWslc(std::format(L"login -u {} -p wrongpassword {}", string::MultiByteToWide(c_username), registryAddressW));
                VERIFY_ARE_EQUAL(1u, result.ExitCode.value_or(0));
                VERIFY_IS_TRUE(result.Stderr.has_value());
                VERIFY_IS_TRUE(result.Stderr->find(L"401 Unauthorized") != std::wstring::npos);
            }

            // Login with wrong username should fail.
            {
                auto result = RunWslc(std::format(L"login -u wronguser -p {} {}", string::MultiByteToWide(c_password), registryAddressW));
                VERIFY_ARE_EQUAL(1u, result.ExitCode.value_or(0));
                VERIFY_IS_TRUE(result.Stderr.has_value());
                VERIFY_IS_TRUE(result.Stderr->find(L"401 Unauthorized") != std::wstring::npos);
            }

            // Login with correct credentials should still succeed after failed attempts.
            {
                auto result = RunWslc(std::format(
                    L"login -u {} -p {} {}", string::MultiByteToWide(c_username), string::MultiByteToWide(c_password), registryAddressW));
                result.Verify({.Stdout = Localization::WSLCCLI_LoginSucceeded() + L"\r\n", .Stderr = L"", .ExitCode = 0});

                VerifyLogoutSucceeds(registryAddressW);
            }
        }
    }

    WSLC_TEST_METHOD(WSLCE2E_Registry_Login_CredentialInputMethods)
    {
        auto session = OpenDefaultElevatedSession();

        {
            auto [registryContainer, registryAddress] = StartLocalRegistry(*session, c_username, c_password, 15002);
            auto registryAddressW = string::MultiByteToWide(registryAddress);
            auto usernameW = string::MultiByteToWide(c_username);
            auto passwordW = string::MultiByteToWide(c_password);

            // Login with -u and -p flags.
            {
                auto result = RunWslc(std::format(L"login -u {} -p {} {}", usernameW, passwordW, registryAddressW));
                result.Verify({.Stdout = Localization::WSLCCLI_LoginSucceeded() + L"\r\n", .Stderr = L"", .ExitCode = 0});

                VerifyLogoutSucceeds(registryAddressW);
            }

            // Login with -u and --password-stdin.
            {
                auto interactive = RunWslcInteractive(std::format(L"login -u {} --password-stdin {}", usernameW, registryAddressW));
                interactive.WriteLine(c_password);
                interactive.CloseStdin();
                auto exitCode = interactive.Wait();
                VERIFY_ARE_EQUAL(0, exitCode, L"Login with --password-stdin should succeed");

                VerifyLogoutSucceeds(registryAddressW);
            }

            // Login with interactive prompts (no flags).
            {
                auto interactive = RunWslcInteractive(std::format(L"login {}", registryAddressW));
                interactive.ExpectStderr("Username: ");
                interactive.WriteLine(c_username);
                interactive.ExpectStderr("Password: ");
                interactive.WriteLine(c_password);
                auto exitCode = interactive.Wait();
                VERIFY_ARE_EQUAL(0, exitCode, L"Interactive login should succeed");

                VerifyLogoutSucceeds(registryAddressW);
            }
        }
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
