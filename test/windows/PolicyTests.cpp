/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    PolicyTests.cpp

Abstract:

    This file contains test cases for WSL policies.

--*/

#include "precomp.h"
#include <fstream>
#include "Common.h"
#include "registry.hpp"
#include "wslpolicies.h"

using namespace wsl::windows::policies;
using namespace wsl::windows::common::registry;

class PolicyTest
{
    WSL_TEST_CLASS(PolicyTest)

    bool m_initialized = false;

    TEST_CLASS_SETUP(TestClassSetup)
    {
        const auto policies = OpenKey(HKEY_LOCAL_MACHINE, ROOT_POLICIES_KEY, KEY_CREATE_SUB_KEY, 0);
        VERIFY_IS_TRUE(!!policies);

        const auto wslPolicies = CreateKey(policies.get(), L"WSL");
        VERIFY_IS_TRUE(!!wslPolicies);

        VERIFY_ARE_EQUAL(LxsstuInitialize(FALSE), TRUE);
        m_initialized = true;
        return true;
    }

    TEST_CLASS_CLEANUP(TestClassCleanup)
    {
        if (m_initialized)
        {
            LxsstuUninitialize(FALSE);
        }

        return true;
    }

    static auto SetPolicy(LPCWSTR Name, DWORD Value)
    {
        return RegistryKeyChange(HKEY_LOCAL_MACHINE, c_registryKey, Name, Value);
    }

    // Writes the supplied entries under the WSLContainerRegistryAllowlist sub-key as REG_SZ
    // values named "AllowedRegistry1", "AllowedRegistry2", ... (matching what the GP editor
    // writes for the ADMX `<list valuePrefix="AllowedRegistry"/>` policy) and deletes the
    // sub-key when the returned scope exits.
    static auto SetRegistryAllowlist(std::initializer_list<std::wstring_view> entries)
    {
        const auto policies = OpenKey(HKEY_LOCAL_MACHINE, c_registryKey, KEY_ALL_ACCESS);

        // Drop any pre-existing sub-key so stale `AllowedRegistryN` values from a previous
        // (possibly interrupted) test run can't leak into this one.
        DeleteKey(policies.get(), c_wslContainerRegistryAllowlist);

        const auto subKey = CreateKey(policies.get(), c_wslContainerRegistryAllowlist);
        DWORD index = 1;
        for (const auto& entry : entries)
        {
            const auto name = std::format(L"AllowedRegistry{}", index++);
            const std::wstring data{entry};
            WriteString(subKey.get(), nullptr, name.c_str(), data.c_str());
        }
        return wil::scope_exit([] {
            try
            {
                const auto policies = OpenKey(HKEY_LOCAL_MACHINE, c_registryKey, KEY_ALL_ACCESS);
                DeleteKey(policies.get(), c_wslContainerRegistryAllowlist);
            }
            CATCH_LOG()
        });
    }

    static void ValidateWarnings(const std::wstring& expectedWarnings, bool pattern = false)
    {
        auto [output, warnings] = LxsstuLaunchWslAndCaptureOutput(L"echo ok");
        VERIFY_ARE_EQUAL(L"ok\n", output);

        if (pattern)
        {
            if (!PathMatchSpec(warnings.c_str(), expectedWarnings.c_str()))
            {
                LogError("Warning '%ls' didn't match pattern '%ls'", warnings.c_str(), expectedWarnings.c_str());
                VERIFY_FAIL();
            }
        }
        else
        {
            VERIFY_ARE_EQUAL(expectedWarnings, warnings);
        }
    };

    WSL2_TEST_METHOD(MountPolicyAllowed)
    {
        SKIP_TEST_ARM64();
        auto revert = SetPolicy(c_allowDiskMount, 1);
        ValidateOutput(
            L"--mount DoesNotExist",
            L"Failed to attach disk 'DoesNotExist' to WSL2: The system cannot find the file specified. \r\n"
            L"Error code: Wsl/Service/AttachDisk/MountDisk/HCS/ERROR_FILE_NOT_FOUND\r\n");
    }

    WSL2_TEST_METHOD(MountPolicyDisabled)
    {
        SKIP_TEST_ARM64();
        auto revert = SetPolicy(c_allowDiskMount, 0);
        ValidateOutput(
            L"--mount DoesNotExist",
            L"wsl.exe --mount is disabled by the computer policy.\r\nError code: Wsl/Service/WSL_E_DISK_MOUNT_DISABLED\r\n");
    }

    void ValidatePolicy(LPCWSTR Name, LPCWSTR Config, LPCWSTR ExpectedWarnings, const std::function<void(DWORD)>& Validate = [](auto) {})
    {
        WslConfigChange config(LxssGenerateTestConfig() + Config);

        // Validate behavior with policy allowed
        {
            auto revert = SetPolicy(Name, 1);
            WslShutdown();

            ValidateWarnings(L""); // Expect no warnings
            Validate(1);
        }

        // Validate behavior with policy disabled
        {
            auto revert = SetPolicy(Name, 0);
            WslShutdown();

            ValidateWarnings(ExpectedWarnings);
            Validate(0);
        }

        // Validate behavior with an invalid policy value
        {
            auto revert = SetPolicy(Name, 12);
            WslShutdown();

            ValidateWarnings(L"");
            Validate(12);
        }
    }

    WSL2_TEST_METHOD(KernelCommandLine)
    {
        auto validate = [](DWORD policyValue) {
            auto [commandLine, _] = LxsstuLaunchWslAndCaptureOutput(L"cat /proc/cmdline");

            if (policyValue == 0)
            {
                VERIFY_IS_FALSE(commandLine.find(L"dummy-cmd-arg") != std::wstring::npos);
            }
            else
            {
                VERIFY_IS_TRUE(commandLine.find(L"dummy-cmd-arg") != std::wstring::npos);
            }
        };

        ValidatePolicy(
            c_allowCustomKernelCommandLineUserSetting,
            L"kernelCommandLine=dummy-cmd-arg",
            L"wsl: The .wslconfig setting 'wsl2.kernelCommandLine' is disabled by the computer policy.\r\n",
            validate);
    }

    WSL2_TEST_METHOD(NestedVirtualization)
    {
        SKIP_TEST_ARM64();
        WINDOWS_11_TEST_ONLY();

        ValidatePolicy(
            c_allowNestedVirtualizationUserSetting,
            L"nestedVirtualization=true",
            L"wsl: The .wslconfig setting 'wsl2.nestedVirtualization' is disabled by the computer policy.\r\n");
    }

    WSL2_TEST_METHOD(KernelDebugging)
    {
        WINDOWS_11_TEST_ONLY();

        ValidatePolicy(
            c_allowKernelDebuggingUserSetting,
            L"kernelDebugPort=1234",
            L"wsl: The .wslconfig setting 'wsl2.kernelDebugPort' is disabled by the computer policy.\r\n");
    }

    WSL2_TEST_METHOD(CustomKernel)
    {
        const std::wstring wslConfigPath = wsl::windows::common::helpers::GetWslConfigPath();
        const std::wstring nonExistentFile = L"DoesNotExist";
        WslConfigChange config(LxssGenerateTestConfig({.kernel = nonExistentFile.c_str(), .kernelModules = nonExistentFile.c_str()}));

        {
            auto revert = SetPolicy(c_allowCustomKernelUserSetting, 1);
            WslShutdown();

            ValidateOutput(
                L"echo ok",
                std::format(
                    L"{}\r\nError code: Wsl/Service/CreateInstance/CreateVm/WSL_E_CUSTOM_KERNEL_NOT_FOUND\r\n",
                    wsl::shared::Localization::MessageCustomKernelNotFound(wslConfigPath, nonExistentFile)));
        }

        // Disable the custom kernel policy and validate that the expected warnings are shown.
        {
            auto revert = SetPolicy(c_allowCustomKernelUserSetting, 0);
            WslShutdown();

            const auto kernelWarning =
                std::format(L"wsl: {}\r\n", wsl::shared::Localization::MessageSettingOverriddenByPolicy(L"wsl2.kernel"));
            const auto modulesWarning =
                std::format(L"wsl: {}\r\n", wsl::shared::Localization::MessageSettingOverriddenByPolicy(L"wsl2.kernelModules"));

            ValidateWarnings(std::format(L"{}{}", kernelWarning, modulesWarning));

            config.Update(LxssGenerateTestConfig({.kernel = nonExistentFile.c_str()}));
            ValidateWarnings(kernelWarning);

            config.Update(LxssGenerateTestConfig({.kernelModules = nonExistentFile.c_str()}));
            ValidateWarnings(modulesWarning);
        }
    }

    WSL2_TEST_METHOD(CustomSystemDistro)
    {
        WslConfigChange config(LxssGenerateTestConfig() + L"systemDistro=DoesNotExist");
        const std::wstring wslConfigPath = wsl::windows::common::helpers::GetWslConfigPath();

        {
            auto revert = SetPolicy(c_allowCustomSystemDistroUserSetting, 1);
            WslShutdown();

            ValidateOutput(
                L"echo ok",
                L"The custom system distribution specified in " + wslConfigPath +
                    L" was not found or is not the correct format.\r\nError code: "
                    L"Wsl/Service/CreateInstance/CreateVm/WSL_E_CUSTOM_SYSTEM_DISTRO_ERROR\r\n");
        }

        {
            auto revert = SetPolicy(c_allowCustomSystemDistroUserSetting, 0);
            WslShutdown();

            ValidateWarnings(L"wsl: The .wslconfig setting 'wsl2.systemDistro' is disabled by the computer policy.\r\n");
        }
    }

    WSL2_TEST_METHOD(CustomNetworkingMode)
    {
        WslConfigChange config(LxssGenerateTestConfig({.networkingMode = wsl::core::NetworkingMode::VirtioProxy}));

        {
            auto revert = SetPolicy(c_allowCustomNetworkingModeUserSetting, 1);
            WslShutdown();

            ValidateWarnings(L"");
        }

        {
            auto revertCustomMode = SetPolicy(c_allowCustomNetworkingModeUserSetting, 0);
            WslShutdown();

            ValidateWarnings(L"wsl: The .wslconfig setting 'wsl2.networkingMode' is disabled by the computer policy.\r\n");

            // Validate that no warnings are shown for NAT or None
            config.Update(LxssGenerateTestConfig({.networkingMode = wsl::core::NetworkingMode::Nat}));
            ValidateWarnings(L"");

            config.Update(LxssGenerateTestConfig({.networkingMode = wsl::core::NetworkingMode::None}));
            ValidateWarnings(L"");

            // Validate that no warnings are shown if the default networking mode is set to the same value as .wslconfig.
            auto revertDefault = SetPolicy(c_defaultNetworkingMode, static_cast<DWORD>(wsl::core::NetworkingMode::VirtioProxy));
            config.Update(LxssGenerateTestConfig({.networkingMode = wsl::core::NetworkingMode::VirtioProxy}));
            ValidateWarnings(L"");
        }
    }

    WSL2_TEST_METHOD(DebugShell)
    {
        auto revert = SetPolicy(c_allowDebugShellUserSetting, 0);
        WslShutdown();

        // Only testing the negative case since the debug shell is difficult to programmatically exit.

        WslKeepAlive keepAlive;
        ValidateOutput(L"--debug-shell", L"The debug shell is disabled by the computer policy.\r\n", L"", 1);
    }

    TEST_METHOD(WSL1)
    {
        // Test policy registry key with allow key explicitly set.
        {
            auto revert = SetPolicy(c_allowWSL1, 1);
            WslShutdown();

            ValidateWarnings(L"");
        }

        // Disable WSL1.
        {
            auto revert = SetPolicy(c_allowWSL1, 0);
            WslShutdown();

            // If running as WSL2, attempt to convert the distro to WSL1. If running as WSL1, attempt to run a command.
            if (LxsstuVmMode())
            {
                ValidateOutput(
                    L"--set-version " LXSS_DISTRO_NAME_TEST_L L" 1",
                    L"WSL1 is disabled by the computer policy.\r\nError code: Wsl/Service/WSL_E_WSL1_DISABLED\r\n");
            }
            else
            {
                ValidateOutput(
                L"echo ok",
                L"WSL1 is disabled by the computer policy.\r\nPlease run 'wsl.exe --set-version " LXSS_DISTRO_NAME_TEST_L L" 2' to upgrade to WSL2.\r\nError code: Wsl/Service/CreateInstance/WSL_E_WSL1_DISABLED\r\n");
            }
        }
    }

    TEST_METHOD(DisableWsl)
    {
        // N.B. Modifying one of the policy registry keys triggers a registry watcher in the service.
        //      Retry for up to 30 seconds to ensure the registry watcher has time to take effect.
        auto createInstance = [&](HRESULT expectedResult) {
            HRESULT result;
            const auto stop = std::chrono::steady_clock::now() + std::chrono::seconds{30};
            for (;;)
            {
                wil::com_ptr<ILxssUserSession> session;
                result = CoCreateInstance(CLSID_LxssUserSession, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&session));
                if (result == expectedResult || std::chrono::steady_clock::now() > stop)
                {
                    break;
                }

                std::this_thread::sleep_for(std::chrono::milliseconds{250});
            }

            VERIFY_ARE_EQUAL(expectedResult, result);
            if (SUCCEEDED(result))
            {
                VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"/bin/true"), 0u);
            }
            else
            {
                auto [output, _] = LxsstuLaunchWslAndCaptureOutput(L"/bin/true", -1);
                VERIFY_ARE_EQUAL(
                    output,
                    L"This program is blocked by group policy. For more information, contact your system administrator. "
                    L"\r\nError "
                    L"code: Wsl/ERROR_ACCESS_DISABLED_BY_POLICY\r\n");
            }
        };

        // Set the policy registry key and validate that user session creation returns the expected result,
        // then delete the key and ensure user session can be created.
        auto testPolicy = [&](LPCWSTR policy, HRESULT expectedResult, bool restartService) {
            {
                auto revert = SetPolicy(policy, 0);
                if (restartService)
                {
                    RestartWslService();
                }

                createInstance(expectedResult);
            }

            if (restartService)
            {
                RestartWslService();
            }
            createInstance(S_OK);
        };

        for (const auto restartService : {false, true})
        {
            // Ensure the top-level disable WSL policy works.
            testPolicy(wsl::windows::policies::c_allowWSL, HRESULT_FROM_WIN32(ERROR_ACCESS_DISABLED_BY_POLICY), restartService);

            // Verify the disable inbox WSL policy does not block lifted.
            testPolicy(wsl::windows::policies::c_allowInboxWSL, S_OK, restartService);
        }

        // Delete and recreate the key without restarting the service to ensure the registry watcher continues to work.
        wsl::windows::common::registry::DeleteKey(HKEY_LOCAL_MACHINE, wsl::windows::policies::c_registryKey);
        auto key = wsl::windows::common::registry::CreateKey(HKEY_LOCAL_MACHINE, wsl::windows::policies::c_registryKey);
        testPolicy(wsl::windows::policies::c_allowWSL, HRESULT_FROM_WIN32(ERROR_ACCESS_DISABLED_BY_POLICY), false);
    }

    WSL2_TEST_METHOD(DefaultNetworkingMode)
    {
        WslConfigChange config(LxssGenerateTestConfig());

        {
            auto revert = SetPolicy(c_defaultNetworkingMode, static_cast<DWORD>(wsl::core::NetworkingMode::None));
            WslShutdown();

            VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"wslinfo --networking-mode | grep -iF 'none'"), 0u);
        }

        {
            auto revert = SetPolicy(c_defaultNetworkingMode, static_cast<DWORD>(wsl::core::NetworkingMode::VirtioProxy));
            WslShutdown();

            VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"wslinfo --networking-mode | grep -iF 'virtioproxy'"), 0u);
        }
    }

    // Build the absolute path to the installed wslc.exe.
    static std::wstring GetWslcExePath()
    {
        auto msiPath = wsl::windows::common::wslutil::GetMsiPackagePath();
        THROW_HR_IF_MSG(E_UNEXPECTED, !msiPath.has_value(), "MSI install location not found in registry; is WSL installed?");
        return (std::filesystem::path(*msiPath) / L"wslc.exe").wstring();
    }

    // Verifies AllowWSLContainer=0 gates the WSLCSessionManager COM factory itself, so that
    // every method (including GetVersion) is unreachable when the policy disables containers.
    WSLC_TEST_METHOD(WSLContainerDisabled)
    {
        auto revert = SetPolicy(c_allowWSLContainer, 0);

        wil::com_ptr<IWSLCSessionManager> sessionManager;
        HRESULT hr = CoCreateInstance(__uuidof(WSLCSessionManager), nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&sessionManager));
        VERIFY_ARE_EQUAL(WSLC_E_CONTAINER_DISABLED, hr);
        VERIFY_IS_NULL(sessionManager.get());
    }

    // Verifies AllowWSLContainer=0 gates wslc.exe at startup with a friendly message that is
    // surfaced on stderr (and not stdout). Locks down both the exact rendered text and the
    // handle the message is written to so future regressions show up here.
    WSLC_TEST_METHOD(WSLContainerDisabledCli)
    {
        auto revert = SetPolicy(c_allowWSLContainer, 0);

        std::wstring cmd = L"\"" + GetWslcExePath() + L"\" container ls";
        auto [stdoutText, stderrText, exitCode] = LxsstuLaunchCommandAndCaptureOutputWithResult(cmd.data(), nullptr, nullptr);

        VERIFY_ARE_EQUAL(1, exitCode);

        // The disabled message must go to stderr only -- never to stdout.
        VERIFY_ARE_EQUAL(L"", stdoutText);

        // The wslc CLI renders failures via MessageErrorCode("{}\nError code: {}") and
        // PrintMessage adds a trailing newline; line endings are \r\n through console pipes.
        const auto expected =
            wsl::shared::Localization::MessageWSLContainerDisabled() + L"\r\nError code: WSLC_E_CONTAINER_DISABLED\r\n";
        VERIFY_ARE_EQUAL(expected, stderrText);
    }

    // Verifies the WSLContainerRegistryAllowlist denies image pulls from registries not in the
    // allowlist.
    WSLC_TEST_METHOD(RegistryAllowlistDenies)
    {
        // Allowlist contains ONLY mcr.microsoft.com -- pulling docker.io must be denied.
        auto revert = SetRegistryAllowlist({L"mcr.microsoft.com"});

        std::wstring cmd = L"\"" + GetWslcExePath() + L"\" image pull alpine:latest";
        auto [stdoutText, stderrText, exitCode] = LxsstuLaunchCommandAndCaptureOutputWithResult(cmd.data(), nullptr, nullptr);

        VERIFY_ARE_NOT_EQUAL(0, exitCode);
        const std::wstring combined = stdoutText + stderrText;
        if (combined.find(L"docker.io") == std::wstring::npos || combined.find(L"blocked by the computer policy") == std::wstring::npos)
        {
            LogError(
                "Expected blocked-by-policy for docker.io when allowlist is mcr.microsoft.com, got stdout: '%ls' stderr: '%ls'",
                stdoutText.c_str(),
                stderrText.c_str());
            VERIFY_FAIL();
        }
    }

    // Verifies that `wslc image build` is rejected outright when an allowlist is configured,
    // since the in-VM docker daemon would fetch FROM base images directly and bypass the
    // per-pull registry gate.
    WSLC_TEST_METHOD(RegistryAllowlistRejectsImageBuild)
    {
        auto revert = SetRegistryAllowlist({L"mcr.microsoft.com"});

        // Set up a minimal build context with a one-line Dockerfile in TEMP.
        const auto contextDir = std::filesystem::temp_directory_path() / L"wsl-policy-build-test";
        std::error_code ec;
        std::filesystem::remove_all(contextDir, ec);
        std::filesystem::create_directories(contextDir);
        auto cleanup = wil::scope_exit([&] { std::filesystem::remove_all(contextDir, ec); });

        {
            std::ofstream df(contextDir / L"Dockerfile");
            VERIFY_IS_TRUE(df.is_open());
            df << "FROM scratch\n";
        }

        std::wstring cmd = L"\"" + GetWslcExePath() + L"\" image build \"" + contextDir.wstring() + L"\"";
        auto [stdoutText, stderrText, exitCode] = LxsstuLaunchCommandAndCaptureOutputWithResult(cmd.data(), nullptr, nullptr);

        VERIFY_ARE_NOT_EQUAL(0, exitCode);
        const std::wstring combined = stdoutText + stderrText;
        if (combined.find(L"Building container images is blocked") == std::wstring::npos ||
            combined.find(L"computer policy") == std::wstring::npos)
        {
            LogError(
                "Expected image-build to be blocked by policy, got stdout: '%ls' stderr: '%ls'", stdoutText.c_str(), stderrText.c_str());
            VERIFY_FAIL();
        }
    }

    // Pure-function tests for the registry-allowlist policy evaluator. These don't talk to the
    // service, but do read/write the WSL policies registry key (created by TestClassSetup).
    TEST_METHOD(IsRegistryAllowed_Logic)
    {
        // No policy key configured -> always allowed.
        VERIFY_IS_TRUE(IsRegistryAllowed(nullptr, L"docker.io"));

        const auto policiesKey = OpenPoliciesKey();
        VERIFY_IS_TRUE(!!policiesKey);

        // No allowlist sub-key configured -> allowed.
        VERIFY_IS_TRUE(IsRegistryAllowed(policiesKey.get(), L"docker.io"));

        // Allowlist with multiple entries; matching is case-insensitive.
        {
            auto revert = SetRegistryAllowlist({L"mcr.microsoft.com", L"Docker.IO"});
            VERIFY_IS_TRUE(IsRegistryAllowed(policiesKey.get(), L"mcr.microsoft.com"));
            VERIFY_IS_TRUE(IsRegistryAllowed(policiesKey.get(), L"docker.io"));
            VERIFY_IS_TRUE(IsRegistryAllowed(policiesKey.get(), L"DOCKER.IO"));
            VERIFY_IS_TRUE(IsRegistryAllowed(policiesKey.get(), L"MCR.Microsoft.COM"));
            VERIFY_IS_FALSE(IsRegistryAllowed(policiesKey.get(), L"ghcr.io"));
        }

        // Sub-key present with no entries -> no effective restriction, every server allowed.
        {
            auto revert = SetRegistryAllowlist({});
            VERIFY_IS_TRUE(IsRegistryAllowed(policiesKey.get(), L"docker.io"));
            VERIFY_IS_TRUE(IsRegistryAllowed(policiesKey.get(), L"mcr.microsoft.com"));
        }

        // Sub-key present but only contains empty entries -> treated as no restriction, not
        // as a deny-all (defensive against stray GP editor list items).
        {
            auto revert = SetRegistryAllowlist({L"", L""});
            VERIFY_IS_TRUE(IsRegistryAllowed(policiesKey.get(), L"docker.io"));
            VERIFY_IS_TRUE(IsRegistryAllowed(policiesKey.get(), L"mcr.microsoft.com"));
        }
    }

    // Pure-function tests for HasRegistryAllowlist (used by `wslc image build` to decide whether
    // to refuse outright when the operation cannot be attributed to a single registry).
    TEST_METHOD(HasRegistryAllowlist_Logic)
    {
        VERIFY_IS_FALSE(HasRegistryAllowlist(nullptr));

        const auto policiesKey = OpenPoliciesKey();
        VERIFY_IS_TRUE(!!policiesKey);

        // No sub-key -> not configured.
        VERIFY_IS_FALSE(HasRegistryAllowlist(policiesKey.get()));

        // Sub-key present with no entries -> not effectively configured.
        {
            auto revert = SetRegistryAllowlist({});
            VERIFY_IS_FALSE(HasRegistryAllowlist(policiesKey.get()));
        }

        // Sub-key present with entries -> configured.
        {
            auto revert = SetRegistryAllowlist({L"mcr.microsoft.com"});
            VERIFY_IS_TRUE(HasRegistryAllowlist(policiesKey.get()));
        }
    }
};